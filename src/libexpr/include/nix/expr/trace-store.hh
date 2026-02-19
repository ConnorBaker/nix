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

        // DepsSets (content-addressed dep storage)
        SQLiteStmt upsertDepsSet;

        // Traces (references DepsSets via deps_set_id FK)
        SQLiteStmt upsertTrace;
        SQLiteStmt lookupTraceByFullHash;
        SQLiteStmt getTraceInfo;
        SQLiteStmt lookupTraceByStructHash;

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

    // Interned dep entry (string IDs instead of strings, for BLOB serialization)
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

    // Session caches
    std::unordered_set<TraceId> verifiedTraceIds;
    std::unordered_map<std::string, StringId> internedStrings;
    std::unordered_map<std::string, AttrPathId> internedAttrPaths;
    std::unordered_map<TraceId, std::vector<Dep>> traceCache;
    std::unordered_map<TraceId, Hash> traceStructHashCache;

    // Session string table (reverse: id -> string, for BLOB deserialization)
    std::unordered_map<StringId, std::string> stringTable;
    bool stringTableLoaded = false;
    void ensureStringTableLoaded();

    // Current dep hash cache (persists across verification → recovery within session)
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
     * reuses it. Deps are content-addressed via DepsSets table.
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
     *     (context_hash, attr_path_id), group by struct_hash, recompute current
     *     dep hashes for each structural variant, retry direct hash lookup.
     *     O(V) where V = number of distinct dep structures. Handles dynamic dep
     *     instability (Shake-style: deps vary between evaluations). Novel
     *     extension beyond BSàlC's taxonomy.
     */
    std::optional<VerifyResult> recovery(
        TraceId oldTraceId,
        std::string_view attrPath,
        const std::unordered_map<std::string, SourcePath> & inputAccessors,
        EvalState & state);

    /**
     * Load the full dependency set for a trace.
     *
     * Reads the deps_blob from the DepsSets table via the trace's deps_set_id
     * foreign key. O(1) — single DB read + zstd decompression.
     *
     * The returned vector is NOT sorted. Callers that need canonical ordering
     * must call sortAndDedupDeps() explicitly.
     */
    std::vector<Dep> loadFullTrace(TraceId traceId);
    bool attrExists(std::string_view attrPath);
    /** Get the current trace hash for an attr path (for ParentContext dep verification).
     *  Returns the trace_hash from the Traces table, which captures the full dep
     *  structure + hashes. Unlike a result hash (which for attrsets only captures
     *  attribute names), the trace hash changes when any dep value changes. */
    std::optional<Hash> getCurrentTraceHash(std::string_view attrPath);
    void clearSessionCaches();
    static std::string buildAttrPath(const std::vector<std::string> & components);

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
     * hash. Volatile deps (CurrentTime, Exec) always fail.
     *
     * When earlyExit is true, returns false on the first mismatch without
     * computing remaining dep hashes. When false (default), computes ALL dep
     * hashes even on mismatch, populating currentDepHashes for recovery().
     *
     * Session-memoized: a trace verified once in this session is not re-verified
     * (tracked via verifiedTraceIds).
     */
    bool verifyTrace(
        TraceId traceId,
        const std::unordered_map<std::string, SourcePath> & inputAccessors,
        EvalState & state,
        bool earlyExit = false);

private:
    std::optional<TraceRow> lookupTraceRow(std::string_view attrPath);

    StringId doInternString(std::string_view s);
    AttrPathId doInternAttrPath(std::string_view path);
    ResultId doInternResult(ResultKind type, const std::string & value,
                            const std::string & context, const Hash & resultHash);

    TraceId getOrCreateTrace(
        const Hash & traceHash,
        const Hash & structHash,
        int64_t depsSetId);

    int64_t getOrCreateDepsSet(
        const std::vector<Dep> & fullDeps,
        const Hash & depsHash);

    CachedResult decodeCachedResult(const TraceRow & row);
    std::tuple<ResultKind, std::string, std::string> encodeCachedResult(const CachedResult & value);

    Hash getTraceStructHash(TraceId traceId);
};

} // namespace nix::eval_trace
