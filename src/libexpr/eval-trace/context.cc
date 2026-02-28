#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/cache/trace-cache.hh"
#include "nix/expr/value.hh"

namespace nix {

EvalTraceContext::EvalTraceContext()
    : pools(createInterningPools().release(), destroyInterningPools)
{
}

EvalTraceContext::~EvalTraceContext() = default;

namespace eval_trace {
Counter nrReplayTotalCalls;
Counter nrReplayBloomHits;
Counter nrReplayEpochHits;
Counter nrReplayAdded;
} // namespace eval_trace

void EvalTraceContext::recordThunkDeps(Value & v, uint32_t epochStart)
{
    if (&v == skipEpochRecordFor) {
        skipEpochRecordFor = nullptr;
        return;
    }
    uint32_t epochEnd = DependencyTracker::sessionTraces.size();
    if (epochStart < epochEnd) {
        epochMap.emplace(&v, DepRange{&DependencyTracker::sessionTraces, epochStart, epochEnd});
        bloomSet(&v);
    }
}

void EvalTraceContext::replayMemoizedDeps(const Value & v)
{
    eval_trace::nrReplayTotalCalls++;
    if (!bloomTest(&v)) return;
    eval_trace::nrReplayBloomHits++;
    auto it = epochMap.find(&v);
    if (it == epochMap.end()) return;
    eval_trace::nrReplayEpochHits++;

    auto & range = it->second;

    // Only the innermost (active) tracker receives replayed deps.
    // The previous implementation walked the full tracker stack, but profiling
    // across multiple large workloads showed nrReplayStackWalkMultiModify = 0
    // (the counter tracked cases where more than one tracker on the stack was
    // modified). This holds because nested trackers (e.g., child TracedExpr
    // evaluations) exclude their dep ranges from the parent via
    // excludeChildRange(), so replayed deps only need to reach the innermost.
    auto * tracker = DependencyTracker::activeTracker;
    if (!tracker) return;

    // If the range is within this tracker's session range, skip —
    // the deps are already captured by [startIndex, sessionTraces.size()).
    if (range.deps == tracker->mySessionTraces
        && range.start >= tracker->startIndex)
        return;
    if (tracker->replayedValues.insert(&v).second) {
        tracker->replayedRanges.push_back(range);
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
    evalCaches.clear();
}

} // namespace nix
