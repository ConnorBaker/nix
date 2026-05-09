#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/recorder.hh"
#include "nix/expr/eval-trace/store/trace-resolve.hh"
#include "../fiber/blocking-scope.hh"
#include "nix/expr/eval-trace/deps/analysis.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/eval-trace/deps/hash.hh"

#include "trace-serialize.hh"
#include "nix/store/store-api.hh"
#include "nix/util/logging.hh"

#include <algorithm>
#include <cassert>
#include <tuple>

namespace nix::eval_trace {

static std::string renderStructuredKeyDisplay(const SqliteTraceStorage::ResolvedDep::StructuredKey & key)
{
    if (!key.dirSetHash.empty()) {
        auto result = std::string("dirset(") + key.dirSetHash + ")@"
            + std::string(1, structuredFormatChar(key.format));
        if (!key.hasKey.empty())
            result += "#has(" + key.hasKey + ")";
        return result;
    }

    auto result = key.filePath + "@" + std::string(1, structuredFormatChar(key.format))
        + displayStructuredPath(key.dataPath);
    if (!key.hasKey.empty())
        result += "#has(" + key.hasKey + ")";
    else if (key.suffix != ShapeSuffix::None)
        result += "#" + std::string(shapeSuffixName(key.suffix));
    return result;
}

static std::string renderDepSourceDisplay(std::string_view encoded)
{
    auto decoded = decodeDepSourceBlob(encoded);
    if (!decoded)
        return "<invalid dep source>";
    return serializeDepSource(*decoded);
}

// ── Dep key/value resolution ─────────────────────────────────────────

ResolvedDep resolveDep(const InterningPools & pools, AttrVocabStore & vocab, const Dep & dep)
{
    if (dep.key.isTraceContext()) {
        auto pathId = dep.key.attrPathId;
        return ResolvedDep{
            .source = "",
            .key = vocab.displayPath(pathId),
            .expectedHash = dep.hash,
            .type = dep.key.kind,
            .structured = std::nullopt,
            .traceContext = ResolvedDep::TraceContextKey{
                .pathId = pathId,
            },
        };
    }
    if (dep.key.isStructured()) {
        auto structured = ResolvedDep::StructuredKey{
            .filePath = std::string(pools.resolve(dep.key.filePathId)),
            .format = dep.key.structuredFormat(),
            .dataPath = resolveStructuredPath(pools, dep.key.dataPathId),
            .suffix = dep.key.suffix,
            .hasKey = dep.key.hasKeyId ? std::string(pools.resolve(dep.key.hasKeyId)) : "",
            .dirSetHash = dep.key.dirSetHashId ? std::string(pools.resolve(dep.key.dirSetHashId)) : "",
        };
        return ResolvedDep{
            .source = renderDepSourceDisplay(pools.resolve(dep.key.sourceId)),
            .key = renderStructuredKeyDisplay(structured),
            .expectedHash = dep.hash,
            .type = dep.key.kind,
            .structured = std::move(structured),
            .traceContext = std::nullopt,
        };
    }
    return ResolvedDep{
        .source = renderDepSourceDisplay(pools.resolve(dep.key.sourceId)),
        .key = renderSimpleDepKeyDisplay(pools, dep.key),
        .expectedHash = dep.hash,
        .type = dep.key.kind,
        .structured = std::nullopt,
        .traceContext = std::nullopt,
    };
}

// Back-compat shim: `SqliteTraceStorage::resolveDep` forwards to the free fn.
// Body is inline in trace-store.hh.

namespace {
// Key-accessor helpers: callers pass either `const std::vector<Dep> &` or
// `const std::vector<Dep::Key> &`.  Both iterate keys identically; the
// two top-level functions below stream through a uniform range-of-keys view.
inline const Dep::Key & keyOf(const Dep & dep) { return dep.key; }
inline const Dep::Key & keyOf(const Dep::Key & key) { return key; }

template<typename Range>
std::optional<RepoRootId> extractGoverningRepoIdImpl(const Range & range)
{
    for (auto & entry : range) {
        const auto & key = keyOf(entry);
        if (key.kind == CanonicalQueryKind::GitRevisionIdentity
            && key.governingRepoId.value != 0)
            return key.governingRepoId;
    }
    return std::nullopt;
}

template<typename Range>
bool allKeysGitRecoverableImpl(const Range & range, RepoRootId targetRepoId)
{
    if (targetRepoId.value == 0)
        return false;

    for (auto & entry : range) {
        const auto & key = keyOf(entry);
        if (key.kind == CanonicalQueryKind::GitRevisionIdentity)
            continue;
        if (key.isTraceContext())
            return false;
        if (isVolatile(key.kind))
            return false;
        if (isCoveredBySessionFingerprint(key.kind))
            continue;
        if (isFileContentDep(key.kind)) {
            if (key.governingRepoId == targetRepoId)
                continue;
            return false;
        }
        return false;
    }
    return true;
}
} // anonymous namespace

std::optional<RepoRootId> extractGoverningRepoId(const std::vector<Dep> & deps)
{
    return extractGoverningRepoIdImpl(deps);
}

std::optional<RepoRootId> extractGoverningRepoId(const std::vector<Dep::Key> & keys)
{
    return extractGoverningRepoIdImpl(keys);
}

bool allDepsGitRecoverable(const std::vector<Dep> & deps, RepoRootId targetRepoId)
{
    return allKeysGitRecoverableImpl(deps, targetRepoId);
}

bool allDepsGitRecoverable(const std::vector<Dep::Key> & keys, RepoRootId targetRepoId)
{
    return allKeysGitRecoverableImpl(keys, targetRepoId);
}

std::optional<StoredGitIdentityHash> extractGitIdentityHash(const std::vector<Dep> & deps)
{
    for (auto & dep : deps) {
        if (dep.key.kind != CanonicalQueryKind::GitRevisionIdentity)
            continue;
        auto * recordedHash = std::get_if<DepHash>(&dep.hash);
        if (!recordedHash)
            return std::nullopt;
        if (dep.key.governingRepoId.value == 0)
            return std::nullopt;
        // Check all other deps are recoverable against this repo id.
        if (allDepsGitRecoverable(deps, dep.key.governingRepoId))
            return StoredGitIdentityHash{recordedHash->value};
        return std::nullopt;
    }
    return std::nullopt;
}

// Back-compat shims for extractGoverningRepoId / allDepsGitRecoverable /
// extractGitIdentityHash / resolveDep are inline in trace-store.hh so
// the compiler can fold them into the free-fn call at the use site.
// The free fns live in deps/analysis.hh + store/trace-resolve.hh.
// Production callers (trace-store-verify.cc, trace-store.cc itself)
// use the free fns directly; tests still go through the shims.

// ── Intern methods ───────────────────────────────────────────────────

ResultId SqliteTraceStorage::doInternResult(const EncodedResultPayload & payload, const ResultHash & resultHash)
{
    auto it = resultByHash.find(resultHash);
    if (it != resultByHash.end())
        return it->second;

    auto id = ResultId(++nextResultId.value);
    resultByHash[resultHash] = id;
    pendingResults.push_back({id, payload, resultHash});
    return id;
}

// ── Trace storage (BSàlC trace store) ───────────────────────────────


SqliteTraceStorage::TraceHeader * SqliteTraceStorage::ensureTraceHeader(const gdp::Proof<BlockingTag> & bs, TraceId traceId)
{
    // Caller holds storeMutex_ via withExclusiveAccess — no inner lock_guard.
    auto it = traceCache.find(traceId);
    if (it != traceCache.end())
        return &it->second.header;

    auto & st = *_state;
    // getTraceInfo columns: 0=trace_hash, 1=key_set_hash, 2=dep_key_set_id, 3=keys_blob, 4=values_blob
    auto use(st.getTraceInfo.use()(static_cast<int64_t>(traceId.value)));
    if (!use.next())
        return nullptr;

    auto [thData, thSize] = use.getBlob(0);
    auto traceHash = evalTraceHashFromBlob<TraceHash>(thData, thSize);
    auto [kshData, kshSize] = use.getBlob(1);
    auto keySetHash = evalTraceHashFromBlob<DepKeySetHash>(kshData, kshSize);
    auto depKeySetId = DepKeySetId(static_cast<uint32_t>(use.getInt(2)));

    auto [insertIt, _] = traceCache.emplace(
        traceId,
        TraceCacheEntry{
            .header = TraceHeader{
                .traceHash = traceHash,
                .keySetHash = keySetHash,
                .depKeySetId = depKeySetId,
            },
        });
    return &insertIt->second.header;
}

std::shared_ptr<const std::vector<Dep>> SqliteTraceStorage::loadFullTrace(
    const ExclusiveTraceStorageAccess &, TraceId traceId)
{
    {
        auto it = traceCache.find(traceId);
        if (it != traceCache.end() && it->second.full)
            return it->second.full;
    }

    auto loadStart = timerStart();
    nrLoadTraces++;

    // Check for deferred values blob (stashed by loadTraceKeysAndHeader)
    auto deferredIt = deferredTraceBlobs.find(traceId);
    if (deferredIt != deferredTraceBlobs.end()) {
        auto & deferred = deferredIt->second;
        auto keysIt = depKeySetCache.find(deferred.depKeySetId);
        if (keysIt != depKeySetCache.end()) {
            auto values = deserializeValues(deferred.valuesBlob.data(), deferred.valuesBlob.size());
            auto & keys = *keysIt->second;
            if (keys.size() != values.size())
                throw Error("trace %d: key/value count mismatch", traceId.value);
            auto result = std::make_shared<std::vector<Dep>>();
            result->reserve(keys.size());
            for (size_t i = 0; i < keys.size(); i++)
                result->push_back({keys[i], std::move(values[i])});
            deferredTraceBlobs.erase(deferredIt);  // consumed
            traceCache[traceId].full = result;
            nrLoadTraceTimeUs += elapsedUs(loadStart);
            return result;
        }
        // Keys not cached — fall through to DB query (will re-fetch both)
    }

    // Single DB read via JOIN — keys from DepKeySets, values from Traces
    // getTraceInfo columns: 0=trace_hash, 1=key_set_hash, 2=dep_key_set_id, 3=keys_blob, 4=values_blob
    std::vector<uint8_t> keysBlobCopy, valuesBlobCopy;
    DepKeySetId depKeySetId;
    TraceHash traceHash{};
    DepKeySetHash keySetHash{};
    {
        auto & st = *_state;
        auto use(st.getTraceInfo.use()(static_cast<int64_t>(traceId.value)));
        if (!use.next()) {
            nrLoadTraceTimeUs += elapsedUs(loadStart);
            return std::make_shared<const std::vector<Dep>>();
        }

        // Opportunistically populate hash fields from the same query
        auto [thData, thSize] = use.getBlob(0);
        traceHash = evalTraceHashFromBlob<TraceHash>(thData, thSize);
        auto [kshData, kshSize] = use.getBlob(1);
        keySetHash = evalTraceHashFromBlob<DepKeySetHash>(kshData, kshSize);
        depKeySetId = DepKeySetId(static_cast<uint32_t>(use.getInt(2)));

        auto [keysData, keysSize] = use.getBlob(3);
        if (keysData && keysSize > 0) {
            auto * p = static_cast<const uint8_t *>(keysData);
            keysBlobCopy.assign(p, p + keysSize);
        }

        auto [valsData, valsSize] = use.getBlob(4);
        if (valsData && valsSize > 0) {
            auto * p = static_cast<const uint8_t *>(valsData);
            valuesBlobCopy.assign(p, p + valsSize);
        }
    }

    TraceHeader header{
        .traceHash = traceHash,
        .keySetHash = keySetHash,
        .depKeySetId = depKeySetId,
    };

    if (keysBlobCopy.empty()) {
        depKeySetCache.insert_or_assign(
            depKeySetId,
            std::make_shared<const std::vector<Dep::Key>>());
        auto result = std::make_shared<const std::vector<Dep>>();
        traceCache.insert_or_assign(traceId, TraceCacheEntry{header, result});
        nrLoadTraceTimeUs += elapsedUs(loadStart);
        return result;
    }

    // Deserialize keys and values independently (values blob is self-describing)
    auto keys = std::make_shared<std::vector<Dep::Key>>(
        deserializeKeys(keysBlobCopy.data(), keysBlobCopy.size()));
    auto values = deserializeValues(valuesBlobCopy.data(), valuesBlobCopy.size());

    if (keys->size() != values.size())
        throw Error("trace %d: key count %d != value count %d",
                    traceId.value, keys->size(), values.size());

    // Zip keys + values into Dep vector (no string resolution)
    auto result = std::make_shared<std::vector<Dep>>();
    result->reserve(keys->size());
    for (size_t i = 0; i < keys->size(); ++i)
        result->push_back({(*keys)[i], std::move(values[i])});

    // Also populate the dep key set cache for potential recovery use
    depKeySetCache.insert_or_assign(depKeySetId, keys);

    traceCache.insert_or_assign(traceId, TraceCacheEntry{header, result});
    nrLoadTraceTimeUs += elapsedUs(loadStart);
    return result;
}

std::shared_ptr<const std::vector<Dep::Key>> SqliteTraceStorage::loadKeySet(
    const ExclusiveTraceStorageAccess & ea, DepKeySetId depKeySetId)
{
    auto loadStart = timerStart();
    nrLoadKeySets++;

    // Session cache: avoid re-decompressing keys_blob for shared key sets
    auto cacheIt = depKeySetCache.find(depKeySetId);
    if (cacheIt != depKeySetCache.end()) {
        nrLoadKeySetCacheHits++;
        nrLoadKeySetTimeUs += elapsedUs(loadStart);
        return cacheIt->second;
    }
    nrLoadKeySetCacheMisses++;

    auto & st = *_state;
    auto use(st.getDepKeySet.use()(static_cast<int64_t>(depKeySetId.value)));
    if (!use.next()) {
        nrLoadKeySetTimeUs += elapsedUs(loadStart);
        return nullptr;
    }

    // Column 0 = key_set_hash (not needed here), column 1 = keys_blob
    auto [keysData, keysSize] = use.getBlob(1);
    if (!keysData || keysSize == 0) {
        auto keys = std::make_shared<const std::vector<Dep::Key>>();
        depKeySetCache.insert_or_assign(depKeySetId, keys);
        nrLoadKeySetTimeUs += elapsedUs(loadStart);
        return keys;
    }

    std::vector<uint8_t> blobCopy(
        static_cast<const uint8_t *>(keysData),
        static_cast<const uint8_t *>(keysData) + keysSize);

    auto keys = std::make_shared<std::vector<Dep::Key>>(
        deserializeKeys(blobCopy.data(), blobCopy.size()));
    depKeySetCache.insert_or_assign(depKeySetId, keys);
    nrLoadKeySetTimeUs += elapsedUs(loadStart);
    return keys;
}

std::vector<HistoryEntry> SqliteTraceStorage::scanHistory(
    const ExclusiveTraceStorageAccess & ea,
    AttrPathId pathId)
{
    auto scanStart = timerStart();
    nrRecoveryScanHistoryCount++;
    std::vector<HistoryEntry> entries;
    auto & st = *_state;
    auto use(st.scanHistoryForAttr.use());
    bindTaggedEvalTraceHash(use, currentStableRecoveryKey());
    use(static_cast<int64_t>(pathId.value));
    while (use.next()) {
        auto [thData, thSize] = use.getBlob(3);
        entries.push_back({
            DepKeySetId(static_cast<uint32_t>(use.getInt(0))),
            TraceId(static_cast<uint32_t>(use.getInt(1))),
            ResultId(static_cast<uint32_t>(use.getInt(2))),
            evalTraceHashFromBlob<TraceHash>(thData, thSize),
        });
    }
    nrRecoveryScanHistoryRows += entries.size();
    nrRecoveryScanHistoryUs += elapsedUs(scanStart);
    return entries;
}

std::optional<SqliteTraceStorage::CurrentNodeRef> SqliteTraceStorage::lookupLatestHistoryForAttr(
    const ExclusiveTraceStorageAccess & ea, AttrPathId pathId)
{
    auto lookupStart = timerStart();
    nrRecoveryLatestHistoryLookupCount++;
    auto & st = *_state;
    auto use(st.lookupLatestHistoryForAttr.use());
    bindTaggedEvalTraceHash(use, currentStableRecoveryKey());
    use(static_cast<int64_t>(pathId.value));
    if (!use.next()) {
        nrRecoveryLatestHistoryLookupUs += elapsedUs(lookupStart);
        return std::nullopt;
    }
    nrRecoveryLatestHistoryLookupUs += elapsedUs(lookupStart);
    return CurrentNodeRef{
        .traceId = TraceId(static_cast<uint32_t>(use.getInt(0))),
        .resultId = ResultId(static_cast<uint32_t>(use.getInt(1))),
    };
}

std::optional<SqliteTraceStorage::TraceKeysAndHeader> SqliteTraceStorage::loadTraceKeysAndHeader(const ExclusiveTraceStorageAccess & ea, TraceId traceId)
{
    // If already fully loaded, extract keys from the full cache
    if (auto it = traceCache.find(traceId); it != traceCache.end() && it->second.full) {
        auto keys = loadKeySet(ea, it->second.header.depKeySetId);
        if (keys)
            return TraceKeysAndHeader{it->second.header, std::move(keys)};

        auto fallbackKeys = std::make_shared<std::vector<Dep::Key>>();
        fallbackKeys->reserve(it->second.full->size());
        for (auto & dep : *it->second.full)
            fallbackKeys->push_back(dep.key);
        return TraceKeysAndHeader{it->second.header, std::move(fallbackKeys)};
    }

    // Query DB for trace info
    std::vector<uint8_t> keysBlobCopy, valuesBlobCopy;
    DepKeySetId depKeySetId;
    TraceHash traceHash{};
    DepKeySetHash keySetHash{};
    {
        auto & st = *_state;
        auto use(st.getTraceInfo.use()(static_cast<int64_t>(traceId.value)));
        if (!use.next())
            return std::nullopt;

        auto [thData, thSize] = use.getBlob(0);
        traceHash = evalTraceHashFromBlob<TraceHash>(thData, thSize);
        auto [kshData, kshSize] = use.getBlob(1);
        keySetHash = evalTraceHashFromBlob<DepKeySetHash>(kshData, kshSize);
        depKeySetId = DepKeySetId(static_cast<uint32_t>(use.getInt(2)));

        auto [keysData, keysSize] = use.getBlob(3);
        if (keysData && keysSize > 0) {
            auto * p = static_cast<const uint8_t *>(keysData);
            keysBlobCopy.assign(p, p + keysSize);
        }

        auto [valsData, valsSize] = use.getBlob(4);
        if (valsData && valsSize > 0) {
            auto * p = static_cast<const uint8_t *>(valsData);
            valuesBlobCopy.assign(p, p + valsSize);
        }
    }

    TraceHeader header{
        .traceHash = traceHash,
        .keySetHash = keySetHash,
        .depKeySetId = depKeySetId,
    };
    traceCache[traceId].header = header;

    if (keysBlobCopy.empty()) {
        auto keys = std::make_shared<const std::vector<Dep::Key>>();
        depKeySetCache.insert_or_assign(depKeySetId, keys);
        return TraceKeysAndHeader{header, std::move(keys)};
    }

    // Deserialize keys only — stash raw values blob for deferred loading
    auto keys = std::make_shared<std::vector<Dep::Key>>(
        deserializeKeys(keysBlobCopy.data(), keysBlobCopy.size()));
    depKeySetCache.insert_or_assign(depKeySetId, keys);

    if (!valuesBlobCopy.empty()) {
        deferredTraceBlobs.insert_or_assign(traceId,
            DeferredTraceBlob{depKeySetId, std::move(valuesBlobCopy)});
    }

    return TraceKeysAndHeader{header, std::move(keys)};
}

// feedKey is inherited from VocabAwareHasher (protected mixin member).
// `computeSortedTraceHash` / `computePresortedTraceHash` below use it
// via an unqualified name lookup, which finds the mixin's protected
// `feedKey` because SqliteTraceStorage privately inherits VocabAwareHasher.

// ── Dedup helpers (A1–A4) ────────────────────────────────────────────

TraceHash SqliteTraceStorage::computeSortedTraceHash(std::vector<Dep> & deps) const
{
    std::sort(deps.begin(), deps.end());
    deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    auto feeder = [this](CanonicalHashBuilder & builder, const Dep::Key & key) {
        feedKey(builder, key);
    };
    return computeTraceHashFromSorted(deps, feeder);
}

TraceHash SqliteTraceStorage::computePresortedTraceHash(const std::vector<Dep> & deps) const
{
    auto feeder = [this](CanonicalHashBuilder & builder, const Dep::Key & key) {
        feedKey(builder, key);
    };
    return computeTraceHashFromSorted(deps, feeder);
}

DepKeySetId SqliteTraceStorage::getOrCreateDepKeySet(
    const DepKeySetHash & keySetHash,
    const std::vector<uint8_t> & keysBlob)
{
    auto it = depKeySetByHash.find(keySetHash);
    if (it != depKeySetByHash.end())
        return it->second;

    auto id = DepKeySetId(++nextDepKeySetId.value);
    depKeySetByHash[keySetHash] = id;
    pendingDepKeySets.push_back({id, keySetHash, keysBlob});
    return id;
}

TraceId SqliteTraceStorage::getOrCreateTrace(
    const TraceHash & traceHash,
    const FullTraceHash & fullHash,
    DepKeySetId depKeySetId,
    const std::vector<uint8_t> & valuesBlob)
{
    auto it = traceByFullHash.find(fullHash);
    if (it != traceByFullHash.end())
        return it->second;

    auto id = TraceId(++nextTraceId.value);
    traceByFullHash[fullHash] = id;
    pendingTraces.push_back({id, traceHash, fullHash, depKeySetId, valuesBlob});
    return id;
}

// ── DB lookups ───────────────────────────────────────────────────────

std::optional<SqliteTraceStorage::CurrentNodeRef> SqliteTraceStorage::lookupCurrentNode(const gdp::Proof<BlockingTag> & bs, AttrPathId pathId)
{
    // Caller holds storeMutex_ via withExclusiveAccess — no inner lock_guard.
    // Check currentNodeIndex first (populated on previous lookups, replaced on
    // current-state publication in recovery and record).
    auto rowCacheIt = currentNodeIndex.find(pathId);
    if (rowCacheIt != currentNodeIndex.end())
        return rowCacheIt->second;

    auto & st = *_state;

    auto use(st.lookupAttr.use());
    bindEvalTraceHash(use, currentSemanticSessionKey().digest);
    use(static_cast<int64_t>(pathId.value));
    if (!use.next())
        return std::nullopt;

    CurrentNodeRef row;
    row.traceId = TraceId(static_cast<uint32_t>(use.getInt(0)));
    row.resultId = ResultId(static_cast<uint32_t>(use.getInt(1)));
    row.nodeStamp = NodeStamp(static_cast<uint32_t>(use.getInt(2)));
    if (!row.nodeStamp.value) {
        row.nodeStamp = allocateNodeStamp();
        auto upsert(st.upsertAttr.use());
        bindEvalTraceHash(upsert, currentSemanticSessionKey().digest);
        upsert(static_cast<int64_t>(pathId.value))
            (static_cast<int64_t>(row.traceId.value))
            (static_cast<int64_t>(row.resultId.value))
            (static_cast<int64_t>(row.nodeStamp.value))
            .exec();
    }

    currentNodeIndex[pathId] = row;
    return row;
}

const SqliteTraceStorage::ResultPayload & SqliteTraceStorage::loadResultPayloadCached(const gdp::Proof<BlockingTag> &, ResultId resultId)
{
    // Caller holds storeMutex_ via withExclusiveAccess — no inner lock_guard.
    // Ref-returning variant for the hot decode path: returns directly into
    // `resultPayloadCache` without copying. The public virtual
    // `loadResultPayload(ea, id)` populates the same cache and returns
    // by value; hot callers use this helper to avoid the copy.
    auto cacheIt = resultPayloadCache.find(resultId);
    if (cacheIt != resultPayloadCache.end())
        return cacheIt->second;

    auto & st = *_state;
    auto use(st.getResult.use()(static_cast<int64_t>(resultId.value)));
    if (!use.next())
        throw Error("result %d not found (DB corruption)", resultId.value);

    ResultPayload payload;
    payload.type = static_cast<ResultKind>(use.getInt(0));
    // Columns: 0=type, 1=payload, 2=aux_context (encoding column dropped
    // per P-SK in epoch v25).  `encodingVersion` is the global
    // `kSemanticResultEncodingVersion` — see trace-result-codec.hh.
    payload.encodingVersion = kSemanticResultEncodingVersion;
    payload.payload = use.isNull(1) ? "" : use.getStr(1);
    payload.auxContext = use.isNull(2) ? "" : use.getStr(2);

    auto [it, _] = resultPayloadCache.insert_or_assign(resultId, std::move(payload));
    return it->second;
}

bool SqliteTraceStorage::attrExists(const ExclusiveTraceStorageAccess & ea, AttrPathId pathId)
{
    return lookupCurrentNode(ea.blockingProof(), pathId).has_value();
}

std::optional<TraceHash> SqliteTraceStorage::getCurrentTraceHash(const ExclusiveTraceStorageAccess & ea, AttrPathId pathId)
{
    auto & bs = ea.blockingProof();
    auto row = lookupCurrentNode(bs, pathId);  // hits currentNodeIndex after first call
    if (!row) return std::nullopt;

    // Return canonical trace_hash, not result hash. Result hash for attrsets
    // only captures attribute names, not values; trace_hash captures
    // trace-hash-contributing deps while excluding implicit structural guards.
    auto * data = ensureTraceHeader(bs, row->traceId);  // hits traceCache after first call
    if (!data) return std::nullopt;
    return data->traceHash;
}

// allocateNodeStamp is inherited from TraceStorage (atomic-backed).

// ── Record path (BSàlC constructive trace recording) ─────────────────
//
// Each trace records ONLY its own deps — no parent dep inheritance.
// A parent attrset's trace covers shape (attribute names, aliases,
// container metadata). Children's file/env/store deps live in
// children's traces exclusively. This separation is what enables
// selective sibling invalidation: changing sibling B's file changes
// B's trace but not the parent's, so sibling A (which doesn't access
// B) remains valid.

SqliteTraceStorage::RecordResult SqliteTraceStorage::record(
    const ExclusiveTraceStorageAccess & ea,
    AttrPathId pathId,
    const CachedResult & value,
    const std::vector<Dep> & allDeps)
{
    // Thin delegate — the pipeline lives in `Recorder`
    // (rearchitecture-proposal.md §14 step 7 + §2.3).
    return Recorder{*this, pools, vocab}.record(ea, pathId, value, allDeps);
}

SqliteTraceStorage::CurrentNodeRef SqliteTraceStorage::publishRecord(
    const gdp::Proof<BlockingTag> & bs,
    AttrPathId pathId, TraceId traceId, ResultId resultId,
    TraceHeader header, std::vector<Dep> fullDeps,
    DepKeySetId depKeySetId, std::vector<Dep::Key> keys)
{
    // Use the free-fn form (deps/analysis.hh). Unqualified lookup inside
    // a SqliteTraceStorage member would otherwise resolve to the deprecated
    // member shim, which this commit is removing.
    auto recoveryIndexHash = eval_trace::extractGitIdentityHash(fullDeps);
    // Unwrap StoredGitIdentityHash to raw EvalTraceHash for the type-agnostic SQL layer.
    // extractGitIdentityHash returns StoredGitIdentityHash (phantom-typed) to
    // prevent BUG-1 confusion; publishStateChange accepts raw bytes only.
    auto rawRecoveryHash = recoveryIndexHash
        ? std::optional{recoveryIndexHash->value}
        : std::nullopt;

    auto ref = publishStateChange(bs, pathId, traceId, resultId, /*insertHistory=*/true,
        rawRecoveryHash);
    traceCache.insert_or_assign(traceId, TraceCacheEntry{
        std::move(header),
        std::make_shared<const std::vector<Dep>>(std::move(fullDeps)),
    });
    depKeySetCache.insert_or_assign(
        depKeySetId,
        std::make_shared<const std::vector<Dep::Key>>(std::move(keys)));
    return ref;
}

SqliteTraceStorage::CurrentNodeRef SqliteTraceStorage::publishStateChange(
    const gdp::Proof<BlockingTag> & bs,
    AttrPathId pathId, TraceId traceId, ResultId resultId,
    bool insertHistory,
    std::optional<EvalTraceHash> gitIdentityHash)
{
    auto nodeStamp = allocateNodeStamp();
    {
        auto & st = *_state;
        SQLiteTxn txn(st.db);
        auto upsert(st.upsertAttr.use());
        bindEvalTraceHash(upsert, currentSemanticSessionKey().digest);
        upsert(static_cast<int64_t>(pathId.value))
            (static_cast<int64_t>(traceId.value))
            (static_cast<int64_t>(resultId.value))
            (static_cast<int64_t>(nodeStamp.value))
            .exec();
        if (insertHistory) {
            auto use(st.insertHistory.use());
            bindTaggedEvalTraceHash(use, currentStableRecoveryKey());
            use(static_cast<int64_t>(pathId.value));
            use(static_cast<int64_t>(traceId.value));
            use(static_cast<int64_t>(resultId.value));
            if (gitIdentityHash)
                use(gitIdentityHash->data(), gitIdentityHash->size());
            else
                use.bind();  // NULL
            use.exec();
        }
        txn.commit();
    }
    CurrentNodeRef ref{traceId, resultId, nodeStamp};
    currentNodeIndex[pathId] = ref;
    return ref;
}

void SqliteTraceStorage::patchTraceHashInMemory(const gdp::Proof<BlockingTag> & bs, TraceId traceId, TraceHash newTraceHash)
{
    auto * data = ensureTraceHeader(bs, traceId);
    if (data) {
        data->traceHash = newTraceHash;
    }
}

// ── SessionRuntimeRoots ──────────────────────────────────────────────

void SqliteTraceStorage::recordRuntimeRoot(
    const ExclusiveTraceStorageAccess & ea,
    const RuntimeRootRecord & record,
    Store & store)
{
    auto & st = *_state;
    auto use(st.insertRuntimeRoot.use());
    bindEvalTraceHash(use, currentSemanticSessionKey().digest);
    bindBlob(use, encodeDepSourceBlob(record.source).value);
    bindBlob(use, encodeRuntimeFetchIdentityDepKey(record.fetchIdentity).value);
    bindBlob(use, encodePersistedHashBlob(record.narHash.value).value);
    bindRuntimeRootStorePath(use, store, record.storePath);
    use.exec();
}

SqliteTraceStorage::RuntimeRootLoadResult SqliteTraceStorage::loadRuntimeRoots(
    const ExclusiveTraceStorageAccess & ea,
    Store & store)
{
    RuntimeRootLoadResult loaded;
    auto & st = *_state;
    auto use(st.loadRuntimeRoots.use());
    bindEvalTraceHash(use, currentSemanticSessionKey().digest);
    while (use.next()) {
        loaded.storedCount++;
        try {
            auto [sourceBlob, sourceLen] = use.getBlob(0);
            auto source = decodeRuntimeRootSourceBlob(std::string_view(
                reinterpret_cast<const char *>(sourceBlob ? sourceBlob : ""),
                sourceLen));
            auto [fetchIdentityBlob, fetchIdentityLen] = use.getBlob(1);
            auto [narHashBlob, narHashLen] = use.getBlob(2);
            auto [storePathBlob, storePathLen] = use.getBlob(3);
            loaded.entries.push_back(RuntimeRootRecord{
                .source = std::move(source),
                .fetchIdentity = decodeRuntimeRootFetchIdentityBlob(std::string_view(
                    reinterpret_cast<const char *>(fetchIdentityBlob ? fetchIdentityBlob : ""),
                    fetchIdentityLen)),
                .narHash = decodeRuntimeRootNarHashBlob(std::string_view(
                    reinterpret_cast<const char *>(narHashBlob ? narHashBlob : ""),
                    narHashLen)),
                .storePath = decodeRuntimeRootStorePathBlob(store, std::string_view(
                    reinterpret_cast<const char *>(storePathBlob ? storePathBlob : ""),
                    storePathLen)),
            });
        } catch (...) {
            loaded.rejectedCount++;
        }
    }
    return loaded;
}

// ── TraceStorage virtual overrides (§2.1) ──────────────────────────

ResultId SqliteTraceStorage::insertResult(
    const ExclusiveTraceStorageAccess &,
    ResultHash hash, EncodedResultPayload && payload)
{
    return doInternResult(payload, hash);
}

DepKeySetId SqliteTraceStorage::insertDepKeySet(
    const ExclusiveTraceStorageAccess &,
    DepKeySetHash hash, std::vector<uint8_t> && keysBlob)
{
    return getOrCreateDepKeySet(hash, keysBlob);
}

TraceId SqliteTraceStorage::insertTrace(
    const ExclusiveTraceStorageAccess &,
    FullTraceHash fullHash, TraceHeader header,
    std::vector<uint8_t> && valuesBlob)
{
    return getOrCreateTrace(header.traceHash, fullHash, header.depKeySetId, valuesBlob);
}

std::optional<SqliteTraceStorage::TraceHeader>
SqliteTraceStorage::loadTraceHeader(
    const ExclusiveTraceStorageAccess & ea, TraceId traceId)
{
    if (auto * ptr = ensureTraceHeader(ea.blockingProof(), traceId))
        return *ptr;
    return std::nullopt;
}

std::optional<TraceBlobs>
SqliteTraceStorage::loadTraceBlobs(
    const ExclusiveTraceStorageAccess & ea, TraceId traceId)
{
    auto & bs = ea.blockingProof();
    auto * headerPtr = ensureTraceHeader(bs, traceId);
    if (!headerPtr)
        return std::nullopt;

    auto & st = *_state;
    auto use(st.getTraceInfo.use()(static_cast<int64_t>(traceId.value)));
    if (!use.next())
        return std::nullopt;
    // getTraceInfo columns: 0=trace_hash, 1=key_set_hash, 2=dep_key_set_id, 3=keys_blob, 4=values_blob
    auto [keysData, keysSize] = use.getBlob(3);
    auto [valuesData, valuesSize] = use.getBlob(4);
    std::vector<uint8_t> keysBlob;
    if (keysData && keysSize)
        keysBlob.assign(
            reinterpret_cast<const uint8_t *>(keysData),
            reinterpret_cast<const uint8_t *>(keysData) + keysSize);
    std::vector<uint8_t> valuesBlob;
    if (valuesData && valuesSize)
        valuesBlob.assign(
            reinterpret_cast<const uint8_t *>(valuesData),
            reinterpret_cast<const uint8_t *>(valuesData) + valuesSize);
    return TraceBlobs{
        .header = *headerPtr,
        .keysBlob = std::move(keysBlob),
        .valuesBlob = std::move(valuesBlob),
    };
}

std::optional<std::vector<uint8_t>>
SqliteTraceStorage::loadKeysBlob(
    const ExclusiveTraceStorageAccess &, DepKeySetId depKeySetId)
{
    auto & st = *_state;
    auto use(st.getDepKeySet.use()(static_cast<int64_t>(depKeySetId.value)));
    if (!use.next())
        return std::nullopt;
    auto [data, size] = use.getBlob(0);
    if (!data || !size)
        return std::vector<uint8_t>{};
    return std::vector<uint8_t>(
        reinterpret_cast<const uint8_t *>(data),
        reinterpret_cast<const uint8_t *>(data) + size);
}

std::optional<SqliteTraceStorage::ResultPayload>
SqliteTraceStorage::loadResultPayload(
    const ExclusiveTraceStorageAccess & ea, ResultId resultId)
{
    // Virtual contract: benign miss signal for unknown IDs. The
    // ref-returning `loadResultPayloadCached` helper throws on miss
    // (it's called on the hot decode path where a missing row is a
    // DB-corruption bug, not an input-validation concern). Translate
    // the throw into `std::nullopt` here.
    try {
        return loadResultPayloadCached(ea.blockingProof(), resultId);
    } catch (Error &) {
        return std::nullopt;
    }
}

std::optional<CurrentNodeRef>
SqliteTraceStorage::lookupCurrent(
    const ExclusiveTraceStorageAccess & ea, AttrPathId pathId)
{
    return lookupCurrentNode(ea.blockingProof(), pathId);
}

std::optional<HistoryNodeRef>
SqliteTraceStorage::lookupLatestHistory(
    const ExclusiveTraceStorageAccess & ea, AttrPathId pathId)
{
    auto row = lookupLatestHistoryForAttr(ea, pathId);
    if (!row)
        return std::nullopt;
    // The legacy `lookupLatestHistoryForAttr` returns a `CurrentNodeRef`
    // with `nodeStamp = 0` for History rows (they have no Sessions
    // stamp). Strip the sentinel to produce the narrower type.
    return HistoryNodeRef{row->traceId, row->resultId};
}

std::vector<HistoryEntry>
SqliteTraceStorage::queryAllHistory(
    const ExclusiveTraceStorageAccess & ea, AttrPathId pathId)
{
    return scanHistory(ea, pathId);
}

std::vector<HistoryEntry>
SqliteTraceStorage::queryHistoryByTraceHash(
    const ExclusiveTraceStorageAccess &, AttrPathId pathId, TraceHash traceHash)
{
    // `lookupHistoryByTraceHash` SELECTs 3 columns — every row matches
    // the input `traceHash` by construction, so fill `entry.traceHash`
    // from the argument rather than re-reading it from the row.
    std::vector<HistoryEntry> out;
    auto & st = *_state;
    auto use(st.lookupHistoryByTraceHash.use());
    bindTaggedEvalTraceHash(use, currentStableRecoveryKey());
    use(static_cast<int64_t>(pathId.value));
    bindTaggedEvalTraceHash(use, traceHash);
    while (use.next()) {
        HistoryEntry entry;
        entry.depKeySetId = DepKeySetId(static_cast<uint32_t>(use.getInt(0)));
        entry.traceId = TraceId(static_cast<uint32_t>(use.getInt(1)));
        entry.resultId = ResultId(static_cast<uint32_t>(use.getInt(2)));
        entry.traceHash = traceHash;
        out.push_back(entry);
    }
    return out;
}

std::vector<HistoryEntry>
SqliteTraceStorage::queryHistoryByGitIdentity(
    const ExclusiveTraceStorageAccess &, AttrPathId pathId,
    CurrentGitIdentityHash gitHash)
{
    // `lookupHistoryByGitIdentity` SELECTs 3 columns and does not join
    // `t.trace_hash`; to populate `entry.traceHash` we need a separate
    // `getTraceInfo` lookup per row. This path is on the cold recovery
    // trail (GitIdentity-indexed recovery), not the hot SV scan.
    std::vector<HistoryEntry> out;
    auto & st = *_state;
    auto use(st.lookupHistoryByGitIdentity.use());
    bindTaggedEvalTraceHash(use, currentStableRecoveryKey());
    use(static_cast<int64_t>(pathId.value));
    use(gitHash.value.data(), gitHash.value.size());
    while (use.next()) {
        HistoryEntry entry;
        entry.depKeySetId = DepKeySetId(static_cast<uint32_t>(use.getInt(0)));
        entry.traceId = TraceId(static_cast<uint32_t>(use.getInt(1)));
        entry.resultId = ResultId(static_cast<uint32_t>(use.getInt(2)));
        out.push_back(entry);
    }
    // Backfill trace_hash per row via getTraceInfo (deferred until
    // after the statement finalises — keeps the two prepared-statement
    // uses serialised).
    for (auto & entry : out) {
        auto infoUse(st.getTraceInfo.use()(static_cast<int64_t>(entry.traceId.value)));
        if (infoUse.next()) {
            auto [thData, thSize] = infoUse.getBlob(0);
            entry.traceHash = evalTraceHashFromBlob<TraceHash>(thData, thSize);
        }
    }
    return out;
}

CurrentNodeRef
SqliteTraceStorage::publishFreshRecord(
    const ExclusiveTraceStorageAccess & ea,
    AttrPathId pathId, TraceId traceId, ResultId resultId,
    std::optional<EvalTraceHash> gitIdentityHash)
{
    return publishStateChange(
        ea.blockingProof(), pathId, traceId, resultId,
        /*insertHistory=*/true, gitIdentityHash);
}

CurrentNodeRef
SqliteTraceStorage::publishHistoryBootstrap(
    const ExclusiveTraceStorageAccess & ea,
    AttrPathId pathId, TraceId traceId, ResultId resultId)
{
    return publishStateChange(
        ea.blockingProof(), pathId, traceId, resultId,
        /*insertHistory=*/false);
}

} // namespace nix::eval_trace
