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
    TraceObserver * observer,
    bool deferFlush)
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
    //
    // `deferFlush` (async-producer-recording-plan Layer 1): skip this per-record
    // flush. Entities remain buffered in `pending*` and are drained by the next
    // non-deferred flush or the destructor's `flushExclusive()`. The step-8
    // `publishRecord` below still updates in-memory caches synchronously
    // (within-session verify + getCurrentTraceHash read those, not SQLite), and
    // its `publishStateChange` Sessions/History write has NO foreign key to Traces
    // (schema: Sessions.trace_id is a bare INTEGER, no REFERENCES; only
    // Traces.dep_key_set_id → DepKeySets is FK-constrained, and those flush together
    // in dependency order). So a deferred entity flush cannot FK-violate the Sessions
    // write — and on a CLEAN exit the teardown flush drains pendingTraces, so the DB
    // is byte-identical to the per-record-flush path.
    //
    // CRASH-CONSISTENCY CAVEAT (redesign-plan follow-up #24 — supersedes the earlier
    // "a lost producer is a future cache miss, never a wrong answer" comment, which was
    // an OVERCLAIM). Deferring flush() ALONE (this branch) leaves an ordering inversion:
    // step 6 (`getOrCreateTrace`) buffers trace-id N into pendingTraces, this flush is
    // skipped, then step 8's `publishStateChange` COMMITS a durable Sessions/History(N)
    // row in its own per-producer txn (sqlite-trace-storage.cc:704-724) while Traces(N)
    // is still unflushed. `nextTraceId` is reloaded as MAX(Traces.id) at open
    // (sqlite-trace-storage-lifecycle.cc:611), so a crash between that commit and the
    // teardown flush makes the next process REUSE id N for a different trace → the stale
    // durable History(N) aliases it. Baseline (per-record flush) is safe (Traces durable
    // BEFORE History). LATENT today: §3b is default-off, and the §3b CONSERVATIVE shape
    // keeps the consumer's flattened deps, which independently catch a mis-resolved
    // producer edge. But the AGGRESSIVE shape removes that backstop by design → the
    // aliasing becomes a candidate STALE SERVE. THEREFORE: do NOT enable the aggressive
    // shape on Layer 1 alone. The fix is Layer 2a (defer publishStateChange too, draining
    // Sessions/History in the teardown txn AFTER the Traces rows) which restores the
    // baseline ordering — see plans/async-producer-recording-plan.md §6.
    if (!deferFlush)
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
    // Layer 2a (redesign-plan #24 fix): `deferFlush` now ALSO defers the
    // Sessions/History write (publishStateChange) via `deferPublish`, so the
    // deferred path writes Traces and Sessions/History together in the batched
    // flush — restoring Traces-before-Sessions ordering. Layer-1-alone (defer
    // flush, sync publish) was the #24-hazardous state; this couples them.
    CurrentNodeRef ref{};
    if (observer) {
        ref = storage_.publishRecord(
            ea.blockingProof(), pathId, traceId, resultId,
            header, sorted, depKeySetId, keys, /*deferPublish=*/deferFlush);
        observer->onNewTrace(traceId, header, sorted, depKeySetId, keys);
        observer->onPublishCurrent(pathId, ref);
    } else {
        ref = storage_.publishRecord(
            ea.blockingProof(), pathId, traceId, resultId,
            header, std::move(sorted), depKeySetId, std::move(keys), /*deferPublish=*/deferFlush);
    }

    nrRecordTimeUs += elapsedUs(recordStart);
    return RecordResult{traceId};
}

} // namespace nix::eval_trace
