#pragma once
/**
 * @file
 *
 * Evaluation profiler interface definitions and builtin implementations.
 */

#include "nix/util/ref.hh"

#include <span>
#include <bitset>
#include <optional>
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

private:
    std::optional<Hooks> neededHooks;

protected:
    /**
     * Get which hooks need to be called.
     *
     * This is the actual implementation which has to be defined by subclasses.
     * Public API goes through `getNeededHooks`, a non-virtual interface (NVI)
     * which caches the return value. The cache is populated lazily on first
     * call and never invalidated; in-tree subclasses (`FunctionCallTrace`,
     * `SampleStack`) return constants, so a single virtual dispatch suffices.
     */
    virtual Hooks getNeededHooksImpl() const
    {
        return Hooks{};
    }

public:
    /**
     * Hook called in the EvalState::callFunction preamble.
     * Gets called only if (getNeededHooks().test(Hook::preFunctionCall)) is true.
     *
     * @param state Evaluator state.
     * @param v Function being invoked.
     * @param args Function arguments.
     * @param pos Function position.
     */
    virtual void preFunctionCallHook(EvalState & state, const Value & v, std::span<Value *> args, const PosIdx pos);

    /**
     * Hook called on EvalState::callFunction exit.
     * Gets called only if (getNeededHooks().test(Hook::postFunctionCall)) is true.
     *
     * @param state Evaluator state.
     * @param v Function being invoked.
     * @param args Function arguments.
     * @param pos Function position.
     */
    virtual void postFunctionCallHook(EvalState & state, const Value & v, std::span<Value *> args, const PosIdx pos);

    virtual ~EvalProfiler() = default;

    /**
     * Get which hooks need to be invoked for this EvalProfiler instance.
     */
    Hooks getNeededHooks()
    {
        if (neededHooks.has_value())
            return *neededHooks;
        return *(neededHooks = getNeededHooksImpl());
    }
};

ref<EvalProfiler> makeSampleStackProfiler(EvalState & state, std::filesystem::path profileFile, uint64_t frequency);

} // namespace nix
