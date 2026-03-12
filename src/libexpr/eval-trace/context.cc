#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/value.hh"
#include "nix/expr/attr-set.hh"
#include "nix/util/logging.hh"
#include "cache/traced-expr.hh"
#include "cache/sibling-tracker.hh"

namespace nix {

void EvalTraceContext::recordThunkDeps(const Value & v, uint32_t epochStart)
{
    uint32_t epochEnd = DependencyTracker::epochLog.size();
    if (epochStart < epochEnd) {
        epochMap.emplace(&v, DepRange{&DependencyTracker::epochLog, epochStart, epochEnd});
        replayBloom.set(&v);
    }
}

// ── replayMemoizedDeps and the first-touch asymmetry ─────────────────
//
// The sibling callback (lines below, siblingCallback check) only fires
// for values that are ALREADY forced — i.e., values that have an epochMap
// entry from a previous forceThunkValue call.  When siblingCallback
// succeeds, the dep copy is skipped and the child gets a compact
// ParentContext dep instead of the sibling's full dep set.
//
// First-touch thunks never reach this path.  When forceThunkValue forces
// a sibling thunk for the first time, the sibling's deps flow through
// record() directly into the child's active DependencyTracker::ownDeps.
// The epochMap entry is only created AFTER forceThunkValue completes
// (via recordThunkDeps), so replayMemoizedDeps cannot intercept the
// first-touch evaluation.  This is the root of the first-touch asymmetry:
// evaluation order determines whether a sibling access produces a compact
// ParentContext dep or a full copy of the sibling's deps.
//
// See forceThunkValue for why this is accepted as sound-but-imprecise.
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
            && siblingCallback(sibIt->second))
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

bool EvalTraceContext::haveSameResolvedTarget(Value & v1, Value & v2)
{
    // Find SiblingIdentity for each value. Try Value* lookup first (works for
    // values accessed through Bindings pointers). Fall back to Bindings* lookup
    // for attrset Values that were copied by ExprSelect::eval's `v = *vAttrs`.
    // The copy preserves the Bindings* pointer, so looking up by v.attrs()
    // finds the TracedExpr that produced the original.
    auto findIdentity = [&](Value & v) -> SiblingIdentity * {
        // For attrsets, check bindingsIdentityMap first — its entries are
        // populated in registerBindings after evaluation and carry originalBindings
        // (needed for Tier 2). siblingIdentityMap entries are registered at
        // installChildThunk time before evaluation, so they have originalBindings=null.
        // Both maps store the same parentExpr and canonicalSiblingIdx, so Tier 1
        // still works correctly when we prefer bindingsIdentityMap.
        if (v.type() == nAttrs) {
            auto bit = bindingsIdentityMap.find(v.attrs());
            if (bit != bindingsIdentityMap.end()) return &bit->second;
        }
        auto it = siblingIdentityMap.find(&v);
        if (it != siblingIdentityMap.end()) return &it->second;
        return nullptr;
    };

    auto * si1 = findIdentity(v1);
    if (!si1)
        return false;
    auto * si2 = findIdentity(v2);
    if (!si2)
        return false;

    // The identity maps use default allocator (mimalloc, GC-invisible), so
    // stored GC-allocated pointers (TracedExpr*, Bindings*) are not traced.
    // Tier 1 and Tier 2 only COMPARE these pointers (no deref), so they
    // won't crash, but pointer reuse after GC collection could cause false
    // positives. Tier 3 DEREFERENCES tracedExpr and is not GC-safe — it
    // is best-effort with GC safety deferred to Stage 2.

    // Tier 1: same parent + same alias index (O(1), set by detectAliases
    // during materialization). Handles direct aliases like
    // { a = platform; b = platform; }.
    if (si1->parentExpr && si1->parentExpr == si2->parentExpr
        && si1->canonicalSiblingIdx >= 0
        && si1->canonicalSiblingIdx == si2->canonicalSiblingIdx)
        return true;

    // Tier 2: same original Bindings* (pre-materialization). Handles aliases
    // where different thunks evaluate to the same attrset, like
    // { buildPlatform = localSystem; hostPlatform = if ... then ... else localSystem; }.
    // detectAliases misses these because thunks have different Value* before
    // evaluation. The originalBindings is cached from resolvedTarget->attrs()
    // at TracedExpr::eval() completion.
    if (si1->originalBindings && si1->originalBindings == si2->originalBindings)
        return true;

    // Tier 3: Navigate via getResolvedTarget (lazy — only when eqValues calls us).
    // Handles cross-parent aliases for NavigatedChild attrsets on the hot path,
    // where evaluatePhase2 is skipped and originalBindings was not set.
    // Best-effort: TracedExpr* deref is NOT GC-safe (identity maps use default
    // allocator, so GC may collect TracedExpr after thunk forcing). Gated on
    // nAttrs and non-null tracedExpr. GC safety deferred to Stage 2.
    // getResolvedTarget() caches the result in lazy->resolvedTarget for future calls.
    if (v1.type() == nAttrs && v2.type() == nAttrs
        && si1->tracedExpr && si2->tracedExpr)
    {
        SuspendDepTracking suspend;
        auto * t1 = si1->tracedExpr->getResolvedTarget();
        auto * t2 = si2->tracedExpr->getResolvedTarget();
        if (t1 && t2) {
            if (t1 == t2)
                return true;
            if (t1->type() == nAttrs && t2->type() == nAttrs
                && t1->attrs() == t2->attrs())
                return true;
        }
    }

    return false;
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
