#pragma once

#include "nix/expr/trace-result.hh"
#include "nix/expr/eval-trace-deps.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/sync.hh"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nix {
class EvalState;
}

namespace nix::eval_trace {

/** SQLite rowid for the Traces table. */
using TraceId = int64_t;

/** SQLite rowid for the Results table. */
using ResultId = int64_t;

/** SQLite rowid for the Strings table. */
using StringId = int64_t;

/** SQLite rowid for the AttrPaths table. */
using AttrPathId = int64_t;

/** SQLite rowid for the DepKeySets table. */
using DepKeySetId = int64_t;

/** SQLite-backed trace store for the eval trace system.
 *  Not thread-safe: must be used from a single thread. The session caches
 *  (verifiedTraceIds, traceDataCache, traceRowCache, currentDepHashes, etc.)
 *  are plain containers without synchronization. Only the SQLite connection
 *  (_state) uses Sync<> for locking. */
struct TraceStore {
    struct State {
        SQLite db;

        // Strings interning (UPSERT RETURNING)
        SQLiteStmt upsertString;
        SQLiteStmt getAllStrings;

        // AttrPaths interning (UPSERT RETURNING)
        SQLiteStmt upsertAttrPath;
        SQLiteStmt lookupAttrPathId;  // read-only lookup (no insert)

        // Results dedup (UPSERT RETURNING)
        SQLiteStmt upsertResult;
        SQLiteStmt getResult;

        // DepKeySets (content-addressed dep key storage, keyed by struct_hash)
        SQLiteStmt upsertDepKeySet;
        SQLiteStmt getDepKeySet;

        // Traces (references DepKeySets via dep_key_set_id FK, stores values_blob)
        SQLiteStmt upsertTrace;
        SQLiteStmt lookupTraceByFullHash;
        SQLiteStmt getTraceInfo;

        // CurrentTraces (current verified state per attribute)
        SQLiteStmt lookupAttr;
        SQLiteStmt upsertAttr;

        // TraceHistory (all historical traces for constructive recovery)
        SQLiteStmt insertHistory;
        SQLiteStmt lookupHistoryByTrace;
        SQLiteStmt scanHistoryForAttr;

        // StatHashCache (stat-hash cache persistence)
        SQLiteStmt queryAllStatHash;
        SQLiteStmt upsertStatHash;

        std::unique_ptr<SQLiteTxn> txn;
    };
    std::unique_ptr<Sync<State>> _state;
    SymbolTable & symbols;
    int64_t contextHash;

    // Interned dep key (string IDs, no hash value — used for keys_blob serialization)
    // Ordering is not required; these are serialized positionally in keys_blob
    // and indexed by DepKeySetId in the session cache.
    struct InternedDepKey {
        DepType type;
        uint32_t sourceId;
        uint32_t keyId;
    };

    // Interned dep entry (key + hash value — used for full dep reconstruction)
    struct InternedDep {
        DepType type;
        uint32_t sourceId;
        uint32_t keyId;
        DepHashValue hash;
    };

    struct TraceRow {
        TraceId traceId;
        ResultId resultId;
        ResultKind type;
        std::string value;
        std::string context;
    };

    // Per-trace session cache: hash fields populated lazily (by ensureTraceHashes,
    // loadFullTrace), deps populated on demand by loadFullTrace only.
    // Default-constructed entries have placeholder all-zero hashes (overwritten
    // before use). The hashesPopulated() check detects this sentinel state —
    // all-zero is never a valid BLAKE3 output (even BLAKE3("") is non-zero).
    struct CachedTraceData {
        Hash traceHash{HashAlgorithm::BLAKE3};
        Hash structHash{HashAlgorithm::BLAKE3};
        std::optional<std::vector<Dep>> deps;

        /** True if hash fields have been populated from DB (non-placeholder). */
        bool hashesPopulated() const {
            // Check traceHash — if it's populated, structHash was populated
            // at the same time (both are set together in ensureTraceHashes/loadFullTrace).
            for (size_t i = 0; i < traceHash.hashSize; i++)
                if (traceHash.hash[i] != 0) return true;
            return false;
        }
    };

    // Session caches — all unordered because we only need O(1) lookup by key,
    // not iteration in any particular order.
    std::unordered_set<TraceId> verifiedTraceIds;
    std::unordered_map<std::string, StringId> internedStrings;
    std::unordered_map<std::string, AttrPathId> internedAttrPaths;

    // Unified per-trace cache, replacing the old separate traceCache and
    // traceStructHashCache. Keyed by TraceId (DB rowid) — unique per trace.
    std::unordered_map<TraceId, CachedTraceData> traceDataCache;

    // lookupTraceRow cache: attrPath (canonical \0-separated) → TraceRow.
    // std::string correctly handles embedded \0 bytes in both hashing and comparison.
    // Avoids repeated CurrentTraces DB lookups for the same attr path.
    // Invalidated when CurrentTraces changes for a path (in recovery and record).
    std::unordered_map<std::string, TraceRow> traceRowCache;

    // DepKeySet session cache: maps DepKeySetId → resolved dep keys.
    // Keyed by integer ID (not hash) because we look up by ID after DB queries.
    // Avoids re-decompressing keys_blob when multiple traces share a key set.
    std::unordered_map<DepKeySetId, std::vector<InternedDepKey>> depKeySetCache;

    // Session string table (reverse: id -> string, for BLOB deserialization)
    std::unordered_map<StringId, std::string> stringTable;
    bool stringTableLoaded = false;
    void ensureStringTableLoaded();

    // Current dep hash cache (persists across verification → recovery within session).
    // Value is nullopt if the dep's resource is unavailable (e.g., file deleted);
    // this caches the failure to avoid re-attempting expensive hash computations.
    std::unordered_map<DepKey, std::optional<DepHashValue>, DepKey::Hash> currentDepHashes;

    TraceStore(SymbolTable & symbols, int64_t contextHash);
    ~TraceStore();

    struct VerifyResult {
        CachedResult value;
        TraceId traceId;
    };

    struct RecordResult {
        TraceId traceId;
    };

    /**
     * Verify a cached trace and return its stored result if valid.
     *
     * Implements a BSàlC verifying trace (VT) check: looks up the current trace
     * for this attribute, recomputes all dep hashes, and compares against stored
     * values. If all match, returns the constructive trace (CT) result directly
     * without re-evaluation. On verification failure, automatically attempts
     * constructive recovery before returning nullopt.
     */
    std::optional<VerifyResult> verify(
        std::string_view attrPath,
        const std::unordered_map<std::string, SourcePath> & inputAccessors,
        EvalState & state);

    /**
     * Record a fresh evaluation result with its dependencies (BSàlC constructive
     * trace recording).
     *
     * Stores (attrPath, result, deps) as a constructive trace: the full result
     * value is persisted alongside dep hashes, enabling future recovery without
     * re-evaluation. Each trace stores only its own deps — no parent dep
     * inheritance. If a trace with the same trace_hash already exists (dedup),
     * reuses it. Dep keys are content-addressed via DepKeySets table; hash
     * values are stored per-trace in values_blob.
     *
     * Also inserts into TraceHistory for constructive recovery: historical traces
     * can be recovered when dep hashes match a previously-seen state (e.g., after
     * a file revert).
     */
    RecordResult record(
        std::string_view attrPath,
        const CachedResult & value,
        const std::vector<Dep> & allDeps,
        bool isRoot);

    /**
     * Constructive trace recovery (BSàlC CT recovery).
     *
     * When verification fails (dep hashes changed), searches historical traces
     * for one whose deps match the current state. This is the key advantage of
     * a constructive trace over a verifying trace: stored results enable recovery
     * without re-evaluation.
     *
     * Two-strategy recovery:
     *   Direct hash recovery: compute trace_hash from current dep hashes, look
     *     up in Traces table. O(1). Handles file reverts and same-structure changes.
     *   Structural variant recovery: scan TraceHistory for entries with the same
     *     (context_hash, attr_path_id), group by dep_key_set_id, load key set once
     *     per group, recompute current dep hashes, retry direct hash lookup.
     *     O(V) where V = number of distinct dep structures. Zero values_blob
     *     decompression needed — only keys_blob is loaded. Handles dynamic dep
     *     instability (Shake-style: deps vary between evaluations).
     */
    [[gnu::cold]]
    std::optional<VerifyResult> recovery(
        TraceId oldTraceId,
        std::string_view attrPath,
        const std::unordered_map<std::string, SourcePath> & inputAccessors,
        EvalState & state);

    /**
     * Load the full dependency set for a trace.
     *
     * Reads the keys_blob from DepKeySets and values_blob from Traces via JOIN.
     * Zips the key set with positional hash values to reconstruct full deps.
     * Single DB round-trip + two zstd decompressions. O(D) in dep count.
     *
     * The returned vector is NOT sorted. Callers that need canonical ordering
     * must call sortAndDedupDeps() explicitly.
     */
    std::vector<Dep> loadFullTrace(TraceId traceId);

    /**
     * Load just the dep key set for a DepKeySets row (no hash values).
     *
     * Returns InternedDepKey entries (type + string IDs). Session-cached:
     * subsequent calls for the same depKeySetId return from depKeySetCache.
     * Used by structural variant recovery to avoid decompressing
     * values_blob when only dep keys are needed for hash recomputation.
     */
    std::vector<InternedDepKey> loadKeySet(DepKeySetId depKeySetId);

    bool attrExists(std::string_view attrPath);
    /** Get the current trace hash for an attr path (for ParentContext dep verification).
     *  Returns the trace_hash from the Traces table, which captures the full dep
     *  structure + hashes. Unlike a result hash (which for attrsets only captures
     *  attribute names), the trace hash changes when any dep value changes. */
    std::optional<Hash> getCurrentTraceHash(std::string_view attrPath);
    void clearSessionCaches();
    static std::string buildAttrPath(const std::vector<std::string> & components);

    // BLOB serialization for dep key sets (keys only, no hash values)
    static std::vector<uint8_t> serializeKeys(const std::vector<InternedDepKey> & keys);
    static std::vector<InternedDepKey> deserializeKeys(const void * blob, size_t size);

    // BLOB serialization for dep hash values (positional, matching key set order).
    // deserializeValues needs the key types to distinguish Blake3Hash (32 bytes)
    // from string values that happen to be 32 bytes (e.g., store paths).
    static std::vector<uint8_t> serializeValues(const std::vector<InternedDep> & deps);
    static std::vector<DepHashValue> deserializeValues(
        const void * blob, size_t size, const std::vector<InternedDepKey> & keys);

    // Full dep interning/resolution (combines key set + values)
    std::vector<Dep> resolveDeps(const std::vector<InternedDepKey> & keys,
                                  const std::vector<DepHashValue> & values);
    std::vector<InternedDep> internDeps(const std::vector<Dep> & deps);

    /**
     * Verify a single trace: recompute all dep hashes and compare against
     * stored expected values (BSàlC VT dep-hash verification).
     *
     * Returns true if every non-volatile dep's current hash matches its stored
     * hash. Volatile deps (CurrentTime, Exec) always fail.
     *
     * Always computes ALL dep hashes even on mismatch, populating
     * currentDepHashes for recovery(). With StatHashCache warm, computing
     * all hashes is cheap (~2ms for ~4K deps) and eliminates redundant
     * hash computation in the recovery path.
     *
     * Session-memoized: a trace verified once in this session is not re-verified
     * (tracked via verifiedTraceIds).
     */
    [[gnu::cold]]
    bool verifyTrace(
        TraceId traceId,
        const std::unordered_map<std::string, SourcePath> & inputAccessors,
        EvalState & state);

    std::tuple<ResultKind, std::string, std::string> encodeCachedResult(const CachedResult & value);

private:
    std::optional<TraceRow> lookupTraceRow(std::string_view attrPath);

    /** Ensure traceDataCache has traceHash + structHash for the given traceId.
     *  Queries getTraceInfo on cache miss. Returns null if trace not found. */
    CachedTraceData * ensureTraceHashes(TraceId traceId);

    StringId doInternString(std::string_view s);
    AttrPathId doInternAttrPath(std::string_view path);
    ResultId doInternResult(ResultKind type, const std::string & value,
                            const std::string & context, const Hash & resultHash);

    TraceId getOrCreateTrace(
        const Hash & traceHash,
        DepKeySetId depKeySetId,
        const std::vector<uint8_t> & valuesBlob);

    DepKeySetId getOrCreateDepKeySet(
        const Hash & structHash,
        const std::vector<uint8_t> & keysBlob);

    CachedResult decodeCachedResult(const TraceRow & row);

    Hash getTraceStructHash(TraceId traceId);
};

} // namespace nix::eval_trace
