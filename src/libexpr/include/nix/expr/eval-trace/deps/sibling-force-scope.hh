#pragma once
///@file
/// RAII isolation for first-touch sibling thunk forcing.

#include "nix/expr/eval-trace/deps/dep-recording-context.hh"

#include <cassert>

namespace nix {

class EvalState;

namespace eval_trace {
DepRecordingContext * currentFiberDepCtx();
DepRecordingContext * currentStandaloneDepCtx();
}

/// RAII isolation for first-touch sibling thunk forcing.
///
/// Encapsulates:
///   1. Pushing an isolated scope on the DepRecordingContext
///   2. Publishing the epoch range to the replay store (recordThunkDeps)
///   3. Replaying through the SiblingReplayCaptureScope (replayMemoizedDeps)
///
/// commit() MUST be called on success. If not called (exception), the
/// destructor pops the scope without publishing.
struct SiblingForceScope
    : private gdp::Certifier<DepRecordingContext::DepCaptureScopeTag>
{
private:
    EvalState & state;
    const Value & value;
    uint32_t epochStart;
    DepRecordingContext * fiberCtx = nullptr;
    bool committed = false;

public:
    explicit SiblingForceScope(
        EvalState & state,
        const Value & value,
        uint32_t epochStart);

    void commit();

    ~SiblingForceScope()
    {
        if (!committed && fiberCtx)
            Certifier::withProof([&](const auto & proof) { fiberCtx->popScope(proof); });
    }

    SiblingForceScope(const SiblingForceScope &) = delete;
    SiblingForceScope & operator=(const SiblingForceScope &) = delete;
};

} // namespace nix
