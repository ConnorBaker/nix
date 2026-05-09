/// in-memory-trace-rows.cc — Implementation of the in-process,
/// non-persistent `TraceStorage` backend (rearchitecture-proposal.md
/// §14 step 4).

#include "nix/expr/eval-trace/store/in-memory-trace-rows.hh"

#include "nix/expr/eval-trace/hash-spec.hh"

#include <utility>

namespace nix::eval_trace {

InMemoryTraceRows::InMemoryTraceRows(SemanticSessionKey initialKey)
    : TraceStorage(std::move(initialKey))
{
}

void InMemoryTraceRows::setSessionConfig(SessionConfig config)
{
    // Pin the process-global hash algorithm so the session key digest
    // matches what an equivalent SqliteTraceStorage would have computed.
    storeSessionConfig(std::move(config), getEvalTraceHashAlgorithm());
}

// ── Content-addressed writes ────────────────────────────────────────

ResultId InMemoryTraceRows::insertResult(
    const ExclusiveTraceStorageAccess &,
    ResultHash hash, EncodedResultPayload && payload)
{
    auto it = resultByHash_.find(hash);
    if (it != resultByHash_.end())
        return it->second;
    auto id = ResultId(++nextResultId_.value);
    resultByHash_[hash] = id;
    resultRows_.emplace(
        id,
        ResultPayload{
            .type = payload.type,
            .encodingVersion = payload.encodingVersion,
            .payload = std::move(payload.payload),
            .auxContext = std::move(payload.auxContext),
        });
    return id;
}

DepKeySetId InMemoryTraceRows::insertDepKeySet(
    const ExclusiveTraceStorageAccess &,
    DepKeySetHash hash, std::vector<uint8_t> && keysBlob)
{
    auto it = depKeySetByHash_.find(hash);
    if (it != depKeySetByHash_.end())
        return it->second;
    auto id = DepKeySetId(++nextDepKeySetId_.value);
    depKeySetByHash_[hash] = id;
    depKeySetRows_.emplace(id, std::move(keysBlob));
    return id;
}

TraceId InMemoryTraceRows::insertTrace(
    const ExclusiveTraceStorageAccess &,
    FullTraceHash fullHash, TraceHeader header,
    std::vector<uint8_t> && valuesBlob)
{
    auto it = traceByFullHash_.find(fullHash);
    if (it != traceByFullHash_.end())
        return it->second;
    auto id = TraceId(++nextTraceId_.value);
    traceByFullHash_[fullHash] = id;
    traceRows_.emplace(
        id,
        TraceRow{
            .header = header,
            .fullHash = fullHash,
            .valuesBlob = std::move(valuesBlob),
        });
    return id;
}

// ── Reads ───────────────────────────────────────────────────────────

std::optional<TraceHeader> InMemoryTraceRows::loadTraceHeader(
    const ExclusiveTraceStorageAccess &, TraceId traceId)
{
    auto it = traceRows_.find(traceId);
    if (it == traceRows_.end())
        return std::nullopt;
    return it->second.header;
}

std::optional<TraceBlobs> InMemoryTraceRows::loadTraceBlobs(
    const ExclusiveTraceStorageAccess &, TraceId traceId)
{
    auto traceIt = traceRows_.find(traceId);
    if (traceIt == traceRows_.end())
        return std::nullopt;
    auto keysIt = depKeySetRows_.find(traceIt->second.header.depKeySetId);
    if (keysIt == depKeySetRows_.end())
        return std::nullopt;
    return TraceBlobs{
        .header = traceIt->second.header,
        .keysBlob = keysIt->second,
        .valuesBlob = traceIt->second.valuesBlob,
    };
}

std::optional<std::vector<uint8_t>> InMemoryTraceRows::loadKeysBlob(
    const ExclusiveTraceStorageAccess &, DepKeySetId depKeySetId)
{
    auto it = depKeySetRows_.find(depKeySetId);
    if (it == depKeySetRows_.end())
        return std::nullopt;
    return it->second;
}

std::optional<ResultPayload> InMemoryTraceRows::loadResultPayload(
    const ExclusiveTraceStorageAccess &, ResultId resultId)
{
    auto it = resultRows_.find(resultId);
    if (it == resultRows_.end())
        return std::nullopt;
    return it->second;
}

// ── Current / history lookup ────────────────────────────────────────

std::optional<CurrentNodeRef> InMemoryTraceRows::lookupCurrent(
    const ExclusiveTraceStorageAccess &, AttrPathId pathId)
{
    auto it = currentByAttr_.find(pathId);
    if (it == currentByAttr_.end())
        return std::nullopt;
    return it->second;
}

std::optional<HistoryNodeRef> InMemoryTraceRows::lookupLatestHistory(
    const ExclusiveTraceStorageAccess &, AttrPathId pathId)
{
    auto it = historyByAttr_.find(pathId);
    if (it == historyByAttr_.end() || it->second.empty())
        return std::nullopt;
    const auto & latest = it->second.back().entry;
    return HistoryNodeRef{latest.traceId, latest.resultId};
}

std::vector<HistoryEntry> InMemoryTraceRows::queryAllHistory(
    const ExclusiveTraceStorageAccess &, AttrPathId pathId)
{
    std::vector<HistoryEntry> out;
    auto it = historyByAttr_.find(pathId);
    if (it == historyByAttr_.end())
        return out;
    out.reserve(it->second.size());
    for (const auto & row : it->second)
        out.push_back(row.entry);
    return out;
}

std::vector<HistoryEntry> InMemoryTraceRows::queryHistoryByTraceHash(
    const ExclusiveTraceStorageAccess &, AttrPathId pathId, TraceHash traceHash)
{
    std::vector<HistoryEntry> out;
    auto it = historyByAttr_.find(pathId);
    if (it == historyByAttr_.end())
        return out;
    for (const auto & row : it->second)
        if (row.entry.traceHash == traceHash)
            out.push_back(row.entry);
    return out;
}

std::vector<HistoryEntry> InMemoryTraceRows::queryHistoryByGitIdentity(
    const ExclusiveTraceStorageAccess &, AttrPathId pathId,
    CurrentGitIdentityHash gitHash)
{
    std::vector<HistoryEntry> out;
    auto it = historyByAttr_.find(pathId);
    if (it == historyByAttr_.end())
        return out;
    for (const auto & row : it->second)
        if (row.gitIdentityHash && *row.gitIdentityHash == gitHash.value)
            out.push_back(row.entry);
    return out;
}

// ── Publish ─────────────────────────────────────────────────────────

CurrentNodeRef InMemoryTraceRows::publishFreshRecord(
    const ExclusiveTraceStorageAccess &,
    AttrPathId pathId, TraceId traceId, ResultId resultId,
    std::optional<EvalTraceHash> gitIdentityHash)
{
    auto ref = CurrentNodeRef{traceId, resultId, allocateNodeStamp()};
    currentByAttr_[pathId] = ref;

    // Fetch the trace's header so the History row carries the same
    // trace_hash / dep_key_set_id that a Sessions lookup would.
    TraceHash traceHash{};
    DepKeySetId depKeySetId{};
    auto traceIt = traceRows_.find(traceId);
    if (traceIt != traceRows_.end()) {
        traceHash = traceIt->second.header.traceHash;
        depKeySetId = traceIt->second.header.depKeySetId;
    }
    historyByAttr_[pathId].push_back(HistoryRow{
        .entry = HistoryEntry{
            .depKeySetId = depKeySetId,
            .traceId = traceId,
            .resultId = resultId,
            .traceHash = traceHash,
        },
        .gitIdentityHash = gitIdentityHash,
    });
    return ref;
}

CurrentNodeRef InMemoryTraceRows::publishHistoryBootstrap(
    const ExclusiveTraceStorageAccess &,
    AttrPathId pathId, TraceId traceId, ResultId resultId)
{
    auto ref = CurrentNodeRef{traceId, resultId, allocateNodeStamp()};
    currentByAttr_[pathId] = ref;
    return ref;
}

// ── Runtime roots ───────────────────────────────────────────────────

void InMemoryTraceRows::recordRuntimeRoot(
    const ExclusiveTraceStorageAccess &,
    const RuntimeRootRecord & record,
    Store & /*store*/)
{
    runtimeRoots_.push_back(record);
}

RuntimeRootLoadResult InMemoryTraceRows::loadRuntimeRoots(
    const ExclusiveTraceStorageAccess &,
    Store & /*store*/)
{
    return RuntimeRootLoadResult{
        .storedCount = runtimeRoots_.size(),
        .rejectedCount = 0,
        .entries = runtimeRoots_,
    };
}

} // namespace nix::eval_trace
