#pragma once
///@file

#include "nix/expr/print.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/thunk-hash.hh"
#include "nix/util/signals.hh"

#include <boost/unordered/concurrent_flat_map.hpp>

#include <cassert>
#include <iostream>
#include <limits>
#include <typeinfo>

namespace nix {

/**
 * Note: Various places expect the allocated memory to be zeroed.
 */
[[gnu::always_inline]]
inline void * EvalMemory::allocBytes(size_t n)
{
    void * p;
#if NIX_USE_BOEHMGC
    p = GC_MALLOC(n);
#else
    p = calloc(n, 1);
#endif
    if (!p)
        throw std::bad_alloc();
    return p;
}

[[gnu::always_inline]]
Value * EvalMemory::allocValue()
{
#if NIX_USE_BOEHMGC
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

#if NIX_USE_BOEHMGC
    if (size == 1) {
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
        // GC_malloc_many only clears the first word (env->up). Explicitly
        // zero the value slot to avoid reading garbage in env hashing.
        env->values[0] = nullptr;
    } else
#endif
        env = (Env *) allocBytes(sizeof(Env) + size * sizeof(Value *));

    /* Store the size for use in content-based hashing.
       This enables iterating over env->values[0..size-1] without
       relying on external size tracking or GC internals. */
    assert(size <= std::numeric_limits<uint32_t>::max() && "env size exceeds uint32_t capacity");
    env->size = static_cast<uint32_t>(size);

    /* We assume that env->values has been cleared by the allocator; maybeThunk() and lookupVar fromWith expect this. */

    return *env;
}

/**
 * Check if a value should NOT be cached (shallow check).
 *
 * Returns true if the value:
 * - Contains unevaluated thunks (may have pending side effects)
 * - Is or contains path values (accessor pointers are context-dependent)
 *
 * This is a SHALLOW check - only looks at immediate children.
 * Deep detection would require traversing the entire value graph,
 * which is expensive. Shallow check is sufficient because:
 * - If v is a list/attrs with uncacheable elements, don't cache v
 * - When thunks are later forced, they get their own cache entries
 */
[[gnu::always_inline]]
inline bool valueIsUncacheable(const Value & v)
{
    // Path values contain SourceAccessor pointers that are context-dependent.
    // Caching a path from one source tree and returning it for another
    // causes "must be in the same source tree" errors.
    if (v.type() == nPath)
        return true;

    if (v.isList()) {
        auto list = v.listView();
        for (auto * elem : list) {
            if (elem) {
                if (elem->isThunk() || elem->isApp())
                    return true;
                if (elem->type() == nPath)
                    return true;
            }
        }
        return false;
    }

    if (v.type() == nAttrs) {
        for (const auto & attr : *v.attrs()) {
            if (attr.value) {
                if (attr.value->isThunk() || attr.value->isApp())
                    return true;
                if (attr.value->type() == nPath)
                    return true;
            }
        }
        return false;
    }

    // Primitives (int, float, bool, null, string) are safe to cache
    return false;
}

[[gnu::always_inline]]
void EvalState::forceValue(Value & v, const PosIdx pos)
{
    if (v.isThunk()) {
        Env * env = v.thunk().env;
        assert(env || v.isBlackhole());
        Expr * expr = v.thunk().expr;

        // Thunk memoization: check if we've already forced an equivalent thunk.
        // The structural hash uses (exprHash, envHash) where envHash uses
        // content-based hashing (via env.size field), making it stable.
        //
        // Prerequisites for safe memoization (all complete):
        // 1. Content-based env hashing (env.size field) - DONE
        // 2. Impure token tracking to skip side-effectful thunks - DONE
        // 3. valueIsUncacheable check to skip thunks and paths - DONE
        // 4. Expression hash caching for performance - DONE
        // 5. Value hash caching for performance - DONE
        StructuralHash thunkHash;
        bool useMemoization = true;

        if (useMemoization) {
            // NOTE: We pass nullptr for valueHashCache because Value* pointers can be
            // reused by GC, causing stale cache entries. The exprHashCache is safe
            // because Expr* are never reused during an evaluation.
            thunkHash = computeThunkStructuralHash(expr, env, trylevel, symbols, &exprHashCache, nullptr);

            // Cache lookup using concurrent_flat_map's cvisit for thread-safe access
            bool cacheHit = false;
            thunkMemoCache->cvisit(thunkHash, [&](const auto & entry) {
                // GC Safety: verify the cached entry was created in the current GC cycle.
                // After a GC run, Value* pointers may be reused for different objects,
                // so entries from previous cycles are stale and must be skipped.
                if (entry.second.gcCycle == getGCCycles()) {
                    // Cache hit: copy the cached result
                    nrThunkMemoHits++;
                    v = *entry.second.value;
                    cacheHit = true;
                } else {
                    // Stale entry from previous GC cycle - treat as miss
                    nrThunkMemoStaleHits++;
                }
            });
            if (cacheHit) return;
        }

        // Save impure token before forcing - if it changes, thunk was impure
        uint64_t tokenBefore = getImpureToken();

        try {
            v.mkBlackhole();
            checkInterrupt();
            if (env) [[likely]]
                expr->eval(*this, *env, v);
            else
                ExprBlackHole::throwInfiniteRecursionError(*this, v);
        } catch (...) {
            v.mkThunk(env, expr);
            tryFixupBlackHolePos(v, pos);
            throw;
        }

        // Cache the result if memoization is enabled AND no impure operations occurred
        // AND the value is cacheable (no thunks, no paths)
        if (useMemoization) {
            if (getImpureToken() != tokenBefore) {
                // Thunk called impure builtins (trace, currentTime, etc.)
                // Don't cache to ensure side effects happen on each force
                nrThunkMemoImpureSkips++;
            } else if (valueIsUncacheable(v)) {
                // Value contains thunks (pending side effects) or paths (context-dependent).
                // Don't cache to ensure correct behavior.
                nrThunkMemoLazySkips++;
            } else {
                nrThunkMemoMisses++;
                // Allocate a new Value to store in the cache.
                // We must copy because v might be a stack variable or get reused.
                Value * cached = mem.allocValue();
                *cached = v;
                // Store with current GC cycle for staleness detection.
                // Use insert_or_assign to overwrite stale entries from previous GC cycles
                // (emplace would leave stale entries in place, causing perpetual misses).
                thunkMemoCache->insert_or_assign(thunkHash, ThunkMemoCacheEntry{cached, getGCCycles()});
            }
        }
    } else if (v.isApp())
        callFunction(*v.app().left, *v.app().right, v, pos);
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
