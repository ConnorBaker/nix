#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/eval-trace/deps/hash.hh"

#include "trace-serialize.hh"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <tuple>

namespace nix::eval_trace {

// ── Timing helpers (no-op when NIX_SHOW_STATS is unset) ──────────────

static auto timerStart()
{
    return Counter::enabled ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};
}

static uint64_t elapsedUs(std::chrono::steady_clock::time_point start)
{
    if (!Counter::enabled) return 0;
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
}

// ── Dep key/value resolution ─────────────────────────────────────────

TraceStore::ResolvedDep TraceStore::resolveDep(const Dep & dep)
{
    if (dep.key.type == DepType::ParentContext) {
        auto pathId = AttrPathId(dep.key.keyId.value);
        return ResolvedDep{"", vocab.displayPath(pathId), dep.hash, DepType::ParentContext};
    }
    return ResolvedDep{
        std::string(pools.resolve(dep.key.sourceId)),
        std::string(pools.resolve(dep.key.keyId)),
        dep.hash,
        dep.key.type};
}


// ── Intern methods ───────────────────────────────────────────────────

ResultId TraceStore::doInternResult(ResultKind type, const std::string & value,
                                     const std::string & context, const ResultHash & resultHash)
{
    auto it = resultByHash.find(resultHash);
    if (it != resultByHash.end())
        return it->second;

    auto id = ResultId(++nextResultId.value);
    resultByHash[resultHash] = id;
    pendingResults.push_back({id, type, value, context, resultHash});
    return id;
}

// ── Trace storage (BSàlC trace store) ───────────────────────────────


TraceStore::CachedTraceData * TraceStore::ensureTraceHashes(TraceId traceId)
{
    auto it = traceDataCache.find(traceId);
    if (it != traceDataCache.end())
        return &it->second;

    auto st(_state->lock());
    // getTraceInfo columns: 0=trace_hash, 1=struct_hash, 2=dep_key_set_id, 3=keys_blob, 4=values_blob
    auto use(st->getTraceInfo.use()(static_cast<int64_t>(traceId.value)));
    if (!use.next())
        return nullptr;

    CachedTraceData data;
    auto [thData, thSize] = use.getBlob(0);
    data.traceHash = TraceHash::fromBlob(thData, thSize);
    auto [shData, shSize] = use.getBlob(1);
    data.structHash = StructHash::fromBlob(shData, shSize);
    // deps left as nullopt (populated lazily by loadFullTrace)

    // Sanity: DB should never contain all-zero hashes (placeholder sentinel).
    assert(data.hashesPopulated() && "deserialized trace has placeholder (all-zero) hashes");

    auto [insertIt, _] = traceDataCache.emplace(traceId, std::move(data));
    return &insertIt->second;
}

StructHash TraceStore::getTraceStructHash(TraceId traceId)
{
    auto * data = ensureTraceHashes(traceId);
    if (!data)
        throw Error("trace %d not found", traceId.value);
    return data->structHash;
}

std::vector<Dep> TraceStore::loadFullTrace(TraceId traceId)
{
    // Check if deps already cached in unified traceDataCache
    auto it = traceDataCache.find(traceId);
    if (it != traceDataCache.end() && it->second.deps)
        return *it->second.deps;

    auto loadStart = timerStart();
    nrLoadTraces++;

    // Single DB read via JOIN — keys from DepKeySets, values from Traces
    // getTraceInfo columns: 0=trace_hash, 1=struct_hash, 2=dep_key_set_id, 3=keys_blob, 4=values_blob
    std::vector<uint8_t> keysBlobCopy, valuesBlobCopy;
    DepKeySetId depKeySetId;
    TraceHash traceHash{};
    StructHash structHash{};
    {
        auto st(_state->lock());
        auto use(st->getTraceInfo.use()(static_cast<int64_t>(traceId.value)));
        if (!use.next()) {
            // Trace not found in DB — don't create a cache entry with
            // placeholder hashes, as ensureTraceHashes would incorrectly
            // return them as valid.
            nrLoadTraceTimeUs += elapsedUs(loadStart);
            return {};
        }

        // Opportunistically populate hash fields from the same query
        auto [thData, thSize] = use.getBlob(0);
        traceHash = TraceHash::fromBlob(thData, thSize);
        auto [shData, shSize] = use.getBlob(1);
        structHash = StructHash::fromBlob(shData, shSize);
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

    // Update hash fields in cache (may already exist from ensureTraceHashes)
    auto & data = traceDataCache[traceId];
    data.traceHash = traceHash;
    data.structHash = structHash;
    assert(data.hashesPopulated() && "DB returned placeholder (all-zero) hashes in loadFullTrace");

    if (keysBlobCopy.empty()) {
        data.deps = {};
        nrLoadTraceTimeUs += elapsedUs(loadStart);
        return {};
    }

    // Deserialize keys first (needed to type-dispatch values), then values
    auto keys = deserializeKeys(keysBlobCopy.data(), keysBlobCopy.size());
    auto values = deserializeValues(valuesBlobCopy.data(), valuesBlobCopy.size(), keys);

    // Zip keys + values into Dep vector (no string resolution)
    std::vector<Dep> result;
    result.reserve(keys.size());
    for (size_t i = 0; i < std::min(keys.size(), values.size()); ++i)
        result.push_back({keys[i], std::move(values[i])});

    // Also populate the dep key set cache for potential recovery use
    depKeySetCache.insert_or_assign(depKeySetId, keys);

    data.deps = result;
    nrLoadTraceTimeUs += elapsedUs(loadStart);
    return result;
}

std::vector<Dep::Key> TraceStore::loadKeySet(DepKeySetId depKeySetId)
{
    // Session cache: avoid re-decompressing keys_blob for shared key sets
    auto cacheIt = depKeySetCache.find(depKeySetId);
    if (cacheIt != depKeySetCache.end())
        return cacheIt->second;

    auto st(_state->lock());
    auto use(st->getDepKeySet.use()(static_cast<int64_t>(depKeySetId.value)));
    if (!use.next())
        return {};

    // Column 0 = struct_hash (not needed here), column 1 = keys_blob
    auto [keysData, keysSize] = use.getBlob(1);
    if (!keysData || keysSize == 0)
        return {};

    std::vector<uint8_t> blobCopy(
        static_cast<const uint8_t *>(keysData),
        static_cast<const uint8_t *>(keysData) + keysSize);

    auto keys = deserializeKeys(blobCopy.data(), blobCopy.size());
    depKeySetCache.insert_or_assign(depKeySetId, keys);
    return keys;
}

void TraceStore::feedKey(HashSink & s, DepType type, uint32_t idValue) const
{
    if (depKind(type) == DepKind::ParentContext)
        vocab.hashPath(s, AttrPathId(idValue));
    else
        s(pools.resolve(DepKeyId(idValue)));
}

// ── Dedup helpers (A1–A4) ────────────────────────────────────────────

TraceHash TraceStore::computeSortedTraceHash(std::vector<Dep> & deps) const
{
    std::sort(deps.begin(), deps.end());
    deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    auto feeder = [this](HashSink & s, DepType type, uint32_t idValue) {
        feedKey(s, type, idValue);
    };
    return computeTraceHashFromSorted(deps, feeder);
}

TraceHash TraceStore::computePresortedTraceHash(const std::vector<Dep> & deps) const
{
    auto feeder = [this](HashSink & s, DepType type, uint32_t idValue) {
        feedKey(s, type, idValue);
    };
    return computeTraceHashFromSorted(deps, feeder);
}

DepKeySetId TraceStore::getOrCreateDepKeySet(
    const StructHash & structHash,
    const std::vector<uint8_t> & keysBlob)
{
    auto it = depKeySetByStructHash.find(structHash);
    if (it != depKeySetByStructHash.end())
        return it->second;

    auto id = DepKeySetId(++nextDepKeySetId.value);
    depKeySetByStructHash[structHash] = id;
    pendingDepKeySets.push_back({id, structHash, keysBlob});
    return id;
}

TraceId TraceStore::getOrCreateTrace(
    const TraceHash & traceHash,
    DepKeySetId depKeySetId,
    const std::vector<uint8_t> & valuesBlob)
{
    auto it = traceByTraceHash.find(traceHash);
    if (it != traceByTraceHash.end())
        return it->second;

    auto id = TraceId(++nextTraceId.value);
    traceByTraceHash[traceHash] = id;
    pendingTraces.push_back({id, traceHash, depKeySetId, valuesBlob});
    return id;
}

// ── DB lookups ───────────────────────────────────────────────────────

std::optional<TraceStore::TraceRow> TraceStore::lookupTraceRow(AttrPathId pathId)
{
    // Check traceRowCache first (populated on previous lookups, invalidated on
    // CurrentTraces changes in acceptRecoveredTrace and record).
    auto rowCacheIt = traceRowCache.find(pathId);
    if (rowCacheIt != traceRowCache.end())
        return rowCacheIt->second;

    auto st(_state->lock());

    auto use(st->lookupAttr.use()(contextHash)(static_cast<int64_t>(pathId.value)));
    if (!use.next())
        return std::nullopt;

    TraceRow row;
    row.traceId = TraceId(static_cast<uint32_t>(use.getInt(0)));
    row.resultId = ResultId(static_cast<uint32_t>(use.getInt(1)));
    row.type = static_cast<ResultKind>(use.getInt(2));
    row.value = use.isNull(3) ? "" : use.getStr(3);
    row.context = use.isNull(4) ? "" : use.getStr(4);

    traceRowCache[pathId] = row;
    return row;
}

bool TraceStore::attrExists(AttrPathId pathId)
{
    return lookupTraceRow(pathId).has_value();
}

std::optional<TraceHash> TraceStore::getCurrentTraceHash(AttrPathId pathId)
{
    auto row = lookupTraceRow(pathId);  // hits traceRowCache after first call
    if (!row) return std::nullopt;

    // Return trace_hash (captures dep structure + hashes), not result hash.
    // Result hash for attrsets only captures attribute names, not values —
    // it wouldn't detect changes to attribute values within an attrset.
    auto * data = ensureTraceHashes(row->traceId);  // hits traceDataCache after first call
    if (!data) return std::nullopt;
    return data->traceHash;
}

// ── Record path (BSàlC constructive trace recording) ─────────────────

TraceStore::RecordResult TraceStore::record(
    AttrPathId pathId,
    const CachedResult & value,
    const std::vector<Dep> & allDeps)
{
    auto recordStart = timerStart();
    nrRecords++;

    // 1. Sort+dedup deps by key (type, sourceId, keyId)
    auto sorted = sortAndDedupDeps(allDeps);

    // 2. Compute trace_hash and struct_hash
    auto feedKeyFn = [this](HashSink & s, DepType type, uint32_t idValue) {
        feedKey(s, type, idValue);
    };
    auto traceHash = computeTraceHashFromSorted(sorted, feedKeyFn);
    auto structHash = computeTraceStructHashFromSorted(sorted, feedKeyFn);

    // 5. Split into keys + values
    std::vector<Dep::Key> keys;
    keys.reserve(sorted.size());
    for (auto & d : sorted)
        keys.push_back(d.key);

    auto keysBlob = serializeKeys(keys);
    auto valuesBlob = serializeValues(sorted);

    // 6. Get or create dep key set (content-addressed by struct_hash)
    auto depKeySetId = getOrCreateDepKeySet(structHash, keysBlob);

    // 7. Encode CachedResult and intern result
    auto [type, val, ctx] = encodeCachedResult(value);
    auto resultHash = computeResultHash(type, val, ctx);
    ResultId resultId = doInternResult(type, val, ctx, resultHash);

    // 8. Get or create trace (keyed by trace_hash, stores dep_key_set_id + values_blob)
    TraceId traceId = getOrCreateTrace(traceHash, depKeySetId, valuesBlob);

    // 9. Flush pending entities to DB (IDs must exist before FK references).
    // flush() also flushes vocab entries via the ATTACH'd connection.
    flush();

    // 10. Upsert Attrs + insert History
    {
        auto st(_state->lock());
        st->upsertAttr.use()
            (contextHash)(static_cast<int64_t>(pathId.value))(static_cast<int64_t>(traceId.value))(static_cast<int64_t>(resultId.value)).exec();
        st->insertHistory.use()
            (contextHash)(static_cast<int64_t>(pathId.value))(static_cast<int64_t>(traceId.value))(static_cast<int64_t>(resultId.value)).exec();
    }

    // 11. Session caches
    bool hasVolatile = std::any_of(allDeps.begin(), allDeps.end(),
        [](auto & d) { return isVolatile(d.key.type); });
    if (!hasVolatile)
        verifiedTraceIds.insert(traceId);

    {
        auto & data = traceDataCache[traceId];
        data.traceHash = traceHash;
        data.structHash = structHash;
        data.deps = sorted;  // already vector<Dep> from sort/dedup
        assert(data.hashesPopulated() && "recording trace with placeholder (all-zero) hashes");
    }
    depKeySetCache.insert_or_assign(depKeySetId, keys);

    // Update traceRowCache so subsequent lookupTraceRow/getCurrentTraceHash
    // calls for this attr path don't go to DB.
    traceRowCache[pathId] = TraceRow{traceId, resultId, type, val, ctx};

    nrRecordTimeUs += elapsedUs(recordStart);
    return RecordResult{traceId};
}

} // namespace nix::eval_trace
