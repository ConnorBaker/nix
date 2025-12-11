#pragma once
///@file
/// GHC Runtime System integration for Nix garbage collection.
///
/// This module provides an alternative GC backend using GHC's allocator
/// and runtime system instead of Boehm GC. It uses FFI to interact with
/// Haskell code that manages GHC-allocated memory.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>  // for std::function

// For `NIX_USE_GHC_GC`
#include "nix/expr/config.hh"

namespace nix {
    // Forward declaration
    struct Value;
}

namespace nix::ghc {

/**
 * GC performance statistics.
 * Tracks nursery and full GC cycles, pause times, and promotion activity.
 */
struct GCStats {
    // Nursery GC statistics
    size_t nurseryGCCount;           // Number of nursery (gen0-only) GC cycles
    double nurseryGCTotalTimeMs;     // Total time spent in nursery GC (milliseconds)
    double nurseryGCAvgTimeMs;       // Average nursery GC pause time

    // Full GC statistics
    size_t fullGCCount;              // Number of full (all-generation) GC cycles
    double fullGCTotalTimeMs;        // Total time spent in full GC (milliseconds)
    double fullGCAvgTimeMs;          // Average full GC pause time

    // Overall statistics
    size_t totalGCCount;             // Total GC cycles (nursery + full)
    double totalGCTimeMs;            // Total time in GC

    // Promotion statistics
    size_t gen0ToGen1Promotions;     // Objects promoted from gen0 to gen1
    size_t gen1ToGen2Promotions;     // Objects promoted from gen1 to gen2

    // Remembered set statistics
    size_t rememberedSetSize;        // Current size of remembered set
    size_t rememberedSetMaxSize;     // Peak remembered set size

    // Memory statistics
    size_t gen0AllocBytes;           // Bytes allocated in gen0 since last GC
    size_t totalAllocBytes;          // Total bytes allocated since last GC

    // Iteration 67: Additional GHC RTS statistics
    size_t majorGCCount;             // Number of major GCs
    size_t maxLiveBytes;             // Peak live bytes (maximum residency)
    size_t maxMemInUseBytes;         // Maximum heap size ever allocated
    size_t gcCpuNs;                  // Cumulative GC CPU time (nanoseconds)
    size_t gcElapsedNs;              // Cumulative GC elapsed time (nanoseconds)
    size_t copiedBytes;              // Bytes copied during last GC
    size_t parMaxCopiedBytes;        // Max bytes copied by any parallel GC thread (work balance)
    size_t generations;              // Number of GC generations
};

} // namespace nix::ghc

#if NIX_USE_GHC_GC

namespace nix::ghc {

/**
 * Initialize the GHC runtime system.
 * Must be called before any GHC-managed allocations.
 * Thread-safe: only initializes once even if called multiple times.
 *
 * @param argc Pointer to argc from main() (GHC may modify it)
 * @param argv Pointer to argv from main() (GHC may modify it)
 * @return true if initialization succeeded, false otherwise
 */
bool initGHCRuntime(int* argc, char*** argv);

/**
 * Shutdown the GHC runtime system.
 * Should be called during program termination.
 * After this call, no GHC-managed memory may be accessed.
 */
void shutdownGHCRuntime();

/**
 * Check if the GHC runtime has been initialized.
 */
bool isGHCRuntimeInitialized();

/**
 * Opaque handle to a GHC StablePtr.
 * This is a reference to a Haskell heap object that prevents GC
 * of the referenced object. Must be explicitly freed.
 */
using StablePtr = void*;

/**
 * Create a StablePtr to keep a Haskell value alive.
 * The value will not be garbage collected until freeStablePtr is called.
 *
 * @param ptr Pointer to the Haskell heap object
 * @return StablePtr handle, or nullptr on failure
 */
StablePtr newStablePtr(void* ptr);

/**
 * Dereference a StablePtr to get the underlying pointer.
 *
 * @param stable The StablePtr to dereference
 * @return Pointer to the Haskell heap object
 */
void* deRefStablePtr(StablePtr stable);

/**
 * Free a StablePtr, allowing the referenced object to be GC'd.
 * After this call, the StablePtr is invalid and must not be used.
 *
 * @param stable The StablePtr to free
 */
void freeStablePtr(StablePtr stable);

/**
 * Allocate memory from the GHC heap.
 * The memory is traced by GHC's garbage collector.
 *
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or nullptr on failure
 */
void* allocBytes(size_t size);

/**
 * Allocate atomic (pointer-free) memory from the GHC heap.
 * The memory is not scanned for pointers during GC.
 * Use for strings and other data that contains no heap pointers.
 *
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory, or nullptr on failure
 */
void* allocBytesAtomic(size_t size);

/**
 * Batch allocate objects and return as linked list.
 * Like GC_malloc_many, returns a linked list where the first word
 * of each object points to the next object in the list.
 *
 * @param objectSize Size of each object in bytes
 * @return Head of linked list of allocated objects
 */
void* allocMany(size_t objectSize);

/**
 * Get next pointer from batch allocation linked list.
 * Inline for performance - called millions of times.
 *
 * @param p Pointer to current object in list
 * @return Pointer to next object, or nullptr if end of list
 */
[[gnu::always_inline]]
inline void* getNext(void* p)
{
    if (!p) return nullptr;
    return *reinterpret_cast<void**>(p);
}

/**
 * Set next pointer in batch allocation linked list.
 * Inline for performance - called millions of times.
 *
 * @param p Pointer to current object
 * @param next Pointer to next object
 */
[[gnu::always_inline]]
inline void setNext(void* p, void* next)
{
    if (p) {
        *reinterpret_cast<void**>(p) = next;
    }
}

/**
 * Trigger a GHC garbage collection cycle.
 * Normally not needed; GHC runs GC automatically.
 */
void performGC();

/**
 * Get the number of GC cycles since GHC RTS initialization.
 */
size_t getGCCycles();

/**
 * Get the current heap size in bytes.
 */
size_t getHeapSize();

/**
 * Get the number of bytes currently allocated.
 */
size_t getAllocatedBytes();

/**
 * Get total allocation count (traced + atomic).
 * Useful for understanding allocation patterns.
 */
size_t getAllocCount();

/**
 * Get allocation count for traced (pointer-containing) allocations.
 */
size_t getTracedAllocCount();

/**
 * Get bytes allocated for traced (pointer-containing) allocations.
 */
size_t getTracedAllocBytes();

/**
 * Get allocation count for atomic (pointer-free) allocations.
 */
size_t getAtomicAllocCount();

/**
 * Get bytes allocated for atomic (pointer-free) allocations.
 */
size_t getAtomicAllocBytes();

// ============================================================================
// Value-specific allocation (Phase 3 preparation)
// ============================================================================

/**
 * Allocate a Value on the GHC-managed heap.
 * This is the GHC equivalent of EvalMemory::allocValue().
 *
 * @return Pointer to zeroed memory for a Value (16 bytes)
 */
void* allocValue();

/**
 * Get the number of Values allocated through the GHC allocator.
 */
size_t getValueAllocCount();

/**
 * Get the total bytes allocated for Values.
 */
size_t getValueAllocBytes();

/**
 * Register a Value as a GC root.
 * In stub mode, this is a no-op.
 * In full GHC mode, this creates a StablePtr to keep the Value alive.
 *
 * @param value Pointer to the Value
 * @return Handle that must be passed to unregisterValueRoot() when done
 */
void* registerValueRoot(void* value);

/**
 * Unregister a Value from being a GC root.
 *
 * @param handle The handle returned by registerValueRoot()
 */
void unregisterValueRoot(void* handle);

// ============================================================================
// Env-specific allocation (Phase 3 preparation)
// ============================================================================

/**
 * Allocate an Env on the GHC-managed heap.
 * Env is a variable-size structure: sizeof(Env) + numSlots * sizeof(Value*)
 *
 * @param numSlots Number of Value* slots in the environment
 * @return Pointer to zeroed memory for an Env
 */
void* allocEnv(size_t numSlots);

/**
 * Get the number of Envs allocated through the GHC allocator.
 */
size_t getEnvAllocCount();

/**
 * Get the total bytes allocated for Envs.
 */
size_t getEnvAllocBytes();

// ============================================================================
// Bindings-specific allocation (Phase 3 preparation)
// ============================================================================

/**
 * Allocate a Bindings (attribute set) on the GHC-managed heap.
 * Bindings is a variable-size structure: sizeof(Bindings) + capacity * sizeof(Attr)
 * sizeof(Bindings) = 24, sizeof(Attr) = 16 on 64-bit
 *
 * @param capacity Number of Attr slots in the bindings
 * @return Pointer to zeroed memory for a Bindings
 */
void* allocBindings(size_t capacity);

/**
 * Get the number of Bindings allocated through the GHC allocator.
 */
size_t getBindingsAllocCount();

/**
 * Get the total bytes allocated for Bindings.
 */
size_t getBindingsAllocBytes();

// ============================================================================
// List-specific allocation (Phase 3 preparation)
// ============================================================================

/**
 * Allocate a list element array on the GHC-managed heap.
 * Used for lists with more than 2 elements.
 * Size is numElems * sizeof(Value*).
 *
 * @param numElems Number of elements in the list
 * @return Pointer to zeroed memory for list elements
 */
void* allocList(size_t numElems);

/**
 * Get the number of list arrays allocated through the GHC allocator.
 */
size_t getListAllocCount();

/**
 * Get the total bytes allocated for list arrays.
 */
size_t getListAllocBytes();

/**
 * Register the current thread with the GHC runtime.
 * Required for any thread that will call into Haskell code.
 * Must be called from the thread to be registered.
 *
 * @return true if registration succeeded
 */
bool registerThread();

/**
 * Unregister the current thread from the GHC runtime.
 * Should be called before the thread exits.
 */
void unregisterThread();

/**
 * Acquire a GHC capability (scheduler token) for the current thread.
 * Required before calling into Haskell from a non-Haskell thread.
 * Must be paired with releaseCapability().
 */
void acquireCapability();

/**
 * Release a GHC capability previously acquired with acquireCapability().
 */
void releaseCapability();

/**
 * RAII wrapper for capability acquisition.
 */
class CapabilityGuard {
public:
    CapabilityGuard() { acquireCapability(); }
    ~CapabilityGuard() { releaseCapability(); }
    CapabilityGuard(const CapabilityGuard&) = delete;
    CapabilityGuard& operator=(const CapabilityGuard&) = delete;
};

/**
 * RAII wrapper for thread registration.
 */
class ThreadRegistration {
public:
    ThreadRegistration() : registered(registerThread()) {}
    ~ThreadRegistration() { if (registered) unregisterThread(); }
    ThreadRegistration(const ThreadRegistration&) = delete;
    ThreadRegistration& operator=(const ThreadRegistration&) = delete;
    bool isRegistered() const { return registered; }
private:
    bool registered;
};

/**
 * Convert a C++ exception to a format that can be stored in a Haskell
 * exception wrapper and rethrown later.
 *
 * @param ex The exception_ptr to convert
 * @return Opaque handle to the exception, or nullptr if conversion failed
 */
void* wrapCppException(std::exception_ptr ex);

/**
 * Debug statistics for performance analysis.
 */
struct DebugStats {
    size_t allocManyCount;
    size_t allocValueCount;
    size_t mmapCount;
};

/**
 * Get debug statistics for allocation analysis.
 */
DebugStats getDebugStats();

// ============================================================================
// Iteration 60: GC Profiling Statistics (Priority 2.1)
// ============================================================================

/**
 * Get current GC statistics.
 * Returns comprehensive performance metrics for analysis and tuning.
 */
GCStats getGCStats();

/**
 * Reset GC statistics.
 * Useful for benchmarking specific code sections.
 */
void resetGCStatsCounters();

/**
 * Print GC statistics to stderr.
 * Called when NIX_GHC_GC_STATS=1 at program exit or on demand.
 */
void printGCStats();

/**
 * Rethrow a C++ exception that was previously wrapped.
 *
 * @param wrapped The wrapped exception handle
 * @throws The original C++ exception
 */
[[noreturn]] void rethrowCppException(void* wrapped);

// ============================================================================
// Iteration 24: Mark-Sweep GC API
// ============================================================================

/**
 * Add a pointer as a GC root.
 * Roots are the starting points for the mark phase.
 */
void gcAddRoot(void* ptr);

/**
 * Remove a pointer from the GC root set.
 */
void gcRemoveRoot(void* ptr);

/**
 * Clear all GC roots.
 */
void gcClearRoots();

/**
 * Begin the mark phase by clearing the marked set.
 * @return The number of roots to iterate over.
 */
size_t gcBeginMark();

/**
 * Mark a pointer as reachable during GC.
 * @param ptr The pointer to mark.
 * @return 0 if newly marked, 1 if already marked, -1 if not a tracked allocation.
 */
int gcMark(void* ptr);

/**
 * Check if a pointer is marked (without marking it).
 * Used for soft cache clearing.
 * @param ptr The pointer to check.
 * @return 1 if marked, 0 if not marked, -1 if not a tracked allocation.
 */
int gcIsMarked(void* ptr);

/**
 * Sweep phase: free all unmarked allocations.
 * @return The number of allocations freed.
 */
size_t gcSweep();

/**
 * Get the number of GC roots.
 */
size_t gcGetRootCount();

/**
 * Get a GC root by index (for iteration).
 * @param index The index of the root to get.
 * @return The root pointer, or nullptr if out of bounds.
 */
void* gcGetRootAt(size_t index);

/**
 * Trace from all GC roots, marking reachable objects.
 * This is the mark phase of mark-sweep GC.
 */
void gcTraceFromRoots();

/**
 * Run a full garbage collection cycle.
 * Traces from roots and sweeps unmarked allocations.
 * @return The number of allocations freed.
 */
size_t gcCollect();

/**
 * Register a soft cache callback.
 * The callback will be invoked during GC (after mark, before sweep) to clear
 * cache entries pointing to unmarked objects.
 * @param callback A function that clears unmarked cache entries and returns the count.
 */
void registerSoftCacheCallback(std::function<size_t()> callback);

/**
 * Set a callback for tracing fileEvalCache during GC.
 * This callback is invoked during the mark phase to trace all cached Values.
 * Unlike soft cache clearing, this keeps the cache intact and marks all entries as live.
 * @param callback A function that traces all cached Values
 */
void setFileCacheTracingCallback(std::function<void()> callback);

/**
 * Trace from a Value during GC mark phase.
 * This recursively traces all Values reachable from the given Value.
 * @param value The Value to trace from
 */
void gcTraceFromValue(nix::Value* value);

/**
 * Clear all registered soft cache callbacks.
 * Should be called when shutting down.
 */
void clearSoftCacheCallbacks();

// ============================================================================
// Phase 4: Env Preservation API (Task 4.3, Iteration 47)
// ============================================================================

/**
 * Preserve an Env chain after thunk forcing.
 * This prevents GC from freeing the Env even though the forced thunk
 * no longer references it directly. Used for cached thunks where multiple
 * imports may share the same Env chain.
 *
 * @param thunkValue Pointer to the forced thunk Value
 * @param env Pointer to the Env to preserve
 */
void gcPreserveEnv(void* thunkValue, void* env);

/**
 * Stop preserving an Env chain (called when cached value is evicted).
 * Decrements the reference count for the Env and allows it to be GC'd
 * if no other thunks reference it.
 *
 * @param thunkValue Pointer to the thunk Value
 */
void gcUnpreserveEnv(void* thunkValue);

// ============================================================================
// Iteration 58: Write Barrier API
// ============================================================================

/**
 * Write barrier: Record when an old object gets a reference to a young object.
 * Must be called whenever an old-generation object (gen1/gen2) is modified
 * to reference a young-generation object (gen0).
 *
 * This is critical for correctness of partial GC. Without write barriers,
 * nursery GC could incorrectly free gen0 objects that are referenced from
 * gen1/gen2 objects.
 *
 * @param oldObject Pointer to the object being modified
 * @param youngObject Pointer to the new reference being stored
 */
void gcWriteBarrier(void* oldObject, void* youngObject);

/**
 * Conservative write barrier: Mark an object as potentially containing young references.
 * Use this when an object is mutated but you don't know the specific young objects.
 *
 * This is more conservative than gcWriteBarrier - it records the object if it's old,
 * regardless of whether the new references are actually young.
 *
 * @param object Pointer to the object being modified
 */
void gcRecordMutation(void* object);

// ============================================================================
// Memory Pressure Triggered GC (Iteration 24)
// ============================================================================

/**
 * Set the GC threshold in bytes.
 * When allocations since the last GC exceed this threshold,
 * GC will be automatically triggered.
 */
void setGCThreshold(size_t bytes);

/**
 * Get the current GC threshold in bytes.
 */
size_t getGCThreshold();

/**
 * Enable or disable automatic GC.
 */
void setGCEnabled(bool enabled);

/**
 * Check if automatic GC is enabled.
 */
bool isGCEnabled();

/**
 * Notify the GC system of an allocation.
 * If allocations exceed the threshold, GC will be triggered.
 * @param bytes Number of bytes allocated
 */
void notifyAllocation(size_t bytes);

/**
 * Reset GC statistics (bytes since last GC).
 */
void resetGCStats();

/**
 * Get the number of bytes allocated since the last GC.
 */
size_t getBytesSinceLastGC();

// ============================================================================
// Iteration 30: Hybrid Allocator Mode
// ============================================================================

/**
 * Enable or disable tracked allocation mode.
 * When enabled: Allocations go through Haskell FFI (enables GC tracking)
 * When disabled: Allocations use fast mmap pools (better performance)
 *
 * Default: disabled (fast mode). Enable with NIX_GHC_GC_TRACK=1 env var.
 */
void setTrackedAllocation(bool enabled);

/**
 * Check if tracked allocation mode is enabled.
 */
bool isTrackedAllocationEnabled();

/**
 * Enter a GC safe point.
 * GC can only run when at a safe point (unless NIX_GHC_GC_UNSAFE is set).
 * This should be called when no Values are on the C++ stack.
 * Triggers pending GC if threshold was exceeded.
 */
void enterSafePoint();

/**
 * Leave a GC safe point.
 */
void leaveSafePoint();

/**
 * RAII wrapper for GC safe points.
 */
class GCSafePoint {
public:
    GCSafePoint() { enterSafePoint(); }
    ~GCSafePoint() { leaveSafePoint(); }
    GCSafePoint(const GCSafePoint&) = delete;
    GCSafePoint& operator=(const GCSafePoint&) = delete;
};

} // namespace nix::ghc

#else // !NIX_USE_GHC_GC

// Provide stub declarations when GHC GC is not enabled
namespace nix::ghc {

inline bool initGHCRuntime(int*, char***) { return false; }
inline void shutdownGHCRuntime() {}
inline bool isGHCRuntimeInitialized() { return false; }

// Statistics stubs (all return 0 when GHC GC is disabled)
inline size_t getGCCycles() { return 0; }
inline size_t getHeapSize() { return 0; }
inline size_t getAllocatedBytes() { return 0; }
inline size_t getAllocCount() { return 0; }
inline size_t getTracedAllocCount() { return 0; }
inline size_t getTracedAllocBytes() { return 0; }
inline size_t getAtomicAllocCount() { return 0; }
inline size_t getAtomicAllocBytes() { return 0; }

// Value-specific stubs
inline void* allocValue() { return nullptr; }
inline size_t getValueAllocCount() { return 0; }
inline size_t getValueAllocBytes() { return 0; }
inline void* registerValueRoot(void*) { return nullptr; }
inline void unregisterValueRoot(void*) {}

// Env-specific stubs
inline void* allocEnv(size_t) { return nullptr; }
inline size_t getEnvAllocCount() { return 0; }
inline size_t getEnvAllocBytes() { return 0; }

// Bindings-specific stubs
inline void* allocBindings(size_t) { return nullptr; }
inline size_t getBindingsAllocCount() { return 0; }
inline size_t getBindingsAllocBytes() { return 0; }

// List-specific stubs
inline void* allocList(size_t) { return nullptr; }
inline size_t getListAllocCount() { return 0; }
inline size_t getListAllocBytes() { return 0; }

// Mark-Sweep GC stubs (Iteration 24)
inline void gcAddRoot(void*) {}
inline void gcRemoveRoot(void*) {}
inline void gcClearRoots() {}
inline size_t gcBeginMark() { return 0; }
inline int gcMark(void*) { return -1; }
inline size_t gcSweep() { return 0; }
inline size_t gcGetRootCount() { return 0; }
inline void* gcGetRootAt(size_t) { return nullptr; }
inline void gcTraceFromRoots() {}
inline size_t gcCollect() { return 0; }

// Phase 4: Env preservation stubs (Iteration 47)
inline void gcPreserveEnv(void*, void*) {}
inline void gcUnpreserveEnv(void*) {}

// Iteration 58: Write barrier stubs
inline void gcWriteBarrier(void*, void*) {}
inline void gcRecordMutation(void*) {}

// Iteration 60: GC stats stubs
inline GCStats getGCStats() { return GCStats{}; }
inline void resetGCStatsCounters() {}
inline void printGCStats() {}

// Memory pressure GC stubs
inline void setGCThreshold(size_t) {}
inline size_t getGCThreshold() { return 0; }
inline void setGCEnabled(bool) {}
inline bool isGCEnabled() { return false; }
inline void notifyAllocation(size_t) {}
inline void resetGCStats() {}
inline size_t getBytesSinceLastGC() { return 0; }
inline void enterSafePoint() {}
inline void leaveSafePoint() {}

// Iteration 30: Hybrid allocator mode stubs
inline void setTrackedAllocation(bool) {}
inline bool isTrackedAllocationEnabled() { return false; }

class GCSafePoint {
public:
    GCSafePoint() {}
    ~GCSafePoint() {}
    GCSafePoint(const GCSafePoint&) = delete;
    GCSafePoint& operator=(const GCSafePoint&) = delete;
};

} // namespace nix::ghc

#endif // NIX_USE_GHC_GC
