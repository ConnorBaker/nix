#pragma once
/// store/in-memory-trace-rows.hh — In-process, non-persistent trace-rows
/// storage.
///
/// Rearchitecture-proposal.md §14 step 4. One of two concrete backends
/// deriving from `TraceStorage`: `SqliteTraceStorage` (today's
/// production path) and `InMemoryTraceRows` (no SQLite file on disk).
///
/// Used by tests that want to exercise the abstract `TraceStorage`
/// interface without a SQLite file on disk, and as a building block for
/// future non-persistent scenarios. Storage shape mirrors
/// `SqliteTraceStorage`'s in-memory caches without the persistence
/// layer: every insert is accepted directly, lookup is a hash-table
/// probe, `flush` is a no-op.

#include "nix/expr/eval-trace/store/trace-storage.hh"
#include "nix/expr/eval-trace/store/trace-value-types.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <vector>

namespace nix {
class Store;
}

namespace nix::eval_trace {

/// In-process, non-persistent trace-rows storage.
///
/// Mirrors `SqliteTraceStorage`'s public data-path shape on
/// `boost::unordered_flat_map` containers.  Content-addressed dedup,
/// no backing file, no flush transaction.  Satisfies the
/// `TraceStorageLike` concept; callers that want to be generic over
/// backends spell `template<TraceStorageLike S>` — no inheritance
/// from a virtual base, no vtable, no vptr.
class InMemoryTraceRows : public TraceStorageBase
{
public:
    explicit InMemoryTraceRows(SemanticSessionKey initialKey);
    ~InMemoryTraceRows() = default;

    /// Bind the process-global hash algorithm via
    /// `storeSessionConfig(config, getEvalTraceHashAlgorithm())`,
    /// matching the `SqliteTraceStorage` behavior. Throws on second
    /// call per `SetOnce`.
    void setSessionConfig(SessionConfig config);

    // ── Content-addressed writes (§2.1) ──────────────────────────────

    ResultId insertResult(
        const ExclusiveTraceStorageAccess &,
        ResultHash, EncodedResultPayload &&);

    DepKeySetId insertDepKeySet(
        const ExclusiveTraceStorageAccess &,
        DepKeySetHash, std::vector<uint8_t> && keysBlob);

    TraceId insertTrace(
        const ExclusiveTraceStorageAccess &,
        FullTraceHash, TraceHeader,
        std::vector<uint8_t> && valuesBlob);

    // ── Reads ────────────────────────────────────────────────────────

    std::optional<TraceHeader> loadTraceHeader(
        const ExclusiveTraceStorageAccess &, TraceId);

    std::optional<TraceBlobs> loadTraceBlobs(
        const ExclusiveTraceStorageAccess &, TraceId);

    std::optional<std::vector<uint8_t>> loadKeysBlob(
        const ExclusiveTraceStorageAccess &, DepKeySetId);

    std::optional<ResultPayload> loadResultPayload(
        const ExclusiveTraceStorageAccess &, ResultId);

    // ── Current / history lookup ────────────────────────────────────

    std::optional<CurrentNodeRef> lookupCurrent(
        const ExclusiveTraceStorageAccess &, AttrPathId);

    std::optional<HistoryNodeRef> lookupLatestHistory(
        const ExclusiveTraceStorageAccess &, AttrPathId);

    std::vector<HistoryEntry> queryAllHistory(
        const ExclusiveTraceStorageAccess &, AttrPathId);

    std::vector<HistoryEntry> queryHistoryByTraceHash(
        const ExclusiveTraceStorageAccess &, AttrPathId, TraceHash);

    std::vector<HistoryEntry> queryHistoryByGitIdentity(
        const ExclusiveTraceStorageAccess &, AttrPathId,
        CurrentGitIdentityHash);

    // ── Publish ─────────────────────────────────────────────────────

    CurrentNodeRef publishFreshRecord(
        const ExclusiveTraceStorageAccess &,
        AttrPathId, TraceId, ResultId,
        std::optional<EvalTraceHash> gitIdentityHash);

    CurrentNodeRef publishHistoryBootstrap(
        const ExclusiveTraceStorageAccess &,
        AttrPathId, TraceId, ResultId);

    void recordRuntimeRoot(
        const ExclusiveTraceStorageAccess &,
        const RuntimeRootRecord & record,
        Store & store);

    RuntimeRootLoadResult loadRuntimeRoots(
        const ExclusiveTraceStorageAccess &,
        Store & store);

    /// No-op: in-memory storage has no pending writes to flush.
    void flush(const ExclusiveTraceStorageAccess &) {}

private:
    /// Stored trace row — blobs held verbatim, no compression.
    struct TraceRow {
        TraceHeader header;
        FullTraceHash fullHash;
        std::vector<uint8_t> valuesBlob;
    };

    /// Stored history row — same shape as an `HistoryEntry` plus the
    /// git-identity hash that `queryHistoryByGitIdentity` filters on.
    struct HistoryRow {
        HistoryEntry entry;
        std::optional<EvalTraceHash> gitIdentityHash;
    };

    // Content-addressed dedup maps (mirror SqliteTraceStorage's in-memory caches).
    boost::unordered_flat_map<ResultHash, ResultId, ResultHash::Hash> resultByHash_;
    boost::unordered_flat_map<DepKeySetHash, DepKeySetId, DepKeySetHash::Hash> depKeySetByHash_;
    boost::unordered_flat_map<FullTraceHash, TraceId, FullTraceHash::Hash> traceByFullHash_;

    // Row storage keyed by ID.
    boost::unordered_flat_map<ResultId, ResultPayload, ResultId::Hash> resultRows_;
    boost::unordered_flat_map<DepKeySetId, std::vector<uint8_t>, DepKeySetId::Hash> depKeySetRows_;
    boost::unordered_flat_map<TraceId, TraceRow, TraceId::Hash> traceRows_;

    // Current-node index (`Sessions` analogue).
    boost::unordered_flat_map<AttrPathId, CurrentNodeRef, AttrPathId::Hash> currentByAttr_;

    // History rows (append-only), grouped by attr path so scans are O(history(path))
    // rather than O(all history).
    boost::unordered_flat_map<AttrPathId, std::vector<HistoryRow>, AttrPathId::Hash> historyByAttr_;

    std::vector<RuntimeRootRecord> runtimeRoots_;

    // In-memory ID counters.
    ResultId nextResultId_{1};
    DepKeySetId nextDepKeySetId_{1};
    TraceId nextTraceId_{1};
};

} // namespace nix::eval_trace
