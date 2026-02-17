#pragma once

#include "nix/expr/trace-cache.hh"
#include "nix/expr/dep-tracker.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/sync.hh"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace nix {
class EvalState;
}

namespace nix::eval_trace {

std::string depTypeString(DepType type);

struct TraceStore {
    struct State {
        SQLite db;

        // Strings interning
        SQLiteStmt insertString;
        SQLiteStmt lookupStringId;

        // AttrPaths interning
        SQLiteStmt insertAttrPath;
        SQLiteStmt lookupAttrPathId;

        // Results dedup
        SQLiteStmt insertResult;
        SQLiteStmt lookupResultByHash;
        SQLiteStmt getResult;

        // Traces (delta-encoded with base_id chain, BLOB storage)
        SQLiteStmt insertTrace;
        SQLiteStmt lookupTraceByFullHash;
        SQLiteStmt getTraceInfo;
        SQLiteStmt lookupTraceByStructHash;
        SQLiteStmt updateTraceBlob;
        SQLiteStmt getAllStrings;

        // CurrentTraces (current verified state per attribute)
        SQLiteStmt lookupAttr;
        SQLiteStmt upsertAttr;

        // TraceHistory (all historical traces for constructive recovery)
        SQLiteStmt insertHistory;
        SQLiteStmt lookupHistoryByTrace;
        SQLiteStmt scanHistoryForAttr;

        std::unique_ptr<SQLiteTxn> txn;
    };
    std::unique_ptr<Sync<State>> _state;
    SymbolTable & symbols;
    int64_t contextHash;

    // Interned dep entry (string IDs instead of strings, for BLOB serialization)
    struct InternedDep {
        DepType type;
        uint32_t sourceId;
        uint32_t keyId;
        DepHashValue hash;
    };

    struct TraceRow {
        int64_t traceId;
        int64_t resultId;
        int type;
        std::string value;
        std::string context;
    };

    // Session caches
    std::set<int64_t> verifiedTraceIds;
    std::unordered_map<std::string, int64_t> internedStrings;
    std::unordered_map<std::string, int64_t> internedAttrPaths;
    std::map<int64_t, std::vector<Dep>> traceCache;
    std::map<int64_t, Hash> traceHashCache;
    std::map<int64_t, Hash> traceStructHashCache;

    // Session string table (reverse: id -> string, for BLOB deserialization)
    std::unordered_map<int64_t, std::string> stringTable;
    bool stringTableLoaded = false;
    void ensureStringTableLoaded();

    // Current dep hash cache (persists across verification → recovery within session)
    std::unordered_map<DepKey, std::optional<DepHashValue>, DepKey::Hash> currentDepHashes;

    // Dirty traces (recorded this session, for post-record optimization)
    std::set<int64_t> dirtyTraceIds;

    TraceStore(SymbolTable & symbols, int64_t contextHash);
    ~TraceStore();

    struct VerifyResult {
        CachedResult value;
        int64_t traceId;
    };

    struct RecordResult {
        int64_t traceId;
    };

    /**
     * Verify a cached trace and return its stored result if valid.
     *
     * Implements a BSàlC verifying trace (VT) check: looks up the current trace
     * for this attribute, recomputes all dep hashes, and compares against stored
     * values. If all match, returns the constructive trace (CT) result directly
     * without re-evaluation. On verification failure, automatically attempts
     * constructive recovery before returning nullopt.
     *
     * The parentTraceIdHint enables Merkle parent chaining: when provided, the
     * parent's trace_hash is mixed into recovery lookups, disambiguating child
     * traces across different parent versions (analogous to Salsa's versioned
     * query with context).
     */
    std::optional<VerifyResult> verify(
        std::string_view attrPath,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state,
        std::optional<int64_t> parentTraceIdHint = std::nullopt);

    /**
     * Record a fresh evaluation result with its dependencies (BSàlC constructive
     * trace recording).
     *
     * Stores (attrPath, result, deps) as a constructive trace: the full result
     * value is persisted alongside dep hashes, enabling future recovery without
     * re-evaluation. If a trace with the same trace_hash already exists (dedup),
     * reuses it. Delta-encodes deps against a base trace when possible.
     *
     * Also inserts into TraceHistory for constructive recovery: historical traces
     * can be recovered when dep hashes match a previously-seen state (e.g., after
     * a file revert).
     */
    RecordResult record(
        std::string_view attrPath,
        const CachedResult & value,
        const std::vector<Dep> & allDeps,
        std::optional<int64_t> parentTraceId,
        bool isRoot);

    /**
     * Constructive trace recovery (BSàlC CT recovery).
     *
     * When verification fails (dep hashes changed), searches historical traces
     * for one whose deps match the current state. This is the key advantage of
     * a constructive trace over a verifying trace: stored results enable recovery
     * without re-evaluation.
     *
     * Two-phase recovery:
     *   Phase 1 — Direct hash lookup: compute trace_hash from current dep hashes
     *     (with optional parent Merkle chaining via parentTraceIdHint), look up
     *     in Traces table. O(1). Handles file reverts and same-structure changes.
     *   Phase 3 — Structural variant scan: scan TraceHistory for entries with the
     *     same (context_hash, attr_path_id), group by struct_hash, recompute
     *     current dep hashes for each structural variant, retry Phase 1 lookup.
     *     O(V) where V = number of distinct dep structures. Handles dynamic dep
     *     instability (Shake-style: deps vary between evaluations). Novel extension
     *     beyond BSàlC's taxonomy.
     */
    std::optional<VerifyResult> recovery(
        int64_t oldTraceId,
        std::string_view attrPath,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state,
        std::optional<int64_t> parentTraceIdHint = std::nullopt);

    std::vector<Dep> loadFullTrace(int64_t traceId);
    bool attrExists(std::string_view attrPath);
    void clearSessionCaches();
    static std::string buildAttrPath(const std::vector<std::string> & components);

    void optimizeTraces();

    // BLOB serialization for dep entries
    static std::vector<uint8_t> serializeDeps(const std::vector<InternedDep> & deps);
    static std::vector<InternedDep> deserializeInternedDeps(const void * blob, size_t size);
    std::vector<Dep> resolveDeps(const std::vector<InternedDep> & interned);
    std::vector<InternedDep> internDeps(const std::vector<Dep> & deps);

    /**
     * Verify a single trace: recompute all dep hashes and compare against
     * stored expected values (BSàlC VT dep-hash verification).
     *
     * Returns true if every non-volatile dep's current hash matches its stored
     * hash. Volatile deps (CurrentTime, Exec) always fail. All computed hashes
     * are cached in currentDepHashes for reuse by recovery().
     *
     * Session-memoized: a trace verified once in this session is not re-verified
     * (tracked via verifiedTraceIds).
     */
    bool verifyTrace(
        int64_t traceId,
        const std::map<std::string, SourcePath> & inputAccessors,
        EvalState & state);

private:
    std::optional<TraceRow> lookupTraceRow(std::string_view attrPath);

    int64_t doInternString(std::string_view s);
    int64_t doInternAttrPath(std::string_view path);
    int64_t doInternResult(int type, const std::string & value,
                           const std::string & context, const Hash & resultHash);

    int64_t getOrCreateTrace(
        const std::vector<Dep> & fullDeps,
        const std::vector<InternedDep> & deltaDeps,
        const Hash & traceHash,
        const Hash & structHash,
        std::optional<int64_t> baseTraceId);

    std::vector<Dep> loadTraceDelta(int64_t traceId);

    static std::vector<Dep> computeTraceDelta(
        const std::vector<Dep> & fullDeps,
        const std::vector<Dep> & baseDeps);

    CachedResult decodeCachedResult(const TraceRow & row);
    std::tuple<int, std::string, std::string> encodeCachedResult(const CachedResult & value);

    Hash getTraceFullHash(int64_t traceId);
    std::optional<int64_t> getTraceBaseId(int64_t traceId);
};

} // namespace nix::eval_trace
