#pragma once
///@file
/**
 * @brief Allocator abstraction layer for Nix expression evaluator.
 *
 * This header provides a compile-time polymorphic allocator interface
 * that abstracts over different garbage collection backends:
 * - BoehmAllocator: Uses Boehm GC (NIX_USE_BOEHMGC)
 * - GHCAllocator: Uses GHC RTS garbage collector (NIX_USE_GHC_GC)
 * - FallbackAllocator: Uses standard malloc/calloc (no GC)
 *
 * The allocator selection happens at compile time to preserve the
 * performance-critical inline allocation paths.
 */

#include "nix/expr/config.hh"

#include <cstddef>
#include <cstdlib>
#include <new>
#include <stdexcept>

#if NIX_USE_BOEHMGC
#  define GC_INCLUDE_NEW
#  define GC_THREADS 1
#  include <gc/gc.h>
#  include <gc/gc_cpp.h>
#  include <gc/gc_allocator.h>
#elif NIX_USE_GHC_GC
#  include "nix/expr/ghc-gc.hh"
#else
#  include <cstdlib>
#endif

namespace nix {

/**
 * Allocator backend using Boehm GC.
 *
 * Provides traced and atomic allocation through GC_MALLOC and GC_MALLOC_ATOMIC.
 * Also provides batch allocation via GC_malloc_many for hot allocation paths.
 */
struct BoehmAllocator
{
    /**
     * Allocate traced memory (may contain pointers that need to be scanned).
     * Memory is zeroed.
     */
    [[gnu::always_inline]]
    static void * allocBytes(size_t n)
    {
#if NIX_USE_BOEHMGC
        void * p = GC_MALLOC(n);
        if (!p)
            throw std::bad_alloc();
        return p;
#else
        (void)n;
        throw std::logic_error("BoehmAllocator::allocBytes called without Boehm GC");
#endif
    }

    /**
     * Allocate atomic (pointer-free) memory.
     * Memory is NOT zeroed.
     */
    [[gnu::always_inline]]
    static void * allocAtomic(size_t n)
    {
#if NIX_USE_BOEHMGC
        void * p = GC_MALLOC_ATOMIC(n);
        if (!p)
            throw std::bad_alloc();
        return p;
#else
        (void)n;
        throw std::logic_error("BoehmAllocator::allocAtomic called without Boehm GC");
#endif
    }

    /**
     * Allocate a Value (16 bytes, traced).
     * For Boehm, this is the same as allocBytes(16).
     */
    [[gnu::always_inline]]
    static void * allocValue()
    {
#if NIX_USE_BOEHMGC
        void * p = GC_MALLOC(16);  // sizeof(Value) = 16
        if (!p)
            throw std::bad_alloc();
        return p;
#else
        throw std::logic_error("BoehmAllocator::allocValue called without Boehm GC");
#endif
    }

    /**
     * Allocate an Env with the given number of Value* slots.
     * Size is sizeof(Env) + numSlots * sizeof(Value*).
     */
    [[gnu::always_inline]]
    static void * allocEnv(size_t numSlots)
    {
#if NIX_USE_BOEHMGC
        // sizeof(Env) = 8 (just an Env* pointer), sizeof(Value*) = 8
        size_t totalSize = 8 + numSlots * 8;
        void * p = GC_MALLOC(totalSize);
        if (!p)
            throw std::bad_alloc();
        return p;
#else
        (void)numSlots;
        throw std::logic_error("BoehmAllocator::allocEnv called without Boehm GC");
#endif
    }

    /**
     * Allocate a Bindings with the given capacity.
     * Size is sizeof(Bindings) + capacity * sizeof(Attr).
     */
    [[gnu::always_inline]]
    static void * allocBindings(size_t capacity)
    {
#if NIX_USE_BOEHMGC
        // sizeof(Bindings) = 24, sizeof(Attr) = 16
        size_t totalSize = 24 + capacity * 16;
        void * p = GC_MALLOC(totalSize);
        if (!p)
            throw std::bad_alloc();
        return p;
#else
        (void)capacity;
        throw std::logic_error("BoehmAllocator::allocBindings called without Boehm GC");
#endif
    }

    /**
     * Allocate a list element array.
     * Size is numElems * sizeof(Value*).
     */
    [[gnu::always_inline]]
    static void * allocList(size_t numElems)
    {
#if NIX_USE_BOEHMGC
        // sizeof(Value*) = 8
        size_t totalSize = numElems * 8;
        void * p = GC_MALLOC(totalSize);
        if (!p)
            throw std::bad_alloc();
        return p;
#else
        (void)numElems;
        throw std::logic_error("BoehmAllocator::allocList called without Boehm GC");
#endif
    }

    /**
     * Batch allocate objects of given size.
     * Returns a linked list where the first word of each object points to the next.
     * Caller must clear the first word after taking each object.
     */
    [[gnu::always_inline]]
    static void * allocMany(size_t objectSize)
    {
#if NIX_USE_BOEHMGC
        void * p = GC_malloc_many(objectSize);
        if (!p)
            throw std::bad_alloc();
        return p;
#else
        (void)objectSize;
        throw std::logic_error("BoehmAllocator::allocMany called without Boehm GC");
#endif
    }

    /**
     * Get next object in a batch allocation list.
     */
    [[gnu::always_inline]]
    static void * getNext(void * p)
    {
#if NIX_USE_BOEHMGC
        return GC_NEXT(p);
#else
        (void)p;
        return nullptr;
#endif
    }

    /**
     * Set next pointer in a batch allocation list.
     */
    [[gnu::always_inline]]
    static void setNext(void * p, void * next)
    {
#if NIX_USE_BOEHMGC
        GC_NEXT(p) = next;
#else
        (void)p;
        (void)next;
#endif
    }

    /**
     * Trigger a garbage collection.
     */
    static void performGC()
    {
#if NIX_USE_BOEHMGC
        GC_gcollect();
#endif
    }
};

/**
 * Allocator backend using GHC's garbage collector.
 *
 * Allocations go through the GHC RTS and are managed by GHC's generational
 * garbage collector. Objects are kept alive via StablePtr references.
 */
struct GHCAllocator
{
    /**
     * Allocate traced memory (may contain pointers that need to be scanned).
     * Memory is zeroed.
     */
    [[gnu::always_inline]]
    static void * allocBytes(size_t n)
    {
#if NIX_USE_GHC_GC
        return ghc::allocBytes(n);
#else
        (void)n;
        throw std::logic_error("GHCAllocator::allocBytes called without GHC GC");
#endif
    }

    /**
     * Allocate atomic (pointer-free) memory.
     * Memory is NOT zeroed by GHC, so we zero it ourselves for consistency.
     */
    [[gnu::always_inline]]
    static void * allocAtomic(size_t n)
    {
#if NIX_USE_GHC_GC
        void * p = ghc::allocBytesAtomic(n);
        // Note: GHC doesn't zero atomic allocations, caller must handle this
        return p;
#else
        (void)n;
        throw std::logic_error("GHCAllocator::allocAtomic called without GHC GC");
#endif
    }

    /**
     * Allocate a Value (16 bytes, traced).
     * Uses dedicated Value allocation with separate tracking.
     */
    [[gnu::always_inline]]
    static void * allocValue()
    {
#if NIX_USE_GHC_GC
        return ghc::allocValue();
#else
        throw std::logic_error("GHCAllocator::allocValue called without GHC GC");
#endif
    }

    /**
     * Allocate an Env with the given number of Value* slots.
     * Uses dedicated Env allocation with separate tracking.
     */
    [[gnu::always_inline]]
    static void * allocEnv(size_t numSlots)
    {
#if NIX_USE_GHC_GC
        return ghc::allocEnv(numSlots);
#else
        (void)numSlots;
        throw std::logic_error("GHCAllocator::allocEnv called without GHC GC");
#endif
    }

    /**
     * Allocate a Bindings with the given capacity.
     * Uses dedicated Bindings allocation with separate tracking.
     */
    [[gnu::always_inline]]
    static void * allocBindings(size_t capacity)
    {
#if NIX_USE_GHC_GC
        return ghc::allocBindings(capacity);
#else
        (void)capacity;
        throw std::logic_error("GHCAllocator::allocBindings called without GHC GC");
#endif
    }

    /**
     * Allocate a list element array.
     * Uses dedicated list allocation with separate tracking.
     */
    [[gnu::always_inline]]
    static void * allocList(size_t numElems)
    {
#if NIX_USE_GHC_GC
        return ghc::allocList(numElems);
#else
        (void)numElems;
        throw std::logic_error("GHCAllocator::allocList called without GHC GC");
#endif
    }

    /**
     * Batch allocate objects of given size.
     * Returns a linked list where the first word of each object points to the next.
     * Caller must clear the first word after taking each object.
     * (Iteration 16: Implemented batch allocation for performance)
     */
    [[gnu::always_inline]]
    static void * allocMany(size_t objectSize)
    {
#if NIX_USE_GHC_GC
        return ghc::allocMany(objectSize);
#else
        (void)objectSize;
        throw std::logic_error("GHCAllocator::allocMany called without GHC GC");
#endif
    }

    /**
     * Get next object in a batch allocation list.
     */
    [[gnu::always_inline]]
    static void * getNext(void * p)
    {
#if NIX_USE_GHC_GC
        return ghc::getNext(p);
#else
        (void)p;
        return nullptr;
#endif
    }

    /**
     * Set next pointer in a batch allocation list.
     */
    [[gnu::always_inline]]
    static void setNext(void * p, void * next)
    {
#if NIX_USE_GHC_GC
        ghc::setNext(p, next);
#else
        (void)p;
        (void)next;
#endif
    }

    /**
     * Trigger a garbage collection.
     */
    static void performGC()
    {
#if NIX_USE_GHC_GC
        ghc::performGC();
#endif
    }
};

/**
 * Fallback allocator using standard malloc/calloc.
 * No garbage collection - memory leaks unless manually freed.
 * Used when neither Boehm nor GHC GC is enabled.
 */
struct FallbackAllocator
{
    [[gnu::always_inline]]
    static void * allocBytes(size_t n)
    {
        void * p = std::calloc(n, 1);
        if (!p)
            throw std::bad_alloc();
        return p;
    }

    [[gnu::always_inline]]
    static void * allocAtomic(size_t n)
    {
        void * p = std::malloc(n);
        if (!p)
            throw std::bad_alloc();
        return p;
    }

    /**
     * Allocate a Value (16 bytes, traced).
     * For fallback, this is the same as allocBytes(16).
     */
    [[gnu::always_inline]]
    static void * allocValue()
    {
        return allocBytes(16);  // sizeof(Value) = 16
    }

    /**
     * Allocate an Env with the given number of Value* slots.
     * For fallback, this is the same as allocBytes(8 + numSlots * 8).
     */
    [[gnu::always_inline]]
    static void * allocEnv(size_t numSlots)
    {
        // sizeof(Env) = 8 (just an Env* pointer), sizeof(Value*) = 8
        return allocBytes(8 + numSlots * 8);
    }

    /**
     * Allocate a Bindings with the given capacity.
     * For fallback, this is the same as allocBytes(24 + capacity * 16).
     */
    [[gnu::always_inline]]
    static void * allocBindings(size_t capacity)
    {
        // sizeof(Bindings) = 24, sizeof(Attr) = 16
        return allocBytes(24 + capacity * 16);
    }

    /**
     * Allocate a list element array.
     * For fallback, this is the same as allocBytes(numElems * 8).
     */
    [[gnu::always_inline]]
    static void * allocList(size_t numElems)
    {
        // sizeof(Value*) = 8
        return allocBytes(numElems * 8);
    }

    /**
     * Fallback doesn't support batch allocation.
     * Returns a single allocation.
     */
    [[gnu::always_inline]]
    static void * allocMany(size_t objectSize)
    {
        return allocBytes(objectSize);
    }

    [[gnu::always_inline]]
    static void * getNext(void * /* p */)
    {
        return nullptr;
    }

    [[gnu::always_inline]]
    static void setNext(void * /* p */, void * /* next */)
    {
        // No-op
    }

    static void performGC()
    {
        // No GC to perform
    }
};

/**
 * Compile-time selected allocator type.
 *
 * This type alias selects the appropriate allocator backend based on
 * compile-time configuration:
 * - NIX_USE_BOEHMGC=1 -> BoehmAllocator
 * - NIX_USE_GHC_GC=1 -> GHCAllocator
 * - Neither -> FallbackAllocator
 */
#if NIX_USE_BOEHMGC
using Allocator = BoehmAllocator;
#elif NIX_USE_GHC_GC
using Allocator = GHCAllocator;
#else
using Allocator = FallbackAllocator;
#endif

/**
 * Check if batch allocation is supported by the current allocator.
 * Boehm GC supports true batch allocation via GC_malloc_many.
 * GHC GC also supports batch allocation via nix_ghc_alloc_many.
 */
constexpr bool allocatorSupportsBatchAllocation()
{
#if NIX_USE_BOEHMGC || NIX_USE_GHC_GC
    return true;
#else
    return false;
#endif
}

} // namespace nix
