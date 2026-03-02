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
#include <functional>
#include <optional>
#include <string>

namespace nix {

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
};

/**
 * Cache key: (path, depType). One entry per unique (path, depType) pair.
 */
struct StatCacheKey {
    std::string path;
    DepType depType;

    bool operator==(const StatCacheKey &) const = default;

    struct Hash {
        std::size_t operator()(const StatCacheKey & k) const noexcept
        {
            return hashValues(k.path, std::to_underlying(k.depType));
        }
    };
};

/**
 * Cache entry: validated stat fingerprint + cached BLAKE3 hash.
 */
struct StatCacheEntry {
    FileFingerprint stat;
    Blake3Hash hash;
};

using StatCacheMap = boost::unordered_flat_map<StatCacheKey, StatCacheEntry, StatCacheKey::Hash>;

/**
 * Bulk-load entries from TraceStore's SQLite into the in-memory
 * StatHashStore. Called once during TraceStore construction.
 */
void loadStatHashStore(StatCacheMap entries);

/**
 * Iterate dirty (newly-stored) stat-hash entries for TraceStore to
 * flush back to its SQLite StatHashCache table during destruction.
 */
void forEachDirtyStatHashEntry(
    std::function<void(const StatCacheKey &, const StatCacheEntry &)> callback);

/**
 * Store a hash in the stat-hash store for the given path and dep type.
 * Performs an lstat if no stat metadata is provided. Best-effort: silently
 * returns if the path cannot be stat'd.
 */
void storeStatHash(
    const std::filesystem::path & physPath, DepType depType,
    const Blake3Hash & hash, std::optional<PosixStat> stat = std::nullopt);

/**
 * Clear the in-memory stat-hash store. Used by tests to force
 * re-hashing after modifying files.
 */
void clearStatHashStore();

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

} // namespace nix
