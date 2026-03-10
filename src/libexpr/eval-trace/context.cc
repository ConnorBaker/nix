#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/value.hh"
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

    // Tier 1: Parent-level alias detection (zero navigation, zero rootLoader).
    // Two children of the same parent with the same canonical sibling index
    // were aliased in the parent's real Bindings during cold evaluation.
    if (te1->parentExpr && te1->parentExpr == te2->parentExpr
        && te1->canonicalSiblingIdx >= 0
        && te1->canonicalSiblingIdx == te2->canonicalSiblingIdx)
        return true;

    // Tier 2: Already-cached resolvedTarget from cold evaluatePhase2
    // (set eagerly, no navigation triggered here).
    // resolvedTarget is set from navigateToReal(), which returns the same
    // Value* for aliased values, so pointer comparison suffices.
    if (te1->lazy && te1->lazy->resolvedTarget
        && te2->lazy && te2->lazy->resolvedTarget)
        return te1->lazy->resolvedTarget == te2->lazy->resolvedTarget;

    // Tier 3: Navigate to resolve (may trigger rootLoader).
    // This fallback ensures correctness for cross-parent aliases on hot paths
    // that aren't covered by tiers 1-2. Log so we can extend the fast paths.
    // Uses resolvedTargetsMatch instead of pointer comparison because
    // getResolvedTarget may return different Value* for the same logical value
    // (ExprOrigChild::eval copies `v = *attr->value`, preserving Bindings*
    // but not Value* identity).
    warn("haveSameResolvedTarget: falling back to navigation for '%s' vs '%s'",
         te1->attrPathStr(), te2->attrPathStr());
    return resolvedTargetsMatch(te1->getResolvedTarget(), te2->getResolvedTarget());
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
