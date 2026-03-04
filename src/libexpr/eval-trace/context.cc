#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/cache/trace-cache.hh"
#include "nix/expr/value.hh"

namespace nix {

namespace eval_trace {
Counter nrReplayTotalCalls;
Counter nrReplayBloomHits;
Counter nrReplayEpochHits;
Counter nrReplayAdded;
} // namespace eval_trace

void EvalTraceContext::recordThunkDeps(const Value & v, uint32_t epochStart)
{
    if (&v == skipEpochRecordFor) {
        skipEpochRecordFor = nullptr;
        return;
    }
    uint32_t epochEnd = DependencyTracker::epochLog.size();
    if (epochStart < epochEnd) {
        epochMap.emplace(&v, DepRange{&DependencyTracker::epochLog, epochStart, epochEnd});
        replayBloom.set(&v);
    }
}

void EvalTraceContext::replayMemoizedDeps(const Value & v)
{
    eval_trace::nrReplayTotalCalls++;
    if (!replayBloom.test(&v)) return;
    eval_trace::nrReplayBloomHits++;
    auto it = epochMap.find(&v);
    if (it == epochMap.end()) return;
    eval_trace::nrReplayEpochHits++;

    auto & range = it->second;

    // Only the innermost (active) tracker receives replayed deps.
    auto * tracker = DependencyTracker::activeTracker;
    if (!tracker) return;

    // If the epoch range started after this tracker was created, the deps
    // are already in ownDeps (they were recorded via record() during this
    // tracker's lifetime). Skip to avoid double-counting.
    if (range.start >= tracker->epochLogStartIndex)
        return;
    if (tracker->replayedValues.insert(&v).second) {
        // Copy deps from epochLog range into this tracker's ownDeps.
        tracker->ownDeps.insert(tracker->ownDeps.end(),
            range.deps->begin() + range.start,
            range.deps->begin() + range.end);
        eval_trace::nrReplayAdded++;
    }
}

void EvalTraceContext::reset()
{
    evalCaches.clear();
    fileContentHashes.clear();
    mountToInput.clear();
    epochMap.clear();
    replayBloom.reset();
    skipEpochRecordFor = nullptr;
}

void EvalTraceContext::flush()
{
    // Destroy trace stores — TraceStore destructors flush all pending data
    // (including vocab entries via ATTACH'd connection) and commit the
    // single cross-DB transaction atomically. No separate vocab flush
    // or commit needed.
    evalCaches.clear();
}

} // namespace nix
