#pragma once
///@file

#include "nix/store/store-api.hh"

namespace nix {

class FinishedEvaluation;

enum struct UseLookupPath { Use, DontUse };

/**
 * Replace the process image with `program`, in a chroot when the store is
 * diverted.  After a successful exec no destructor runs, so the caller must
 * have finished its evaluator -- caches released, pending store objects
 * written -- and the token, which only `EvalState::finish()` constructs, is
 * what makes the call compile only then.  Pass `finish()` in this call.
 */
void execProgramInStore(
    ref<Store> store,
    UseLookupPath useLookupPath,
    const std::string & program,
    const Strings & args,
    FinishedEvaluation finished,
    std::optional<std::string_view> system = std::nullopt,
    std::optional<StringMap> env = std::nullopt);

/**
 * `execvp` for a command that evaluated and prepared the environment itself
 * (`nix-shell`), with the same token.  Returns only on failure, as `execvp`
 * does.
 */
int execvpAfterEvaluation(const char * program, char * const * argv, FinishedEvaluation finished);

} // namespace nix
