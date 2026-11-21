#pragma once
///@file

#include <cstddef>
#include <cstring>

// For `NIX_USE_BOEHMGC`
#include "nix/expr/config.hh"
#include "nix/util/fun.hh"

#if NIX_USE_BOEHMGC

#  define GC_INCLUDE_NEW
#  define GC_THREADS 1

#  include <gc/gc.h>
#  include <gc/gc_cpp.h>
#  include <gc/gc_allocator.h>

/**
 * A `traceable_allocator` that zeroes destroyed elements.
 *
 * Boehm scans the backing storage of containers using `traceable_allocator`,
 * including slots whose elements have been erased. Open-addressing containers
 * such as `boost::concurrent_flat_map` keep the element array around after
 * `erase()` or `clear()`, so the stale pointers left in those slots keep
 * unreachable objects alive. Zeroing the storage on destruction avoids that.
 */
template<typename T>
struct gc_root_allocator : traceable_allocator<T>
{
    using traceable_allocator<T>::traceable_allocator;

    template<typename U>
    struct rebind
    {
        using other = gc_root_allocator<U>;
    };

    template<typename U>
    void destroy(U * p)
    {
        p->~U();
        std::memset(static_cast<void *>(p), 0, sizeof(U));
    }
};

#else

#  include <memory>

/* Some dummy aliases for Boehm GC definitions to reduce the number of
   #ifdefs. */

template<typename T>
using traceable_allocator = std::allocator<T>;

template<typename T>
using gc_allocator = std::allocator<T>;

template<typename T>
using gc_root_allocator = std::allocator<T>;

#  define GC_MALLOC_ATOMIC std::malloc

struct gc
{};

struct gc_cleanup
{};

#endif

namespace nix {

/**
 * Initialise the Boehm GC, if applicable.
 */
void initGC();

/**
 * Make sure `initGC` has already been called.
 */
void assertGCInitialized();

/**
 * Collect garbage, then give every page the collector's heap no longer
 * uses back to the operating system.
 *
 * The collector otherwise keeps its heap at its largest size for the
 * rest of the process, so a command that evaluates and then waits on
 * builds holds the memory evaluation needed for the whole wait.
 *
 * Words that returned frames left on the calling thread's stack, below
 * the caller's frame, are cleared first, so they keep nothing alive. Words
 * in live frames, in registers, or on other threads' stacks still do.
 * Uses 64 KiB of the calling thread's stack.
 */
void releaseUnusedGCMemory();

/**
 * What the collector holds free but still mapped, and what it has unmapped
 * (returned to the operating system), in bytes.
 */
struct GCMemoryCounts
{
    size_t free;
    size_t unmapped;

    bool operator==(const GCMemoryCounts &) const = default;
};

/**
 * Call `collect`, which runs one collection and returns the counts after
 * it, until two calls in a row leave the counts as they were, or `limit`
 * calls. Returns the number of calls.
 *
 * One unchanged call is not enough: a block freed by one collection is
 * unmapped only by the one after the next, so the collection between them
 * changes nothing.
 */
unsigned collectUntilStable(fun<GCMemoryCounts()> collect, unsigned limit = 16);

#if NIX_USE_BOEHMGC
/**
 * The number of GC cycles since initGC().
 */
size_t getGCCycles();
#endif

} // namespace nix
