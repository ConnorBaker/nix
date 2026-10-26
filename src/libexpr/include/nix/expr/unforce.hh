#pragma once
///@file

/* Unforcing (unforce.cc): the cold parts. The hot ones (`EvalRegion`,
   `logPreImage`) are in eval.hh. */

#include "nix/expr/eval.hh"

namespace nix {

[[gnu::noinline]] void unforcePauseBegin();
[[gnu::noinline]] void unforcePauseEnd();

/**
 * While this lives, evaluator memory is allocated outside the open job's
 * region: for what outlives the job outside any thunk it forces (a cache).
 */
struct PauseRegion
{
    [[gnu::always_inline]] PauseRegion()
    {
        unforcePauseBegin();
    }

    [[gnu::always_inline]] ~PauseRegion()
    {
        unforcePauseEnd();
    }

    PauseRegion(const PauseRegion &) = delete;
    PauseRegion & operator=(const PauseRegion &) = delete;
};

/**
 * An effect that running again could give another result (a download, a
 * copy that nothing remembers, a build) or show again (a trace, a warning)
 * ran: the open job, if any, keeps its result.
 */
void unforceNoteEffect();

/** Whether a thunk of `expr` is a cell that unforcing reverted. */
bool unforceIsReverted(const Expr * expr);

/** `Value::determinePos` of a thunk of `expr`: a reverted cell's value's position, or `pos`. */
[[gnu::noinline, gnu::cold]] PosIdx unforceRevertedPos(const Expr * expr, PosIdx pos);

/** Whether unforcing has reverted a cell in this process. */
extern bool unforceAnyReverted;

} // namespace nix
