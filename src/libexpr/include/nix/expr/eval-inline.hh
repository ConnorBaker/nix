#pragma once
///@file

#include "nix/expr/print.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval-settings.hh"
#include <exception>

namespace nix {

/**
 * Note: Various places expect the allocated memory to be zeroed.
 */
[[gnu::always_inline]]
inline void * EvalMemory::allocBytes(size_t n)
{
#if !NIX_USE_BOEHMGC
    /* Unforcing (the only user of evalRegion) is never active on a Boehm
       build, so the region check is compiled out entirely there. */
    if (evalRegion.active()) [[unlikely]]
        return evalRegion.alloc(n);
#endif
    void * p;
#if NIX_USE_BOEHMGC
    p = GC_MALLOC(n);
#else
    p = evalUseBaseArena ? evalBaseArena.alloc(n) : evalBaseAlloc(n);
#endif
    if (!p)
        throw std::bad_alloc();
    return p;
}

template<typename T, std::size_t N>
[[gnu::always_inline]]
inline void * EvalMemory::allocBytes(size_t count)
{
    size_t res = 0;

    if (__builtin_mul_overflow(count, sizeof(T), &res)) [[unlikely]]
        throw std::bad_alloc();

    if (__builtin_add_overflow(res, N, &res)) [[unlikely]]
        throw std::bad_alloc();

    return allocBytes(res);
}

[[gnu::always_inline]]
Value * EvalMemory::allocValue()
{
#if !NIX_USE_BOEHMGC
    /* Unforcing (the only user of evalRegion) is never active on a Boehm
       build, so this region check is compiled out there: allocValue keeps
       its original Boehm-build cost exactly. */
    if (evalRegion.active()) [[unlikely]] {
        stats.nrValues++;
        return (Value *) evalRegion.allocCell();
    }
#endif
#if NIX_USE_BOEHMGC
    /* Allocation cache for GC'd Value objects. Boehm GC is already a global resource, so thread_local is
       a natural solution. Multiple EvalState instances on the same thread will reuse the same cache. */
    static thread_local std::shared_ptr<void *> valueAllocCache{
        std::allocate_shared<void *>(traceable_allocator<void *>(), nullptr)};

    /* We use the boehm batch allocator to speed up allocations of Values (of which there are many).
       GC_malloc_many returns a linked list of objects of the given size, where the first word
       of each object is also the pointer to the next object in the list. This also means that we
       have to explicitly clear the first word of every object we take. */
    if (!*valueAllocCache) {
        *valueAllocCache = GC_malloc_many(sizeof(Value));
        if (!*valueAllocCache)
            throw std::bad_alloc();
    }

    /* GC_NEXT is a convenience macro for accessing the first word of an object.
       Take the first list item, advance the list to the next item, and clear the next pointer. */
    void * p = *valueAllocCache;
    *valueAllocCache = GC_NEXT(p);
    GC_NEXT(p) = nullptr;
#else
    void * p = allocBytes(sizeof(Value));
#endif

    stats.nrValues++;
    return (Value *) p;
}

[[gnu::always_inline]]
Env & EvalMemory::allocEnv(size_t size)
{
    stats.nrEnvs++;
    stats.nrValuesInEnvs += size;

    Env * env;

#if !NIX_USE_BOEHMGC
    /* While the region is active, record each Env's size, so that it can be
       copied out of the region: in the bump region's side table. Unforcing
       (the only user of evalRegion) is never active on a Boehm build, so
       this is compiled out there. */
    if (evalRegion.active()) [[unlikely]] {
        auto p = evalRegion.bump(sizeof(Env) + size * sizeof(Value *));
        evalRegion.noteEnvSize(p, size);
        return *(Env *) p;
    }
#endif

#if NIX_USE_BOEHMGC
    if (size == 1) {
        /* Allocation cache for size-1 Env objects. Boehm GC is already a global resource, so thread_local is
           a natural solution. Multiple EvalState instances on the same thread will reuse the same cache. */
        static thread_local std::shared_ptr<void *> env1AllocCache{
            std::allocate_shared<void *>(traceable_allocator<void *>(), nullptr)};
        /* see allocValue for explanations. */
        if (!*env1AllocCache) {
            *env1AllocCache = GC_malloc_many(sizeof(Env) + sizeof(Value *));
            if (!*env1AllocCache)
                throw std::bad_alloc();
        }

        void * p = *env1AllocCache;
        *env1AllocCache = GC_NEXT(p);
        GC_NEXT(p) = nullptr;
        env = (Env *) p;
    } else
#endif
        env = (Env *) allocBytes<Value *, sizeof(Env)>(size);

    /* We assume that env->values has been cleared by the allocator; maybeThunk() and lookupVar fromWith expect this. */

    return *env;
}

[[gnu::always_inline]]
void EvalState::forceValue(Value & v, const PosIdx pos)
{
    /* inc2b-1: ONE force site serves both eval-cores == 1 and > 1 -- there is
       no `forcingArmed` guard on the hot path, so the inlined frame is inc2a's
       on every recursion path (a per-force guard spilled a 16-byte Value slot
       in register-tight primops such as prim_getAttr, pushing its edge sooner
       than base; this reordering removes it). The disarmed path is behaviourally
       byte-for-byte inc2a: `logPreImage` first, the same thunk reads, the same
       blackhole CAS, the same `if (env) eval else <infinite-recursion>`, the
       same catch, the app arm's `Value savedApp` + callFunction inline.

       The one structural change vs inc2a -- reading word 0 BEFORE env/expr and
       CASing that exact word -- is what lets the single path be race-safe
       without a mode branch: a concurrent pending-mark makes the loser's CAS
       fail, so the election has a unique winner and no thunk is ever
       double-evaluated. Arming is consulted only in the cold, out-of-line
       `forceValuePending` (self-cycle/own-recursion -> infinite recursion when
       the executor is disabled; another worker's in-progress force -> wait when
       enabled), never on the inlined hot path. At eval-cores == 1 there is no
       contention: the CAS always wins and env==null is always a genuine
       self-cycle, so this is exactly inc2a. `logPreImage` stays BEFORE the mark
       (value-identical while the forcer is unique); the winner-only relocation
       is inc2c. */
    if (v.isThunk()) {
        logPreImage(v);
        uint64_t expected = v.fvWord0Plain();
        if (Value::fvIsPendingTag(expected)) [[unlikely]] {
            /* The cell is already pending. At eval-cores == 1 this is this
               thread re-entering a thunk it is already forcing -- a self-cycle;
               forceValueOnPending reproduces inc2a's env==null arm exactly
               (throw infinite recursion, endLoggedForce +
               handleEvalExceptionForThunk with env==null for the position fix-up
               and mkFailed, then rethrow so the enclosing initial force adds the
               "entering the infinite recursion" trace). At eval-cores > 1 another
               worker is forcing it: wait. Handling pending HERE -- before
               env/expr -- is what lets the won path below assume a real, non-null
               env and drop inc2a's `if (env)`, so the inlined frame never exceeds
               inc2a's on any recursion path (no arming guard; the only returning
               wait is this cold, out-of-line call with nothing but `v` live). */
            forceValueOnPending(v, pos);
            return;
        }
        Env * env = v.thunk().env;
        Expr * expr = v.thunk().expr;
        /* CAS the exact (real-thunk) word we read to pdPending. The word0-first
           order makes this race-safe: if another worker marks the thunk pending
           between our read and this CAS, the CAS fails and we wait rather than
           double-evaluating. At eval-cores == 1 it cannot fail (no contention),
           so this branch is inc2a's `assert(won)` -- never taken. */
        if (!v.fvMarkPending(expected)) [[unlikely]] {
            endLoggedForce(v);
            forceValuePending(v);
            return;
        }
        try {
            /* env is non-null: a real thunk was elected (pending was handled
               above, and a lost CAS returned above). */
            expr->eval(*this, *env, v);
        } catch (...) {
            endLoggedForce(v);
            handleEvalExceptionForThunk(env, expr, v, pos);
            throw;
        }
        endLoggedForce(v);
    } else if (v.isApp()) {
        logPreImage(v);
        Value savedApp = v;
        try {
            callFunction(*v.app().left, *v.app().right, v, pos);
        } catch (...) {
            endLoggedForce(v);
            handleEvalExceptionForApp(v, savedApp);
            throw;
        }
        endLoggedForce(v);
    } else if (v.isFailed()) {
        handleEvalFailed(v, pos);
    }
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

template<typename Cb>
std::invoke_result_t<Cb, Value *, bool>
EvalState::peelToStringOutPath(const PosIdx pos, Value & v, bool checkToStringReturn, Cb && cb)
{
    using R = std::invoke_result_t<Cb, Value *, bool>;
    bool cameThroughToString = false;

    auto peel = [&, &state = *this](this const auto & peel, const PosIdx pos, Value & v) -> R {
        state.forceValue(v, pos);
        auto vType = v.type();
        if (vType != nAttrs) {
            /* Trivially string-coercible types (i.e. coerceMore = false compatible)
               get to return happily.
               String and path terminate happily. External needs to be handled
               by caller. E.g. `printValueAsJSON` routes to `ExternalValueBase::printValueAsJSON`,
               `coerceToString` to `ExternalValueBase::coerceToString`).
               Everything else violates the "`__toString` returns a
               string" contract.
               */
            if (checkToStringReturn && cameThroughToString && vType != nString && vType != nPath && vType != nExternal)
                state
                    .error<TypeError>(
                        "`__toString` should return a string, but got %1%: %2%",
                        showType(v),
                        ValuePrinter(state, v, errorPrintOptions))
                    .atPos(pos)
                    .debugThrow();
            return std::forward<Cb>(cb)(&v, cameThroughToString);
        }
        if (auto i = v.attrs()->get(state.s.toString)) {
            Value & v1 = *state.allocValue();
            try {
                state.callFunction(*i->value, v, v1, i->pos);
            } catch (Error & e) {
                e.addTrace(state.positions[i->pos], "while calling the `__toString` attribute");
                throw;
            }
            cameThroughToString = true;
            try {
                auto _level = state.addCallDepth(pos);
                return peel(i->pos, v1);
            } catch (Error & e) {
                e.addTrace(
                    state.positions[i->pos], "while %s the result of the `__toString` attribute", WhileTryingToUse{v1});
                throw;
            }
        }
        if (auto i = v.attrs()->get(state.s.outPath)) {
            try {
                state.forceValue(*i->value, i->pos);
                auto _level = state.addCallDepth(pos);
                return peel(i->pos, *i->value);
            } catch (Error & e) {
                e.addTrace(state.positions[i->pos], "while %s the `outPath` attribute", WhileTryingToUse{*i->value});
                throw;
            }
        }
        /* Can't peel more. (no `__toString`, no `outPath`).
           If we came through a `__toString` call at any depth, this violates
           the string-return contract. (usually not enforced) */
        if (checkToStringReturn && cameThroughToString)
            state
                .error<TypeError>(
                    "`__toString` must return a string, but got %1%: %2%",
                    showType(v),
                    ValuePrinter(state, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return std::forward<Cb>(cb)(&v, cameThroughToString);
    };
    return peel(pos, v);
}

} // namespace nix
