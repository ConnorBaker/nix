#pragma once
///@file

#include "nix/expr/dep-tracker.hh"
#include "nix/util/file-system.hh"

#include <filesystem>
#include <optional>
#include <string>

namespace nix {

/**
 * Persistent cache mapping (path, stat_metadata, dep_type) → BLAKE3 hash.
 *
 * Avoids re-reading and re-hashing unchanged files across process invocations.
 * Uses a two-level architecture:
 *   L1: In-memory concurrent_flat_map (session-scoped, O(1) lookup)
 *   L2: SQLite database (persistent across processes, validated by stat metadata)
 *
 * Thread-safe: L1 is lock-free via concurrent_flat_map, L2 uses Sync<State>.
 *
 * The L1 key is (dev, ino, mtime_sec, mtime_nsec, size, depType) — no path.
 * This is correct because (dev, ino) uniquely identifies a file system object;
 * hard links sharing an inode correctly share the hash.
 */
class StatHashCache
{
public:
    static StatHashCache & instance();

    /**
     * Result of a cache lookup. Always includes the stat (unless lstat fails),
     * so callers can pass it to storeHash on miss without a redundant lstat.
     */
    struct LookupResult {
        std::optional<Blake3Hash> hash;
        std::optional<PosixStat> stat;
    };

    /**
     * Look up a cached hash for the given physical path and dep type.
     * The returned stat is populated even on cache miss (unless lstat fails),
     * allowing callers to pass it to storeHash without a redundant lstat.
     */
    LookupResult lookupHash(
        const std::filesystem::path & physPath, DepType type);

    /**
     * Store a computed hash with caller-provided stat metadata.
     * Avoids a redundant lstat when the caller already has stat info
     * (e.g., from a prior lookupHash call).
     */
    void storeHash(
        const std::filesystem::path & physPath, DepType type,
        const Blake3Hash & hash, const PosixStat & st);

    /**
     * Store a computed hash, lstating the path to get current metadata.
     * Use the stat-accepting overload when stat info is already available.
     */
    void storeHash(
        const std::filesystem::path & physPath, DepType type,
        const Blake3Hash & hash);

    /**
     * Clear the in-memory (L1) cache. Called from resetFileCache().
     */
    void clearMemoryCache();

    ~StatHashCache();

private:
    StatHashCache();

    /**
     * Bulk-load all L2 entries from SQLite into memory (once per session).
     */
    void ensureL2Loaded();

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace nix
