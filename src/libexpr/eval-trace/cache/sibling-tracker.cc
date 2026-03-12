#include "sibling-tracker.hh"
#include "traced-expr.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"

namespace nix::eval_trace {

thread_local SiblingAccessTracker * SiblingAccessTracker::current = nullptr;

SiblingAccessTracker::SiblingAccessTracker(TracedExpr * parent, AttrPathId selfPathId, EvalTraceContext * ctx)
    : parentExpr(parent), selfPathId(selfPathId), previous(current), traceCtx(ctx)
{
    current = this;
    if (traceCtx) {
        savedCallback = traceCtx->siblingCallback;
        traceCtx->siblingCallback = &staticMaybeRecord;
    }
}

SiblingAccessTracker::~SiblingAccessTracker()
{
    current = previous;
    if (traceCtx)
        traceCtx->siblingCallback = savedCallback;
}

void SiblingAccessTracker::recordAccess(AttrPathId pathId, const TraceHash & traceHash)
{
    if (seen.insert(pathId).second)
        accesses.emplace_back(pathId, traceHash);
}

void SiblingAccessTracker::maybeRecord(const EvalTraceContext::SiblingIdentity & si)
{
    if (!current) return;
    if (si.parentExpr != current->parentExpr) return;
    if (si.pathId == current->selfPathId) return;  // skip self
    try {
        auto hash = si.traceStore->getCurrentTraceHash(si.pathId);
        if (hash)
            current->recordAccess(si.pathId, *hash);
        else {
            current->hasUntracedAccess = true;
            current->untracedSiblings.emplace_back(si.pathId, si.traceStore);
        }
    } catch (...) {}
}

bool SiblingAccessTracker::staticMaybeRecord(const EvalTraceContext::SiblingIdentity & si)
{
    if (!current) return false;
    if (si.parentExpr != current->parentExpr) return false;
    if (si.pathId == current->selfPathId) return false;
    // Return true for already-seen siblings to prevent replayMemoizedDeps
    // from falling through to normal epoch replay (which would import the
    // sibling's direct deps into the current child, defeating per-sibling
    // ParentContext precision).
    if (current->seen.count(si.pathId)) return true;
    auto before = current->accesses.size();
    maybeRecord(si);
    return current->accesses.size() > before;
}

} // namespace nix::eval_trace
