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
    /* inc2b-1/2b-2: BOTH forceable arms (thunk and app -- both primary pdThunk)
       run the same guard-free election: read word0, pre-check pending, CAS the
       exact word we read to a pending word tagged with our thread id, dispatch
       to expr->eval (thunk) / callFunction (app) on the win, wait on loss. There
       is no `forcingArmed` guard, so the inlined frame stays inc2a's on every
       recursion path (an earlier per-force guard spilled a 16-byte slot in
       register-tight primops, pushing their edge sooner than base).

       Race-safety comes from the word0-first order and the exact-word CAS: a
       concurrent pending-mark makes the loser's CAS fail, so there is exactly one
       winner and no cell (thunk OR app) is ever double-evaluated or plain-stored
       by two threads. The thread id in the pending word lets a same-thread
       re-entry (self-cycle) be told from a cross-thread wait. All the
       cold/concurrency-only handling -- self-cycle diagnosis and waiting -- lives
       in the out-of-line `forceValueOnPending` / `forceValuePending`, never on the
       inlined hot path. At eval-cores == 1 the CAS always wins and a pending cell
       is always our own self-cycle, so each arm is behaviourally inc2a (the thunk
       arm byte-for-byte; the app arm adds only the never-taken election). The one
       inline per-force cost over inc2a is reading `myEvalThreadId` (an
       initial-exec TLS load) for the pending word; it does not grow the hot frame
       past base. inc2c-1: `logPreImage` is RELOCATED into the election
       (`electThunk`/`electApp`), appended winner-only on the CAS win using the
       captured pre-image. That removes the inline `logPreImage` call (and its
       `isolationDepth` read) from BOTH force arms here, so the inlined force
       frame is <= inc2b-2's on every recursion path; the only inline unforce
       read left on the force site is `endLoggedForce`'s `isolationDepth` guard. */
    if (v.isThunk()) {
        /* electThunk runs the whole election out of line (word0 read, pending
           pre-check, env/expr capture, CAS to a pending word tagged with our
           thread id) and returns the captured env/expr. env==null means it was
           already handled here (we waited for another worker, or lost the CAS and
           waited); a self-cycle throws from within. Keeping the TLS read and the
           env/expr-across-a-call out of line is what holds the inlined frame at
           inc2a's. */
        Value::ClosureThunk t = electThunk(v, pos);
        if (!t.env) [[unlikely]]
            return;
        try {
            t.expr->eval(*this, *t.env, v);
        } catch (...) {
            endLoggedForce(v);
            handleEvalExceptionForThunk(t.env, t.expr, v, pos);
            throw;
        }
        endLoggedForce(v);
    } else if (v.isApp()) {
        /* Apps are forceable cells too (primary pdThunk, app subtag) and share
           the SAME election via electApp: a shared tApp forced by two threads
           must be evaluated once, not double-evaluated and plain-stored (2b-2
           fix). left==null means already handled. inc2c-1: `logPreImage`
           relocated into electApp (winner-only). */
        Value::FunctionApplicationThunk a = electApp(v, pos);
        if (!a.left) [[unlikely]]
            return;
        try {
            callFunction(*a.left, *a.right, v, pos);
        } catch (...) {
            endLoggedForce(v);
            Value savedApp;
            savedApp.mkApp(a.left, a.right);
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
