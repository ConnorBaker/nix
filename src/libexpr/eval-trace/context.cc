#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/value.hh"
#include "nix/expr/attr-set.hh"
#include "nix/util/logging.hh"
#include "cache/traced-expr.hh"

namespace nix {

void EvalTraceContext::recordThunkDeps(const Value & v, uint32_t epochStart)
{
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

    // Sibling detection: if this value is a registered sibling TracedExpr
    // and a SiblingAccessTracker is active, record the sibling access and
    // skip dep copy (the child gets a ParentContext dep instead).
    // Only skip if the callback succeeds — on failure (parent mismatch,
    // no traceId), fall through to normal dep replay.
    if (siblingCallback) {
        auto sibIt = siblingIdentityMap.find(&v);
        if (sibIt != siblingIdentityMap.end()
            && siblingCallback(sibIt->second.tracedExpr, sibIt->second.traceStore))
            return;
    }

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

/**
 * Check whether two resolved Values share the same underlying data.
 *
 * getResolvedTarget() may return different Value* addresses for aliased
 * values (e.g., when ExprOrigChild::eval copies `v = *attr->value`).
 * The copies share the same Bindings* (for attrsets) or the same element
 * pointers (for lists), so compare those rather than Value* addresses.
 */
static bool resolvedTargetsMatch(Value * t1, Value * t2)
{
    if (t1 == t2) return true;
    if (!t1 || !t2) return false;
    if (t1->type() != t2->type()) return false;

    if (t1->type() == nAttrs)
        return t1->attrs() == t2->attrs();
    if (t1->type() == nList) {
        if (t1->listSize() != t2->listSize()) return false;
        if (t1->listSize() == 0) return true;
        auto v1 = t1->listView();
        auto v2 = t2->listView();
        for (size_t i = 0; i < v1.size(); i++)
            if (v1[i] != v2[i]) return false;
        return true;
    }
    return false;
}

bool EvalTraceContext::haveSameResolvedTarget(Value & v1, Value & v2)
{
    // Find SiblingIdentity for each value. Try Value* lookup first (works for
    // values accessed through Bindings pointers). Fall back to Bindings* lookup
    // for attrset Values that were copied by ExprSelect::eval's `v = *vAttrs`.
    // The copy preserves the Bindings* pointer, so looking up by v.attrs()
    // finds the TracedExpr that produced the original.
    auto findIdentity = [&](Value & v) -> SiblingIdentity * {
        auto it = siblingIdentityMap.find(&v);
        if (it != siblingIdentityMap.end()) return &it->second;
        if (v.type() == nAttrs) {
            auto bit = bindingsIdentityMap.find(v.attrs());
            if (bit != bindingsIdentityMap.end()) return &bit->second;
        }
        return nullptr;
    };

    auto * si1 = findIdentity(v1);
    if (!si1) return false;
    auto * si2 = findIdentity(v2);
    if (!si2) return false;

    auto * te1 = si1->tracedExpr;
    auto * te2 = si2->tracedExpr;

    // Fast path 1: same canonical sibling (zero navigation, always correct)
    if (te1->parentExpr && te1->parentExpr == te2->parentExpr
        && te1->canonicalSiblingIdx >= 0
        && te1->canonicalSiblingIdx == te2->canonicalSiblingIdx)
        return true;

    // Fast path 2: cached resolvedTarget pointer match
    // ONLY returns true on match — mismatch falls through to data comparison
    Value * t1 = (te1->lazy ? te1->lazy->resolvedTarget : nullptr);
    Value * t2 = (te2->lazy ? te2->lazy->resolvedTarget : nullptr);
    if (t1 && t2 && t1 == t2)
        return true;

    // Resolve targets if not cached
    if (!t1) t1 = te1->getResolvedTarget();
    if (!t2) t2 = te2->getResolvedTarget();

    // Data-level comparison (Bindings* for attrsets, element ptrs for lists)
    return resolvedTargetsMatch(t1, t2);
}

void EvalTraceContext::reset()
{
    evalCaches.clear();
    fileContentHashes.clear();
    mountToInput.clear();
    epochMap.clear();
    replayBloom.reset();
    siblingIdentityMap.clear();
    bindingsIdentityMap.clear();
    siblingCallback = nullptr;
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
