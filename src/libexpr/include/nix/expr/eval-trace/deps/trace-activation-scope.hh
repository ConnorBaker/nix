#pragma once
///@file
/// TraceActivationScope: shared traced-evaluation activation guard.

#include "nix/expr/eval.hh"

namespace nix::eval_trace {

struct TraceActivationScope
{
    EvalState & state;

    explicit TraceActivationScope(EvalState & state)
        : state(state)
    {
        state.traceActiveDepth++;
    }

    ~TraceActivationScope()
    {
        state.traceActiveDepth--;
    }

    TraceActivationScope(const TraceActivationScope &) = delete;
    TraceActivationScope & operator=(const TraceActivationScope &) = delete;
};

} // namespace nix::eval_trace
