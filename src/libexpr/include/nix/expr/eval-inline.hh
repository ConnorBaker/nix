#pragma once
///@file

#include "nix/expr/allocator.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/ghc-gc.hh"  // Phase 4: Env preservation (Iteration 48)
#include "nix/expr/print.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval-settings.hh"

namespace nix {

/**
 * Note: Various places expect the allocated memory to be zeroed.
 * Uses the compile-time selected Allocator backend.
 */
[[gnu::always_inline]]
inline void * allocBytes(size_t n)
{
    return Allocator::allocBytes(n);
}

[[gnu::always_inline]]
Value * EvalMemory::allocValue()
{
    void * p;

    if constexpr (allocatorSupportsBatchAllocation()) {
        /* We use the batch allocator to speed up allocations of Values (of which there are many).
           GC_malloc_many returns a linked list of objects of the given size, where the first word
           of each object is also the pointer to the next object in the list. This also means that we
           have to explicitly clear the first word of every object we take. */
        thread_local static std::shared_ptr<void *> valueAllocCache{
            std::allocate_shared<void *>(traceable_allocator<void *>(), nullptr)};

        if (!*valueAllocCache) {
            *valueAllocCache = Allocator::allocMany(sizeof(Value));
        }

        /* Take the first list item, advance the list to the next item, and clear the next pointer. */
        p = *valueAllocCache;
        *valueAllocCache = Allocator::getNext(p);
        Allocator::setNext(p, nullptr);
    } else {
        // Use dedicated Value allocation for better tracking
        p = Allocator::allocValue();
    }

    stats.nrValues++;
    return (Value *) p;
}

[[gnu::always_inline]]
Env & EvalMemory::allocEnv(size_t size)
{
    stats.nrEnvs++;
    stats.nrValuesInEnvs += size;

    Env * env;

    if constexpr (allocatorSupportsBatchAllocation()) {
        if (size == 1) {
            /* see allocValue for explanations. */
            thread_local static std::shared_ptr<void *> env1AllocCache{
                std::allocate_shared<void *>(traceable_allocator<void *>(), nullptr)};

            if (!*env1AllocCache) {
                *env1AllocCache = Allocator::allocMany(sizeof(Env) + sizeof(Value *));
            }

            void * p = *env1AllocCache;
            *env1AllocCache = Allocator::getNext(p);
            Allocator::setNext(p, nullptr);
            env = (Env *) p;
        } else {
            // Use allocEnv for variable-size Envs (non-batch path)
            env = (Env *) Allocator::allocEnv(size);
        }
    } else {
        // Use dedicated Env allocation for better tracking
        env = (Env *) Allocator::allocEnv(size);
    }

    /* We assume that env->values has been cleared by the allocator; maybeThunk() and lookupVar fromWith expect this. */

    return *env;
}

/**
 * An identifier of the current thread for deadlock detection, stored
 * in p0 of pending/awaited thunks. We're not using std::thread::id
 * because it's not guaranteed to fit.
 */
extern thread_local uint32_t myEvalThreadId;

// ============================================================================
// Phase 4: Thunk Forcing Analysis (Task 4.1, Iteration 43)
// ============================================================================
// This is the core thunk forcing mechanism. Understanding this is critical
// for fixing the cached thunk problem.
//
// PROBLEM IDENTIFICATION:
// When a thunk is forced (pdThunk case below), the following happens:
// 1. Env* and Expr* are extracted from p0_ and p1_ (lines 134-136)
// 2. expr->eval() is called, which evaluates the expression
// 3. The result is written to *this, OVERWRITING p0 and p1
// 4. After step 3, the Env* is no longer referenced by this Value
// 5. If this was a cached thunk, the Env chain becomes UNREACHABLE
// 6. GC can free the Env chain, causing use-after-free for other thunks
//    that still reference the same Env
//
// CACHED THUNK SCENARIO:
// - File A is imported multiple times via genList
// - First import: thunk is forced, result cached in fileEvalCache
// - Forcing overwrites the thunk's Env pointer with the result
// - Second import: returns cached value (no longer has Env reference)
// - GC runs: Env chain is unreachable from cached value, gets freed
// - Other unevaluated thunks from same file still reference the freed Env
// - Next force: use-after-free crash
//
// SOLUTION NEEDED:
// Preserve Env chain even after thunk is forced, OR prevent GC from
// freeing Envs that may be referenced by other thunks.
// ============================================================================

template<std::size_t ptrSize>
void ValueStorage<ptrSize, std::enable_if_t<detail::useBitPackedValueStorage<ptrSize>>>::force(
    EvalState & state, PosIdx pos)
{
    auto p0_ = p0.load(std::memory_order_acquire);

    auto pd = static_cast<PrimaryDiscriminator>(p0_ & discriminatorMask);

    if (pd == pdThunk) {
        try {
            // The value we get here is only valid if we can set the
            // thunk to pending.
            auto p1_ = p1;

            // Atomically set the thunk to "pending".
            if (!p0.compare_exchange_strong(
                    p0_,
                    pdPending | (myEvalThreadId << discriminatorBits),
                    std::memory_order_acquire,
                    std::memory_order_acquire)) {
                pd = static_cast<PrimaryDiscriminator>(p0_ & discriminatorMask);
                if (pd == pdPending || pd == pdAwaited) {
                    // The thunk is already "pending" or "awaited", so
                    // we need to wait for it.
                    p0_ = waitOnThunk(state, p0_);
                    goto done;
                }
                assert(pd != pdThunk);
                // Another thread finished this thunk, no need to wait.
                goto done;
            }

            bool isApp = p1_ & discriminatorMask;
            if (isApp) {
                auto left = untagPointer<Value *>(p0_);
                auto right = untagPointer<Value *>(p1_);
                state.callFunction(*left, *right, (Value &) *this, pos);

                // Iteration 58: Write barrier for function application
                // callFunction stores the result in *this
                ghc::gcRecordMutation((Value *) this);
            } else {
                // CRITICAL SECTION (Phase 4 Analysis):
                // Extract Env and Expr from thunk's p0/p1
                auto env = untagPointer<Env *>(p0_);
                auto expr = untagPointer<Expr *>(p1_);
                // Evaluate expression in the Env, writing result to *this
                // ISSUE: After eval(), *this no longer references env
                // The Env chain becomes unreachable if this is a cached thunk
                expr->eval(state, *env, (Value &) *this);

                // Phase 4: Preserve Env after forcing (Iteration 48)
                // Keep the Env alive as a GC root to prevent use-after-free
                // when this thunk is cached and other thunks still need the Env
                ghc::gcPreserveEnv((Value *) this, env);

                // Iteration 58: Write barrier for thunk forcing
                // After forcing, this Value might now contain young references
                // (e.g., freshly allocated list elements, bindings, etc.)
                ghc::gcRecordMutation((Value *) this);
            }
        } catch (...) {
            state.tryFixupBlackHolePos((Value &) *this, pos);
            setStorage(new Value::Failed{.ex = std::current_exception()});
            throw;
        }
    }

    else if (pd == pdPending || pd == pdAwaited)
        p0_ = waitOnThunk(state, p0_);

done:
    if (InternalType(p0_ & 0xff) == tFailed)
        std::rethrow_exception((std::bit_cast<Failed *>(p1))->ex);
}

[[gnu::always_inline]]
inline void EvalState::forceAttrs(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    forceAttrs(v, [&]() { return pos; }, errorCtx);
}

template<typename Callable>
[[gnu::always_inline]]
inline void EvalState::forceAttrs(Value & v, Callable getPos, std::string_view errorCtx)
{
    PosIdx pos = getPos();
    forceValue(v, pos);
    if (v.type() != nAttrs) {
        error<TypeError>("expected a set but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
            .withTrace(pos, errorCtx)
            .debugThrow();
    }
}

[[gnu::always_inline]]
inline void EvalState::forceList(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    forceValue(v, pos);
    if (!v.isList()) {
        error<TypeError>("expected a list but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
            .withTrace(pos, errorCtx)
            .debugThrow();
    }
}

[[gnu::always_inline]]
inline CallDepth EvalState::addCallDepth(const PosIdx pos)
{
    if (callDepth > settings.maxCallDepth)
        error<EvalBaseError>("stack overflow; max-call-depth exceeded").atPos(pos).debugThrow();

    return CallDepth(callDepth);
};

} // namespace nix
