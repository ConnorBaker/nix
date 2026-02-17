#pragma once

#include "nix/store/sqlite.hh"
#include "nix/store/path.hh"
#include "nix/store/store-dir-config.hh"
#include "nix/util/hash.hh"
#include "nix/util/sync.hh"

#include <optional>
#include <string>

namespace nix::eval_cache {

/**
 * Lightweight SQLite index mapping (context_hash, attr_path) to
 * trace_path for the store-based eval cache.
 *
 * This is NOT the cache itself — results live in the Nix store as
 * content-addressed Text blobs. This index is just a fast lookup to
 * avoid recomputing trace hashes on the warm path.
 *
 * If the index is lost (deleted, corrupted), the eval cache still works:
 * the recovery path recomputes trace hashes and checks the store.
 */
struct EvalIndexDb
{
    struct State
    {
        SQLite db;
        SQLiteStmt upsert;
        SQLiteStmt lookup;
        SQLiteStmt insertRecovery;
        SQLiteStmt lookupRecovery;
        SQLiteStmt upsertStructGroup;
        SQLiteStmt scanStructGroups;
        std::unique_ptr<SQLiteTxn> txn;
    };

    std::unique_ptr<Sync<State>> _state;

    EvalIndexDb();
    ~EvalIndexDb();

    struct IndexEntry {
        StorePath tracePath;
    };

    /**
     * Look up a cached eval trace by context hash and attr path.
     *
     * @param contextHash First 8 bytes of BLAKE3 context hash as int64.
     * @param attrPath Null-byte-separated attr path components.
     * @param store Store config for parsing store paths.
     * @return The cached entry, or nullopt if not found.
     */
    std::optional<IndexEntry> lookup(
        int64_t contextHash,
        std::string_view attrPath,
        const StoreDirConfig & store);

    /**
     * Insert or update a cached eval trace.
     *
     * @param contextHash First 8 bytes of BLAKE3 context hash as int64.
     * @param attrPath Null-byte-separated attr path components.
     * @param tracePath The content-addressed trace store path.
     * @param store Store config for printing store paths.
     */
    void upsert(
        int64_t contextHash,
        std::string_view attrPath,
        const StorePath & tracePath,
        const StoreDirConfig & store);

    /**
     * Record a dep hash → trace path mapping for recovery.
     *
     * When deps revert to a previously-seen state, recovery can find
     * the matching trace by computing the current dep hash and looking
     * it up here — without needing to know the result value.
     */
    void upsertRecovery(
        int64_t contextHash,
        std::string_view attrPath,
        const Hash & depHash,
        const StorePath & tracePath,
        const StoreDirConfig & store);

    /**
     * Look up a trace by dep content hash for recovery.
     */
    std::optional<StorePath> lookupByDepHash(
        int64_t contextHash,
        std::string_view attrPath,
        const Hash & depHash,
        const StoreDirConfig & store);

    // ── Struct groups (Phase 3 recovery) ─────────────────────────

    struct StructGroup {
        StorePath tracePath;
    };

    /**
     * Insert or update a structural group entry.
     * Maps (context_hash, attr_path, struct_hash) → trace_path.
     */
    void upsertStructGroup(
        int64_t contextHash,
        std::string_view attrPath,
        const Hash & structHash,
        const StorePath & tracePath,
        const StoreDirConfig & store);

    /**
     * Scan all struct groups for a given (context_hash, attr_path).
     * Returns representative traces for each unique dep structure.
     */
    std::vector<StructGroup> scanStructGroups(
        int64_t contextHash,
        std::string_view attrPath,
        const StoreDirConfig & store);

    // ── Batched cold-path writes ─────────────────────────────────

    /**
     * Batch all cold-path index writes under a single lock acquisition.
     * Performs: EvalIndex upsert, DepHashRecovery upsert (without parent),
     * DepHashRecovery upsert (with parent, if provided),
     * DepStructGroups upsert.
     */
    void coldStoreIndex(
        int64_t contextHash,
        std::string_view attrPath,
        const StorePath & tracePath,
        const Hash & depHash,
        const std::optional<std::pair<Hash, StorePath>> & parentDepHash,
        const Hash & structHash,
        const StoreDirConfig & store);
};

} // namespace nix::eval_cache
