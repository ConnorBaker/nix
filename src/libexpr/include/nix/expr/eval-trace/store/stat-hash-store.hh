#pragma once
/// @file
/// StatHashStore: Persistent (path, depType) → (FileFingerprint, BLAKE3) cache.
/// Single-level cache keyed by (path, depType) with stat validation on lookup.
/// Process-global singleton, shared across all EvalState instances and threads.

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/file-system.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <sys/types.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace nix::eval_trace {

/**
 * Process-global singleton cache mapping (path, depType) → (stat fingerprint, BLAKE3 hash).
 * On lookup, validates the cached entry's stat metadata against a fresh lstat;
 * a cache hit means the file hasn't changed and the stored hash is still valid.
 */
struct StatHashStore
{
    /**
     * Stat metadata used for cache validation: if these fields match, the file
     * hasn't changed and the cached hash is still valid. Field types match
     * the POSIX stat struct.
     */
    // Fields ordered largest-alignment-first to minimize padding.
    struct FileFingerprint {
        ino_t ino;
        time_t mtime_sec;
        int64_t mtime_nsec;
        off_t size;
        dev_t dev;

        bool operator==(const FileFingerprint &) const = default;

        static FileFingerprint fromStat(const PosixStat & st)
        {
            auto [sec, nsec] = [&](auto & s) {
                if constexpr (requires { s.st_mtimespec; })
                    return std::pair{s.st_mtimespec.tv_sec, s.st_mtimespec.tv_nsec};
                else
                    return std::pair{s.st_mtim.tv_sec, s.st_mtim.tv_nsec};
            }(st);
            return {
                .ino = st.st_ino,
                .mtime_sec = sec,
                .mtime_nsec = nsec,
                .size = st.st_size,
                .dev = st.st_dev,
            };
        }
    };

    /**
     * Cache key: (path, depType). One entry per unique (path, depType) pair.
     */
    struct Key {
        std::string path;
        DepType depType;

        bool operator==(const Key &) const = default;

        friend std::size_t hash_value(const Key & k) noexcept
        {
            return hashValues(k.path, std::to_underlying(k.depType));
        }
    };

    /**
     * Cache value: validated stat fingerprint + cached BLAKE3 hash.
     */
    struct Value {
        FileFingerprint stat;
        Blake3Hash hash;
    };

    using Map = boost::unordered_flat_map<Key, Value>;

    struct LookupResult {
        PosixStat stat;
        std::optional<Blake3Hash> hash;
    };

    static StatHashStore & instance();

    std::optional<LookupResult> lookupHash(const std::filesystem::path & physPath, DepType type);

    void storeHash(
        const std::filesystem::path & physPath, DepType type,
        const Blake3Hash & hash, std::optional<PosixStat> st = std::nullopt);

    void clear();

    /**
     * Bulk-load entries into the in-memory cache.
     * Called by TraceStore after ATTACHing the DB.
     */
    void load(Map entries);

    /**
     * Extract dirty (newly-stored) entries for TraceStore to flush back
     * to its SQLite StatHashCache table during destruction.
     */
    Map takeDirty();

    /// Return the path to the stat-hash SQLite database file.
    /// Computed from the current cache directory (handles per-test overrides).
    std::filesystem::path getDbPath() const;

    /// Create the DB file and schema at getDbPath() (idempotent).
    /// Called by TraceStore before ATTACHing.
    void ensureSchema();

    /**
     * Stat-cached Content hash: looks up the physical file's stat metadata in
     * the persistent stat-hash store before falling back to depHash(readFile()).
     */
    Blake3Hash depHashFile(const SourcePath & path);

    /**
     * Stat-cached NARContent hash: like depHashPath() but checks stat store first.
     */
    Blake3Hash depHashPathCached(const SourcePath & path);

    /**
     * Stat-cached Directory hash: like depHashDirListing() but checks stat store
     * for the directory's own stat metadata first.
     */
    Blake3Hash depHashDirListingCached(const SourcePath & path, const SourceAccessor::DirEntries & entries);

private:
    Map cache;
    Map dirtyEntries;

    Blake3Hash cachedHash(const SourcePath & path, DepType depType, auto && computeHash);

    StatHashStore() = default;
    ~StatHashStore() = default;
};

} // namespace nix::eval_trace
