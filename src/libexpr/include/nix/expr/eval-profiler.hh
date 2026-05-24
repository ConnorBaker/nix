#pragma once
/**
 * @file
 *
 * Evaluation profiler interface definitions and builtin implementations.
 */

#include "nix/util/ref.hh"

#include <span>
#include <bitset>
#include <filesystem>

namespace nix {

class EvalState;
class PosIdx;
struct Value;

class EvalProfiler
{
public:
    enum Hook {
        preFunctionCall,
        postFunctionCall,
    };

    static constexpr std::size_t numHooks = Hook::postFunctionCall + 1;
    using Hooks = std::bitset<numHooks>;

    /**
     * Get which hooks need to be invoked for this EvalProfiler instance.
     * Subclasses return a constant; the result is captured into
     * `EvalState`'s `profilerHooks` snapshot at construction time and
     * never re-queried, so per-instance caching here is unnecessary.
     */
    virtual Hooks getNeededHooks() const = 0;

    /**
     * Hook called in the EvalState::callFunction preamble.
     * Gets called only if `getNeededHooks().test(Hook::preFunctionCall)`
     * was true at `EvalState` construction.
     *
     * @param state Evaluator state.
     * @param v Function being invoked.
     * @param args Function arguments.
     * @param pos Function position.
     */
    virtual void preFunctionCallHook(EvalState & state, const Value & v, std::span<Value *> args, const PosIdx pos) = 0;

    /**
     * Hook called on EvalState::callFunction exit.
     * Gets called only if `getNeededHooks().test(Hook::postFunctionCall)`
     * was true at `EvalState` construction.
     *
     * @param state Evaluator state.
     * @param v Function being invoked.
     * @param args Function arguments.
     * @param pos Function position.
     */
    virtual void postFunctionCallHook(EvalState & state, const Value & v, std::span<Value *> args, const PosIdx pos) = 0;

    virtual ~EvalProfiler() = default;
};

ref<EvalProfiler> makeSampleStackProfiler(EvalState & state, std::filesystem::path profileFile, uint64_t frequency);

} // namespace nix
