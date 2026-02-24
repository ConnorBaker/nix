#include "nix/expr/eval-trace-context.hh"
#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/value.hh"

namespace nix {

void EvalTraceContext::recordThunkDeps(Value & v, uint32_t epochStart)
{
    if (&v == skipEpochRecordFor) {
        skipEpochRecordFor = nullptr;
        return;
    }
    uint32_t epochEnd = DependencyTracker::sessionTraces.size();
    if (epochStart < epochEnd)
        epochMap.emplace(&v, DepRange{&DependencyTracker::sessionTraces, epochStart, epochEnd});
}

void EvalTraceContext::replayMemoizedDeps(const Value & v)
{
    auto it = epochMap.find(&v);
    if (it == epochMap.end()) return;

    auto & range = it->second;

    // Add the epoch range to each active tracker that doesn't already
    // include these deps in its own session range. Deduped by Value pointer.
    for (auto * tracker = DependencyTracker::activeTracker; tracker; tracker = tracker->previous) {
        // If the range is within this tracker's session range, skip —
        // the deps are already captured by [startIndex, sessionTraces.size()).
        if (range.deps == tracker->mySessionTraces
            && range.start >= tracker->startIndex)
            continue;
        if (tracker->replayedValues.insert(&v).second)
            tracker->replayedRanges.push_back(range);
    }
}

void EvalTraceContext::reset()
{
    evalCaches.clear();
    fileContentHashes.clear();
    mountToInput.clear();
    epochMap.clear();
    skipEpochRecordFor = nullptr;
}

void EvalTraceContext::flush()
{
    evalCaches.clear();
}

} // namespace nix
