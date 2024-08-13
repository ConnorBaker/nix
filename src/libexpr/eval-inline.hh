#pragma once
///@file

#include "print.hh"
#include "eval.hh"
#include "eval-error.hh"

namespace nix {

/**
 * Note: Various places expect the allocated memory to be zeroed.
 */
[[gnu::always_inline]]
inline void * allocBytes(size_t n)
{
    void * p;
#if HAVE_BOEHMGC
    p = GC_MALLOC(n);
#else
    p = calloc(n, 1);
#endif
    if (!p) throw std::bad_alloc();
    return p;
}


[[gnu::always_inline]]
Value * EvalState::allocValue()
{
#if HAVE_BOEHMGC
    /* We use the boehm batch allocator to speed up allocations of Values (of which there are many).
       GC_malloc_many returns a linked list of objects of the given size, where the first word
       of each object is also the pointer to the next object in the list. This also means that we
       have to explicitly clear the first word of every object we take. */
    thread_local static std::shared_ptr<void *> valueAllocCache{std::allocate_shared<void *>(traceable_allocator<void *>(), nullptr)};

    if (!*valueAllocCache) {
        *valueAllocCache = GC_malloc_many(sizeof(Value));
        if (!*valueAllocCache) throw std::bad_alloc();
    }

    /* GC_NEXT is a convenience macro for accessing the first word of an object.
       Take the first list item, advance the list to the next item, and clear the next pointer. */
    void * p = *valueAllocCache;
    *valueAllocCache = GC_NEXT(p);
    GC_NEXT(p) = nullptr;
#else
    void * p = allocBytes(sizeof(Value));
#endif

    nrValues++;
    return (Value *) p;
}


[[gnu::always_inline]]
Env & EvalState::allocEnv(size_t size)
{
    nrEnvs++;
    nrValuesInEnvs += size;

    Env * env;

#if HAVE_BOEHMGC
    if (size == 1) {
        /* see allocValue for explanations. */
        thread_local static std::shared_ptr<void *> env1AllocCache{std::allocate_shared<void *>(traceable_allocator<void *>(), nullptr)};

        if (!*env1AllocCache) {
            *env1AllocCache = GC_malloc_many(sizeof(Env) + sizeof(Value *));
            if (!*env1AllocCache) throw std::bad_alloc();
        }

        void * p = *env1AllocCache;
        *env1AllocCache = GC_NEXT(p);
        GC_NEXT(p) = nullptr;
        env = (Env *) p;
    } else
#endif
        env = (Env *) allocBytes(sizeof(Env) + size * sizeof(Value *));

    /* We assume that env->values has been cleared by the allocator; maybeThunk() and lookupVar fromWith expect this. */

    return *env;
}


[[gnu::always_inline]]
void EvalState::forceValue(Value & v, const PosIdx pos)
{
    auto type = v.internalType.load(std::memory_order_acquire);

    if (isFinished(type))
        goto done;

    if (type == tThunk) {
        try {
            if (!v.internalType.compare_exchange_strong(type, tPending, std::memory_order_acquire, std::memory_order_acquire)) {
                if (type == tPending || type == tAwaited) {
                    waitOnThunk(v, type == tAwaited);
                    goto done;
                }
                if (isFinished(type))
                    goto done;
                printError("NO LONGER THUNK %x %d", this, type);
                abort();
            }
            Env * env = v.payload.thunk.env;
            Expr * expr = v.payload.thunk.expr;
            expr->eval(*this, *env, v);
        } catch (...) {
            v.mkFailed();
            throw;
        }
    }
    #if 0
    if (v.isThunk()) {
        try {
            v.mkBlackhole();
            //checkInterrupt();
            expr->eval(*this, *env, v);
        } catch (...) {
            v.mkThunk(env, expr);
            tryFixupBlackHolePos(v, pos);
            throw;
        }
    }
    #endif
    else if (type == tApp) {
        try {
            if (!v.internalType.compare_exchange_strong(type, tPending, std::memory_order_acquire, std::memory_order_acquire)) {
                if (type == tPending || type == tAwaited) {
                    waitOnThunk(v, type == tAwaited);
                    goto done;
                }
                if (isFinished(type))
                    goto done;
                printError("NO LONGER APP %x %d", this, type);
                abort();
            }
            callFunction(*v.payload.app.left, *v.payload.app.right, v, pos);
        } catch (...) {
            v.mkFailed();
            throw;
        }
    }
    else if (type == tPending || type == tAwaited)
        type = waitOnThunk(v, type == tAwaited);
    else
        abort();

 done:
    if (type == tFailed)
        std::rethrow_exception(v.payload.failed->ex);
}


[[gnu::always_inline]]
inline void EvalState::forceAttrs(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    forceAttrs(v, [&]() { return pos; }, errorCtx);
}


template <typename Callable>
[[gnu::always_inline]]
inline void EvalState::forceAttrs(Value & v, Callable getPos, std::string_view errorCtx)
{
    PosIdx pos = getPos();
    forceValue(v, pos);
    if (v.type() != nAttrs) {
        error<TypeError>(
            "expected a set but found %1%: %2%",
            showType(v),
            ValuePrinter(*this, v, errorPrintOptions)
        ).withTrace(pos, errorCtx).debugThrow();
    }
}


[[gnu::always_inline]]
inline void EvalState::forceList(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    forceValue(v, pos);
    if (!v.isList()) {
        error<TypeError>(
            "expected a list but found %1%: %2%",
            showType(v),
            ValuePrinter(*this, v, errorPrintOptions)
        ).withTrace(pos, errorCtx).debugThrow();
    }
}


}
