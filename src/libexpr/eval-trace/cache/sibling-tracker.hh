#pragma once
/// @file
/// SiblingAccessTracker — per-accessed-sibling ParentContext dep recording.
/// Private header for eval-trace cache internals.

#include "nix/expr/eval-trace/ids.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/context.hh"

#include <boost/unordered/unordered_flat_set.hpp>
#include <utility>
#include <vector>

namespace nix::eval_trace {

struct TracedExpr;
struct TraceStore;

/**
 * RAII tracker for recording which siblings a navigated child accesses
 * during evaluation. Thread-local linked list enables nesting (inner
 * child evaluations isolate from outer).
 *
 * Used to create per-sibling ParentContext deps instead of a single
 * whole-parent dep, avoiding cascading invalidation when unrelated
 * siblings are added/removed. Each navigated child's evaluateFresh()
 * creates a tracker scoped to its parentExpr; sibling TracedExpr::eval()
 * calls maybeRecord() at completion to register themselves.
 */
struct SiblingAccessTracker
{
    TracedExpr * parentExpr;
    std::vector<std::pair<AttrPathId, TraceHash>> accesses;
    boost::unordered_flat_set<AttrPathId, AttrPathId::Hash> seen;

    static thread_local SiblingAccessTracker * current;
    SiblingAccessTracker * previous;
    EvalTraceContext::SiblingCallback savedCallback = nullptr;
    EvalTraceContext * traceCtx = nullptr;

    explicit SiblingAccessTracker(TracedExpr * parent, EvalTraceContext * ctx);
    ~SiblingAccessTracker();

    SiblingAccessTracker(const SiblingAccessTracker &) = delete;
    SiblingAccessTracker & operator=(const SiblingAccessTracker &) = delete;

    void recordAccess(AttrPathId pathId, const TraceHash & traceHash);

    static void maybeRecord(TracedExpr * expr, TraceStore & db);

    /// Static callback for replayMemoizedDeps sibling detection.
    static bool staticMaybeRecord(TracedExpr * tracedExpr, TraceStore * traceStore);
};

} // namespace nix::eval_trace
