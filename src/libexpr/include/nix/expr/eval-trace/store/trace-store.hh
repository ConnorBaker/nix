#pragma once

#include "nix/expr/eval-trace/result.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/sync.hh"
#include "nix/util/traced-data-ids.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
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
struct StringIdTag {};
struct AttrPathIdTag {};
struct DepKeySetIdTag {};

/** ID for the Traces table (one per unique dep-structure + dep-values combination). */
using TraceId = StrongId<TraceIdTag, uint32_t>;

/** ID for the Results table (one per unique result content hash). */
using ResultId = StrongId<ResultIdTag, uint32_t>;

/** ID for the Strings table (interned dep source names and dep key strings). */
using StringId = StrongId<StringIdTag, uint32_t>;

/** ID for the AttrPaths table (null-byte-separated attribute paths). */
using AttrPathId = StrongId<AttrPathIdTag, uint32_t>;

/** ID for the DepKeySets table (content-addressed dep key sets, keyed by struct_hash). */
using DepKeySetId = StrongId<DepKeySetIdTag, uint32_t>;

// ── Hash helpers for boost flat maps ─────────────────────────────────

/** Transparent string hash — avoids string_view → string copy on cache hit. */
struct TransparentStringHash {
    using is_transparent = void;
    using is_avalanching = void;

    std::size_t operator()(std::string_view sv) const noexcept {
        return boost::hash<std::string_view>{}(sv);
    }
};

/** Transparent string equality. */
struct TransparentStringEqual {
    using is_transparent = void;

    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

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

        // AttrPaths — read-all for bulk load, explicit-ID insert for flush
        SQLiteStmt getAllAttrPaths;
        SQLiteStmt insertAttrPathWithId;
        SQLiteStmt lookupAttrPathId;  // read-only lookup (no insert)

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

        // StatHashCache (stat-hash cache persistence)
        SQLiteStmt queryAllStatHash;
        SQLiteStmt upsertStatHash;

        std::unique_ptr<SQLiteTxn> txn;
    };
    std::unique_ptr<Sync<State>> _state;
    SymbolTable & symbols;
    InterningPools & pools;
    int64_t contextHash;

    // Interned dep key (string IDs, no hash value — used for keys_blob serialization).
    // Ordering is not required; entries are serialized positionally in keys_blob
    // and indexed by DepKeySetId in the session cache.
    struct InternedDepKey {
        DepType type;
        StringId sourceId;
        StringId keyId;
    };

    // Interned dep entry (key + hash value — used for full dep reconstruction)
    struct InternedDep {
        DepType type;
        StringId sourceId;
        StringId keyId;
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

    /// Forward maps: string → ID (transparent lookup avoids string_view copy)
    boost::unordered_flat_map<std::string, StringId,
        TransparentStringHash, TransparentStringEqual> internedStrings;
    boost::unordered_flat_map<std::string, AttrPathId,
        TransparentStringHash, TransparentStringEqual> internedAttrPaths;

    /// Content-addressed dedup maps (in-memory, flushed to DB periodically)
    boost::unordered_flat_map<Hash, ResultId, HashKeyHash> resultByHash;
    boost::unordered_flat_map<Hash, DepKeySetId, HashKeyHash> depKeySetByStructHash;
    boost::unordered_flat_map<Hash, TraceId, HashKeyHash> traceByTraceHash;

    /// Unified per-trace cache, keyed by TraceId. Replaces the old separate
    /// traceCache and traceStructHashCache.
    boost::unordered_flat_map<TraceId, CachedTraceData, TraceId::Hash> traceDataCache;

    /// lookupTraceRow cache: attrPath (canonical \0-separated) → TraceRow.
    /// std::string correctly handles embedded \0 bytes in both hashing and comparison.
    /// Avoids repeated CurrentTraces DB lookups for the same attr path.
    /// Invalidated when CurrentTraces changes for a path (in recovery and record).
    boost::unordered_flat_map<std::string, TraceRow,
        TransparentStringHash, TransparentStringEqual> traceRowCache;

    /// DepKeySet session cache: maps DepKeySetId → resolved dep keys.
    /// Keyed by integer ID (not hash) because we look up by ID after DB queries.
    /// Avoids re-decompressing keys_blob when multiple traces share a key set.
    boost::unordered_flat_map<DepKeySetId, std::vector<InternedDepKey>, DepKeySetId::Hash> depKeySetCache;

    /// Reverse map: ID → string (O(1) lookup by index, indexed by id.value-1).
    /// Populated by bulkLoadAll() at startup and by doInternString() for new strings.
    std::vector<std::string> stringTable;
    bool stringTableLoaded = false;
    void ensureStringTableLoaded();

    /// Current dep hash cache (persists across verification → recovery within session).
    /// Value is nullopt if the dep's resource is unavailable (e.g., file deleted);
    /// this caches the failure to avoid re-attempting expensive hash computations.
    boost::unordered_flat_map<DepKey, std::optional<DepHashValue>, DepKey::Hash> currentDepHashes;

    // ── In-memory ID counters (next ID to assign = max(DB IDs) + 1) ──

    StringId nextStringId;
    AttrPathId nextAttrPathId;
    ResultId nextResultId;
    DepKeySetId nextDepKeySetId;
    TraceId nextTraceId;

    // ── Pending writes (deferred to flush()) ─────────────────────────

    std::vector<std::pair<StringId, std::string>> pendingStrings;

    std::vector<std::pair<AttrPathId, std::string>> pendingAttrPaths;

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

    TraceStore(SymbolTable & symbols, InterningPools & pools, int64_t contextHash);
    ~TraceStore();

    /** Bulk-load all interned entities from DB into in-memory maps.
     *  Called once from constructor after schema creation, and again
     *  from clearSessionCaches() to repopulate after a reset. */
    void bulkLoadAll();

    /** Flush all pending writes to SQLite in dependency order
     *  (Strings → AttrPaths → Results → DepKeySets → Traces).
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
        const std::vector<CompactDep> & allDeps,
        bool isRoot);

    /** Convenience overload: accepts Dep objects, interns them into CompactDeps.
     *  Must pass an explicit vector (not an initializer list) to avoid ambiguity. */
    RecordResult recordDeps(
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

    // Full dep interning/resolution (combines key set + values)
    std::vector<Dep> resolveDeps(const std::vector<InternedDepKey> & keys,
                                  const std::vector<DepHashValue> & values);
    std::vector<InternedDep> internDeps(const std::vector<CompactDep> & deps);
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
