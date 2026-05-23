#pragma once
///@file

#include "nix/expr/eval.hh"

namespace nix {

/**
 * Release our references to `EvalState::evalCaches` so the caches are
 * persisted to disk before we `exec` out of this process. C++
 * destructors do not run after `exec`, so any cached value the eval
 * caches hold a reference to must be flushed by hand at this point.
 *
 * The four `Cmd*::run` overloads that exec a tool out of the store
 * (`run`, `develop`, the `env shell` family, and the `formatter run`
 * tooling) all need to make this call right before `execProgramInStore`
 * (or equivalent). Use this helper instead of inlining
 * `state.evalCaches.clear()` so the rationale only has to live in one
 * place.
 */
inline void releaseEvalCachesBeforeExec(EvalState & state)
{
    state.evalCaches.clear();
}

} // namespace nix
