#include "sibling-tracker.hh"
#include "traced-expr.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"

namespace nix::eval_trace {

thread_local SiblingAccessTracker * SiblingAccessTracker::current = nullptr;

SiblingAccessTracker::SiblingAccessTracker(TracedExpr * parent, EvalTraceContext * ctx)
    : parentExpr(parent), previous(current), traceCtx(ctx)
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

void SiblingAccessTracker::maybeRecord(TracedExpr * expr, TraceStore & db)
{
    if (!current) return;
    if (expr->parentExpr != current->parentExpr) return;
    if (!expr->lazy || !expr->lazy->traceId) return;
    try {
        auto hash = db.getCurrentTraceHash(expr->pathId);
        if (hash) current->recordAccess(expr->pathId, *hash);
    } catch (...) {}
}

bool SiblingAccessTracker::staticMaybeRecord(TracedExpr * tracedExpr, TraceStore * traceStore)
{
    if (!current) return false;
    auto before = current->accesses.size();
    maybeRecord(tracedExpr, *traceStore);
    return current->accesses.size() > before;
}

} // namespace nix::eval_trace
