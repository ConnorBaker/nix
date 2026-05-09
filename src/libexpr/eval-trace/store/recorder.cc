/// recorder.cc — Constructive-trace recording pipeline
/// (rearchitecture-proposal.md §14 step 7 + §2.3). Private backend
/// helpers are reached via `friend class Recorder;` on
/// `SqliteTraceStorage`.

#include "nix/expr/eval-trace/store/recorder.hh"

#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/trace-result-codec.hh"

#include "trace-serialize.hh"
#include "nix/util/logging.hh"

namespace nix::eval_trace {

Recorder::Recorder(SqliteTraceStorage & storage, InterningPools & pools,
                   AttrVocabStore & vocab) noexcept
    : VocabAwareHasher(pools, vocab)
    , storage_(storage)
{
}

RecordResult Recorder::record(
    const ExclusiveTraceStorageAccess & ea,
    AttrPathId pathId,
    const CachedResult & value,
    const std::vector<Dep> & allDeps,
    TraceObserver * observer)
{
    auto recordStart = timerStart();
    nrRecords++;
    debug("eval-trace/store: record pathId=%u nDeps=%zu", pathId.value, allDeps.size());

    // 1. Sort deps canonically and drop only exact duplicate observations.
    auto sorted = sortAndDedupDeps(allDeps);

    // 2. Compute canonical recovery hash plus exact storage hashes.
    auto feedKeyFn = [this](CanonicalHashBuilder & builder, const Dep::Key & key) {
        feedKey(builder, key);
    };
    auto hashStart = timerStart();
    auto traceHash = computeTraceHashFromSorted(sorted, feedKeyFn);
    auto fullHash = computeFullTraceHashFromSorted(sorted, feedKeyFn);
    auto keySetHash = computeDepKeySetHashFromSorted(sorted, feedKeyFn);
    nrRecordHashUs += elapsedUs(hashStart);

    // 3. Split into keys + values.
    std::vector<Dep::Key> keys;
    keys.reserve(sorted.size());
    for (auto & d : sorted)
        keys.push_back(d.key);

    auto serializeKeysStart = timerStart();
    auto keysBlob = SqliteTraceStorage::serializeKeys(keys);
    nrRecordSerializeKeysUs += elapsedUs(serializeKeysStart);

    auto serializeValuesStart = timerStart();
    auto valuesBlob = SqliteTraceStorage::serializeValues(sorted);
    nrRecordSerializeValuesUs += elapsedUs(serializeValuesStart);

    // 4. Get or create exact dep key set.
    auto depKeySetId = storage_.getOrCreateDepKeySet(keySetHash, keysBlob);

    // 5. Encode CachedResult and intern result.
    auto payload = encodeCachedResult(value, hasherVocab());
    auto resultHash = computeResultHash(
        payload.type, payload.encodingVersion, payload.payload, payload.auxContext);
    ResultId resultId = storage_.doInternResult(payload, resultHash);

    // 6. Get or create trace (keyed by full_hash, stores canonical trace_hash).
    TraceId traceId = storage_.getOrCreateTrace(traceHash, fullHash, depKeySetId, valuesBlob);

    // 7. Flush pending entities to DB (IDs must exist before FK references).
    // flush() also flushes vocab entries via the ATTACH'd connection.
    storage_.flush(ea);

    // 8. Atomically publish: DB writes + all session cache updates.
    // `publishRecord` takes header/sorted/keys by value and moves from
    // them into its read-through caches. When there is no observer we
    // can move directly; with an observer we keep `sorted` and `keys`
    // alive so the observer can mirror the same payload into its own
    // caches after publish returns.
    TraceHeader header{
        .traceHash = traceHash,
        .keySetHash = keySetHash,
        .depKeySetId = depKeySetId,
    };
    CurrentNodeRef ref{};
    if (observer) {
        ref = storage_.publishRecord(
            ea.blockingProof(), pathId, traceId, resultId,
            header, sorted, depKeySetId, keys);
        observer->onNewTrace(traceId, header, sorted, depKeySetId, keys);
        observer->onPublishCurrent(pathId, ref);
    } else {
        ref = storage_.publishRecord(
            ea.blockingProof(), pathId, traceId, resultId,
            header, std::move(sorted), depKeySetId, std::move(keys));
    }

    nrRecordTimeUs += elapsedUs(recordStart);
    return RecordResult{traceId};
}

} // namespace nix::eval_trace
