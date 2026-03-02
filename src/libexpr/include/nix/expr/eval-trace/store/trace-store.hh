#pragma once

#include "nix/expr/eval-trace/result.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/store/attr-vocab-store.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/sync.hh"
#include "nix/util/traced-data-ids.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace nix {
class EvalState;
struct InterningPools;
}

namespace nix::eval_trace {

// ── Strongly-typed IDs for trace store entities ──────────────────────
//
// Dense uint32_t IDs assigned in-memory (starting at 1). Correspond to
// SQLite INTEGER PRIMARY KEY rowids but are assigned by the application,
// not by SQLite autoincrement. Flushed to DB periodically via flush().

struct TraceIdTag {};
struct ResultIdTag {};
struct DepKeySetIdTag {};

/** ID for the Traces table (one per unique dep-structure + dep-values combination). */
using TraceId = StrongId<TraceIdTag, uint32_t>;

/** ID for the Results table (one per unique result content hash). */
using ResultId = StrongId<ResultIdTag, uint32_t>;

// StringId, AttrNameId, AttrPathId are defined in traced-data-ids.hh.

/** ID for the DepKeySets table (content-addressed dep key sets, keyed by struct_hash). */
using DepKeySetId = StrongId<DepKeySetIdTag, uint32_t>;

// ── Hash helpers for boost flat maps ─────────────────────────────────

/** Hash for nix::Hash keys (uses first 8 bytes of BLAKE3 — well-distributed). */
struct HashKeyHash {
    using is_avalanching = void;

    std::size_t operator()(const Hash & h) const noexcept {
        std::size_t result;
        std::memcpy(&result, h.hash, sizeof(result));
        return result;
    }
};

/** SQLite-backed trace store for the eval trace system.
 *  Not thread-safe: must be used from a single thread. The session caches
 *  (verifiedTraceIds, traceDataCache, traceRowCache, currentDepHashes, etc.)
 *  are plain containers without synchronization. Only the SQLite connection
 *  (_state) uses Sync<> for locking. */
struct TraceStore {
    struct State {
        SQLite db;

        // Strings — read-all for bulk load, explicit-ID insert for flush
        SQLiteStmt getAllStrings;
        SQLiteStmt insertStringWithId;

        // Results — read-all for bulk load, explicit-ID insert for flush
        SQLiteStmt getAllResults;
        SQLiteStmt insertResultWithId;
        SQLiteStmt getResult;

        // DepKeySets (content-addressed dep key storage, keyed by struct_hash)
        SQLiteStmt getAllDepKeySets;
        SQLiteStmt insertDepKeySetWithId;
        SQLiteStmt getDepKeySet;

        // Traces (references DepKeySets via dep_key_set_id FK, stores values_blob)
        SQLiteStmt getAllTraces;
        SQLiteStmt insertTraceWithId;
        SQLiteStmt lookupTraceByFullHash;
        SQLiteStmt getTraceInfo;

        // CurrentTraces (current verified state per attribute)
        SQLiteStmt lookupAttr;
        SQLiteStmt upsertAttr;

        // TraceHistory (all historical traces for constructive recovery)
        SQLiteStmt insertHistory;
        SQLiteStmt lookupHistoryByTrace;
        SQLiteStmt scanHistoryForAttr;

        // StatHashCache (stat-hash cache persistence, on stat_cache.* schema)
        SQLiteStmt queryAllStatHash;
        SQLiteStmt upsertStatHash;

        // Vocab (on vocab.* schema, ATTACH'd from attr-vocab.sqlite)
        SQLiteStmt insertVocabName;
        SQLiteStmt insertVocabPath;

        std::unique_ptr<SQLiteTxn> txn;
    };
    std::unique_ptr<Sync<State>> _state;
    SymbolTable & symbols;
    InterningPools & pools;
    AttrVocabStore & vocab;
    int64_t contextHash;

    // Interned dep key (string IDs, no hash value). Used for keys_blob serialization
    // (positionally in keys_blob, indexed by DepKeySetId in the session cache)
    // and as the key type for currentDepHashes (replacing string-based DepKey).
    //
    // StringId, DepSourceId, and DepKeyId all share the same StringInternTable
    // index space. We use StringId here for storage; internDeps() converts
    // DepSourceId/DepKeyId to StringId via raw value copy.
    struct InternedDepKey {
        DepType type;
        StringId sourceId;
        StringId keyId;

        bool operator==(const InternedDepKey &) const = default;

        struct Hash {
            using is_avalanching = void;
            std::size_t operator()(const InternedDepKey & k) const noexcept {
                return hashValues(std::to_underlying(k.type), k.sourceId.value, k.keyId.value);
            }
        };
    };

    // Interned dep entry (key + hash value — used for full dep reconstruction)
    struct InternedDep {
        InternedDepKey key;
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
        std::optional<std::vector<InternedDep>> deps;

        /** True if hash fields have been populated from DB (non-placeholder).
         *  Checks traceHash — if it's populated, structHash was populated
         *  at the same time (both are set together in ensureTraceHashes/loadFullTrace). */
        bool hashesPopulated() const {
            for (size_t i = 0; i < traceHash.hashSize; i++)
                if (traceHash.hash[i] != 0) return true;
            return false;
        }
    };

    // ── Session caches (boost flat containers for cache locality) ─────

    /// Trace IDs verified in this session (skip re-verification).
    boost::unordered_flat_set<TraceId, TraceId::Hash> verifiedTraceIds;

    /// Content-addressed dedup maps (in-memory, flushed to DB periodically)
    boost::unordered_flat_map<Hash, ResultId, HashKeyHash> resultByHash;
    boost::unordered_flat_map<Hash, DepKeySetId, HashKeyHash> depKeySetByStructHash;
    boost::unordered_flat_map<Hash, TraceId, HashKeyHash> traceByTraceHash;

    /// Unified per-trace cache, keyed by TraceId. Replaces the old separate
    /// traceCache and traceStructHashCache.
    boost::unordered_flat_map<TraceId, CachedTraceData, TraceId::Hash> traceDataCache;

    /// lookupTraceRow cache: AttrPathId → TraceRow (integer-keyed).
    /// Avoids repeated CurrentTraces DB lookups for the same attr path.
    /// Invalidated when CurrentTraces changes for a path (in recovery and record).
    boost::unordered_flat_map<AttrPathId, TraceRow, AttrPathId::Hash> traceRowCache;

    /// DepKeySet session cache: maps DepKeySetId → resolved dep keys.
    /// Keyed by integer ID (not hash) because we look up by ID after DB queries.
    /// Avoids re-decompressing keys_blob when multiple traces share a key set.
    boost::unordered_flat_map<DepKeySetId, std::vector<InternedDepKey>, DepKeySetId::Hash> depKeySetCache;

    /// Current dep hash cache (persists across verification → recovery within session).
    /// Value is nullopt if the dep's resource is unavailable (e.g., file deleted);
    /// this caches the failure to avoid re-attempting expensive hash computations.
    boost::unordered_flat_map<InternedDepKey, std::optional<DepHashValue>, InternedDepKey::Hash> currentDepHashes;

    // ── In-memory ID counters (next ID to assign = max(DB IDs) + 1) ──

    ResultId nextResultId;
    DepKeySetId nextDepKeySetId;
    TraceId nextTraceId;

    /// High-water mark: highest string ID flushed to DB.
    /// Strings with ID > flushedStringHighWaterMark are pending write.
    uint32_t flushedStringHighWaterMark = 0;

    // ── Pending writes (deferred to flush()) ─────────────────────────

    struct PendingResult {
        ResultId id;
        ResultKind type;
        std::string value;
        std::string context;
        Hash hash;
    };
    std::vector<PendingResult> pendingResults;

    struct PendingDepKeySet {
        DepKeySetId id;
        Hash structHash;
        std::vector<uint8_t> keysBlob;
    };
    std::vector<PendingDepKeySet> pendingDepKeySets;

    struct PendingTrace {
        TraceId id;
        Hash traceHash;
        DepKeySetId depKeySetId;
        std::vector<uint8_t> valuesBlob;
    };
    std::vector<PendingTrace> pendingTraces;

    // ── Lifecycle ────────────────────────────────────────────────────

    TraceStore(SymbolTable & symbols, InterningPools & pools, AttrVocabStore & vocab, int64_t contextHash);
    ~TraceStore();

    /** Bulk-load all interned entities from DB into in-memory maps.
     *  Called once from constructor after schema creation, and again
     *  from clearSessionCaches() to repopulate after a reset. */
    void bulkLoadAll();

    /** Flush all pending writes to SQLite in dependency order
     *  (Strings → Results → DepKeySets → Traces).
     *  Called from record() before upsertAttr/insertHistory, and
     *  from the destructor before committing the transaction. */
    void flush();

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
        AttrPathId pathId,
        const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
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
        AttrPathId pathId,
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
        AttrPathId pathId,
        const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
        EvalState & state);

    /**
     * Load the full dependency set for a trace.
     *
     * Reads the keys_blob from DepKeySets and values_blob from Traces via JOIN.
     * Zips the key set with positional hash values to reconstruct full deps.
     * Single DB round-trip + two zstd decompressions. O(D) in dep count.
     *
     * The returned vector is NOT sorted. Callers that need canonical ordering
     * must call sortAndDedupInterned() explicitly.
     */
    std::vector<InternedDep> loadFullTrace(TraceId traceId);

    /**
     * Load just the dep key set for a DepKeySets row (no hash values).
     *
     * Returns InternedDepKey entries (type + string IDs). Session-cached:
     * subsequent calls for the same depKeySetId return from depKeySetCache.
     * Used by structural variant recovery to avoid decompressing
     * values_blob when only dep keys are needed for hash recomputation.
     */
    std::vector<InternedDepKey> loadKeySet(DepKeySetId depKeySetId);

    bool attrExists(AttrPathId pathId);

    /** Get the current trace hash for an attr path (for ParentContext dep verification).
     *  Returns the trace_hash from the Traces table, which captures the full dep
     *  structure + hashes. Unlike a result hash (which for attrsets only captures
     *  attribute names), the trace hash changes when any dep value changes. */
    std::optional<Hash> getCurrentTraceHash(AttrPathId pathId);

    void clearSessionCaches();

    // ── BLOB serialization ───────────────────────────────────────────

    /// Serialize dep key set to packed 9-byte entries (type[1] + sourceId[4] + keyId[4]),
    /// zstd compressed. Stored in DepKeySets table, shared across traces with
    /// the same dep structure (same struct_hash).
    static std::vector<uint8_t> serializeKeys(const std::vector<InternedDepKey> & keys);
    static std::vector<InternedDepKey> deserializeKeys(const void * blob, size_t size);

    /// Serialize dep hash values to per-entry hashLen[1] + hashData[hashLen],
    /// zstd compressed. Stored in Traces table. Entries are positionally matched
    /// with keys_blob in the corresponding DepKeySets row.
    /// deserializeValues needs the key types to distinguish Blake3Hash (32 bytes)
    /// from string values that happen to be 32 bytes (e.g., store paths).
    static std::vector<uint8_t> serializeValues(const std::vector<InternedDep> & deps);
    static std::vector<DepHashValue> deserializeValues(
        const void * blob, size_t size, const std::vector<InternedDepKey> & keys);

    /**
     * Resolved dependency with owned strings. Used by verification code
     * (computeCurrentHash, resolveDepPath) and test assertions that need
     * human-readable source/key values.
     */
    struct ResolvedDep {
        std::string source;
        std::string key;
        DepHashValue expectedHash;
        DepType type;
    };

    /// Resolve a single InternedDep to a string-based ResolvedDep.
    ResolvedDep resolveDep(const InternedDep & idep);

    /// Resolve a StringId to its string value. O(1) lookup via pools.strings.
    /// Returns string_view into arena memory; lifetime tied to InterningPools.
    std::string_view resolveString(StringId id) const;

    /// Convert Deps to InternedDeps. DepSourceId/DepKeyId share the same
    /// index space as StringId (unified StringInternTable), so conversion is
    /// a raw value copy — no string lookup needed.
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
        const boost::unordered_flat_map<std::string, SourcePath> & inputAccessors,
        EvalState & state);

    std::tuple<ResultKind, std::string, std::string> encodeCachedResult(const CachedResult & value);

private:
    std::optional<TraceRow> lookupTraceRow(AttrPathId pathId);

    /** Ensure traceDataCache has traceHash + structHash for the given traceId.
     *  Queries getTraceInfo on cache miss. Returns null if trace not found. */
    CachedTraceData * ensureTraceHashes(TraceId traceId);

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
