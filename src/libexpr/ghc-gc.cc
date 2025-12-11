#include "nix/expr/ghc-gc.hh"
#include "nix/util/error.hh"

#if NIX_USE_GHC_GC

#include <array>      // for std::array (concurrent mark set sharding)
#include <atomic>
#include <chrono>     // for std::chrono (GC profiling)
#include <deque>      // for std::deque (incremental marking work list)
#include <future>     // for std::async (parallel tracing)
#include <mutex>
#include <unordered_set> // for std::unordered_set
#include <cstdlib>    // for malloc/free/calloc
#include <exception>  // for std::rethrow_exception
#include <dlfcn.h>    // for dlopen/dlsym/dlclose
#include <functional> // for std::function
#include <vector>     // for std::vector

// Platform-specific stack scanning
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <pthread.h>
#endif

// Iteration 24: Mark phase tracing requires access to Value, Env, Bindings
#include "nix/expr/value.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/eval.hh"

// ============================================================================
// Iteration 19: Lazy Loading of GHC Libraries
// ============================================================================
//
// The GHC libraries (libHSbase ~12MB, libHSghc-prim ~4.8MB, libHSrts ~837KB)
// are not needed for allocations (which use pure C calloc), but were causing
// an 8x startup slowdown due to dynamic library loading overhead.
//
// This implementation uses dlopen() to load GHC libraries lazily - only when
// initGHCRuntime() is explicitly called. Allocations work without GHC.
//
// The allocation functions (allocValue, allocEnv, allocBindings, allocList)
// all use std::calloc() and don't require GHC at all.
// ============================================================================

// ============================================================================
// Dynamic Library Loading Infrastructure
// ============================================================================

namespace {

// Handle for libghcalloc.so (our Haskell FFI module)
static void* ghcAllocHandle = nullptr;

// Handle for GHC RTS library
static void* ghcRtsHandle = nullptr;

// Function pointer types for GHC RTS functions
// RtsOptsEnabled enum values: RtsOptsNone=0, RtsOptsSafeOnly=1, RtsOptsAll=2
using hs_init_ghc_t = void (*)(int*, char***, void*);
using hs_init_t = void (*)(int*, char***);
using hs_init_with_rtsopts_t = void (*)(int*, char***);

// Iteration 66: RtsConfig structure for hs_init_ghc()
// This enables full RTS option support
struct RtsConfig {
    int rts_opts_enabled;  // 0=None, 1=SafeOnly, 2=All
    const char* rts_opts;  // RTS options string (or nullptr)
    bool rts_hs_main;      // Whether this is hs_main
};
using hs_exit_t = void (*)();
using hs_perform_gc_t = void (*)();

// Function pointer types for libghcalloc.so FFI functions
using ghc_alloc_bytes_t = void* (*)(size_t);
using ghc_alloc_value_t = void* (*)();
using ghc_alloc_env_t = void* (*)(size_t);
using ghc_alloc_bindings_t = void* (*)(size_t);
using ghc_alloc_list_t = void* (*)(size_t);
using ghc_alloc_many_t = void* (*)(size_t, size_t);  // Batch allocation
using ghc_free_t = void (*)(void*);  // Iteration 23: Explicit deallocation
using ghc_new_stable_ptr_t = void* (*)(void*);
using ghc_deref_stable_ptr_t = void* (*)(void*);
using ghc_free_stable_ptr_t = void (*)(void*);
using ghc_register_value_root_t = void* (*)(void*);
using ghc_unregister_value_root_t = void (*)(void*);
using ghc_perform_gc_t = void (*)();
using ghc_get_stat_t = size_t (*)();

// Resolved function pointers (null until libraries loaded)
static hs_init_ghc_t fn_hs_init_ghc = nullptr;
static hs_init_t fn_hs_init = nullptr;
static hs_init_with_rtsopts_t fn_hs_init_with_rtsopts = nullptr;
static hs_exit_t fn_hs_exit = nullptr;

// libghcalloc.so function pointers - allocation
static ghc_alloc_bytes_t fn_alloc_bytes = nullptr;
static ghc_alloc_bytes_t fn_alloc_bytes_atomic = nullptr;
static ghc_alloc_value_t fn_alloc_value = nullptr;
static ghc_alloc_env_t fn_alloc_env = nullptr;
static ghc_alloc_bindings_t fn_alloc_bindings = nullptr;
static ghc_alloc_list_t fn_alloc_list = nullptr;
static ghc_alloc_many_t fn_alloc_many = nullptr;  // Batch allocation
static ghc_free_t fn_free = nullptr;  // Iteration 23

// libghcalloc.so function pointers - StablePtr management
static ghc_new_stable_ptr_t fn_new_stable_ptr = nullptr;
static ghc_deref_stable_ptr_t fn_deref_stable_ptr = nullptr;
static ghc_free_stable_ptr_t fn_free_stable_ptr = nullptr;
static ghc_register_value_root_t fn_register_value_root = nullptr;
static ghc_unregister_value_root_t fn_unregister_value_root = nullptr;
static ghc_perform_gc_t fn_perform_gc = nullptr;
static ghc_get_stat_t fn_get_gc_cycles = nullptr;
static ghc_get_stat_t fn_get_heap_size = nullptr;
static ghc_get_stat_t fn_get_allocated_bytes = nullptr;
static ghc_get_stat_t fn_get_alloc_count = nullptr;
static ghc_get_stat_t fn_get_traced_alloc_count = nullptr;
static ghc_get_stat_t fn_get_traced_alloc_bytes = nullptr;
static ghc_get_stat_t fn_get_atomic_alloc_count = nullptr;
static ghc_get_stat_t fn_get_atomic_alloc_bytes = nullptr;
static ghc_get_stat_t fn_get_value_alloc_count = nullptr;
static ghc_get_stat_t fn_get_value_alloc_bytes = nullptr;
static ghc_get_stat_t fn_get_env_alloc_count = nullptr;
static ghc_get_stat_t fn_get_env_alloc_bytes = nullptr;
static ghc_get_stat_t fn_get_bindings_alloc_count = nullptr;
static ghc_get_stat_t fn_get_bindings_alloc_bytes = nullptr;
static ghc_get_stat_t fn_get_list_alloc_count = nullptr;
static ghc_get_stat_t fn_get_list_alloc_bytes = nullptr;
static ghc_get_stat_t fn_get_live_alloc_count = nullptr;  // Iteration 23
static ghc_get_stat_t fn_get_live_alloc_bytes = nullptr;  // Iteration 23

// Iteration 67: Additional GC statistics
static ghc_get_stat_t fn_get_major_gcs = nullptr;
static ghc_get_stat_t fn_get_max_live_bytes = nullptr;
static ghc_get_stat_t fn_get_max_mem_in_use_bytes = nullptr;
static ghc_get_stat_t fn_get_gc_cpu_ns = nullptr;
static ghc_get_stat_t fn_get_gc_elapsed_ns = nullptr;
static ghc_get_stat_t fn_get_copied_bytes = nullptr;
static ghc_get_stat_t fn_get_par_max_copied_bytes = nullptr;
static ghc_get_stat_t fn_get_generations = nullptr;

// Iteration 24: Mark-Sweep GC function pointers
using ghc_gc_add_root_t = void (*)(void*);
using ghc_gc_remove_root_t = void (*)(void*);
using ghc_gc_clear_roots_t = void (*)();
using ghc_gc_begin_mark_t = size_t (*)();
using ghc_gc_mark_t = int (*)(void*);
using ghc_gc_is_marked_t = int (*)(void*);  // Check if marked (for soft cache)
using ghc_gc_sweep_t = size_t (*)();
using ghc_get_alloc_size_t = size_t (*)(void*);  // Get size of an allocation
using ghc_gc_get_root_at_t = void* (*)(size_t);

static ghc_gc_add_root_t fn_gc_add_root = nullptr;
static ghc_gc_remove_root_t fn_gc_remove_root = nullptr;
static ghc_gc_clear_roots_t fn_gc_clear_roots = nullptr;
static ghc_gc_begin_mark_t fn_gc_begin_mark = nullptr;
static ghc_gc_mark_t fn_gc_mark = nullptr;
static ghc_get_alloc_size_t fn_get_alloc_size = nullptr;
static ghc_gc_is_marked_t fn_gc_is_marked = nullptr;  // Check if marked
static ghc_gc_sweep_t fn_gc_sweep = nullptr;
static ghc_get_stat_t fn_gc_get_root_count = nullptr;
static ghc_gc_get_root_at_t fn_gc_get_root_at = nullptr;

// Helper to load a symbol from a library handle
template<typename T>
bool loadSymbol(void* handle, const char* name, T& out) {
    dlerror();  // Clear any existing error
    void* sym = dlsym(handle, name);
    const char* error = dlerror();
    if (error) {
        return false;
    }
    out = reinterpret_cast<T>(sym);
    return true;
}

// Iteration 64: fn_hs_init moved to main declarations above (line 80)

// Load GHC libraries and initialize the RTS.
// The key insight: libghcalloc.so already links to all GHC dependencies (RTS, base, etc.)
// We load it with RTLD_LAZY to get the symbols, then call hs_init before using them.
bool loadGHCLibrariesAndInit() {
    if (ghcAllocHandle) {
        return true;  // Already loaded
    }

    // Step 1: Load libghcalloc.so with RTLD_LAZY
    // This loads all GHC dependencies (RTS, base, ghc-prim) but doesn't resolve symbols yet
    ghcAllocHandle = dlopen("libghcalloc.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!ghcAllocHandle) {
        if (std::getenv("NIX_GHC_GC_DEBUG")) {
            fprintf(stderr, "GHC GC: Failed to load libghcalloc.so: %s\n", dlerror());
        }
        return false;
    }
    if (std::getenv("NIX_GHC_GC_DEBUG")) {
        fprintf(stderr, "GHC GC: Loaded libghcalloc.so (lazy)\n");
    }

    // Step 2: Get hs_init from the now-loaded RTS (via libghcalloc.so's dependencies)
    if (!loadSymbol(ghcAllocHandle, "hs_init", fn_hs_init)) {
        if (std::getenv("NIX_GHC_GC_DEBUG")) {
            fprintf(stderr, "GHC GC: Failed to get hs_init\n");
        }
        dlclose(ghcAllocHandle);
        ghcAllocHandle = nullptr;
        return false;
    }
    if (!loadSymbol(ghcAllocHandle, "hs_exit", fn_hs_exit)) {
        if (std::getenv("NIX_GHC_GC_DEBUG")) {
            fprintf(stderr, "GHC GC: Failed to get hs_exit\n");
        }
        dlclose(ghcAllocHandle);
        ghcAllocHandle = nullptr;
        return false;
    }

    // Step 3: Initialize the RTS
    // Note: To enable GC statistics, set GHCRTS=-T before running
    // e.g., GHCRTS=-T nix eval --expr "..."
    static int default_argc = 1;
    static char* default_argv_storage[] = { const_cast<char*>("nix"), nullptr };
    static char** default_argv = default_argv_storage;

    if (std::getenv("NIX_GHC_GC_DEBUG")) {
        fprintf(stderr, "GHC GC: Calling hs_init...\n");
    }
    fn_hs_init(&default_argc, &default_argv);
    if (std::getenv("NIX_GHC_GC_DEBUG")) {
        fprintf(stderr, "GHC GC: hs_init completed\n");
    }

    ghcRtsHandle = ghcAllocHandle;  // Same handle, RTS was loaded as dependency

    // Load function pointers from libghcalloc.so
    bool ok = true;

    // Allocation functions
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_alloc_bytes", fn_alloc_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_alloc_bytes_atomic", fn_alloc_bytes_atomic);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_alloc_value", fn_alloc_value);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_alloc_env", fn_alloc_env);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_alloc_bindings", fn_alloc_bindings);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_alloc_list", fn_alloc_list);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_free", fn_free);  // Iteration 23

    // StablePtr functions
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_new_stable_ptr", fn_new_stable_ptr);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_deref_stable_ptr", fn_deref_stable_ptr);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_free_stable_ptr", fn_free_stable_ptr);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_register_value_root", fn_register_value_root);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_unregister_value_root", fn_unregister_value_root);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_perform_gc", fn_perform_gc);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_gc_cycles", fn_get_gc_cycles);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_heap_size", fn_get_heap_size);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_allocated_bytes", fn_get_allocated_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_alloc_count", fn_get_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_traced_alloc_count", fn_get_traced_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_traced_alloc_bytes", fn_get_traced_alloc_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_atomic_alloc_count", fn_get_atomic_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_atomic_alloc_bytes", fn_get_atomic_alloc_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_value_alloc_count", fn_get_value_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_value_alloc_bytes", fn_get_value_alloc_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_env_alloc_count", fn_get_env_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_env_alloc_bytes", fn_get_env_alloc_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_bindings_alloc_count", fn_get_bindings_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_bindings_alloc_bytes", fn_get_bindings_alloc_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_list_alloc_count", fn_get_list_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_list_alloc_bytes", fn_get_list_alloc_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_live_alloc_count", fn_get_live_alloc_count);  // Iteration 23
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_live_alloc_bytes", fn_get_live_alloc_bytes);  // Iteration 23

    // Iteration 24: Mark-Sweep GC functions
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_add_root", fn_gc_add_root);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_remove_root", fn_gc_remove_root);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_clear_roots", fn_gc_clear_roots);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_begin_mark", fn_gc_begin_mark);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_mark", fn_gc_mark);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_is_marked", fn_gc_is_marked);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_sweep", fn_gc_sweep);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_alloc_size", fn_get_alloc_size);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_get_root_count", fn_gc_get_root_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_get_root_at", fn_gc_get_root_at);

    if (!ok) {
        if (std::getenv("NIX_GHC_GC_DEBUG")) {
            fprintf(stderr, "GHC GC: Failed to load some function pointers from libghcalloc.so\n");
        }
        dlclose(ghcAllocHandle);
        ghcAllocHandle = nullptr;
        return false;
    }

    if (std::getenv("NIX_GHC_GC_DEBUG")) {
        fprintf(stderr, "GHC GC: All function pointers loaded. fn_alloc_value=%p\n", (void*)fn_alloc_value);
    }

    return true;
}

// Iteration 64: loadGHCLibraries() and unloadGHCLibraries() removed - not needed with GHC RTS

} // anonymous namespace

// Exception handling - still implemented in C++ since it needs std::exception_ptr
extern "C" {
void* nix_ghc_wrap_exception(void* ex_ptr) {
    return ex_ptr;  // Just return the pointer as-is
}

[[noreturn]] void nix_ghc_rethrow_exception(void* wrapped) {
    // Re-throw the original exception
    auto* ex = static_cast<std::exception_ptr*>(wrapped);
    std::rethrow_exception(*ex);
}
}

namespace nix::ghc {

// Thread-safe initialization state
static std::atomic<bool> ghcInitialized{false};
static std::mutex ghcInitMutex;

// Iteration 64: C++ allocation tracking removed - GHC RTS handles this

// Iteration 64: Concurrent mark set removed - GHC RTS handles marking

// Iteration 64: Root set tracking removed - GHC RTS handles this

// ============================================================================
// Phase 4: Env Preservation Strategy (Task 4.2, Iteration 44)
// ============================================================================
// DESIGN: Option A - Preserve Env After Forcing (Recommended approach)
//
// PROBLEM RECAP:
// When a thunk is forced, expr->eval() overwrites the Value with the result,
// losing the Env* reference. If this thunk was cached, the Env chain becomes
// unreachable and GC frees it, causing use-after-free for other thunks.
//
// SOLUTION:
// Maintain a separate registry of Env chains that were used by forced thunks.
// These Envs are kept alive as GC roots until explicitly unrooted.
//
// DATA STRUCTURES:
// 1. forcedThunkEnvs - Map from Value* to Env* for forced cached thunks
//    - Key: Value* (the forced thunk)
//    - Value: Env* (the Env chain to preserve)
//    - Thread-safe with mutex
//
// 2. envRefCount - Reference count for each Env
//    - Tracks how many forced thunks reference this Env
//    - When count reaches 0, Env can be unrooted
//
// LIFECYCLE:
// 1. REGISTER (during thunk forcing):
//    - Before expr->eval(), extract Env* from thunk
//    - After expr->eval(), register Env* in forcedThunkEnvs
//    - Increment envRefCount for this Env
//    - Add Env* to GC roots if not already present
//
// 2. TRACE (during GC marking):
//    - gcTraceFromRoots() traces all Envs in forcedThunkEnvs
//    - This keeps the Env chains alive even though no Value references them
//
// 3. UNROOT (when cached value is evicted):
//    - When fileEvalCache evicts a cached value, decrement envRefCount
//    - If envRefCount reaches 0, remove Env from forcedThunkEnvs
//    - Remove Env from GC roots if no longer needed
//
// INTEGRATION POINTS:
// - ValueStorage::force() (eval-inline.hh) - Register Env after forcing
// - gcTraceFromRoots() - Trace preserved Envs
// - fileEvalCache eviction - Unroot Envs when cache entries removed
//
// ADVANTAGES:
// - Minimal changes to existing code
// - No change to Value structure or ABI
// - Explicit lifecycle management
// - Can be disabled via configuration flag
//
// DISADVANTAGES:
// - Extra memory overhead for forcedThunkEnvs map
// - Requires cache eviction integration for cleanup
// - Envs kept alive longer than strictly necessary
//
// FUTURE OPTIMIZATION:
// - Track which thunks are "cacheable" (from file imports)
// - Only preserve Envs for cacheable thunks
// - Use weak references if platform supports them
// ============================================================================

// Phase 4: Env preservation data structures (Task 4.3, Iteration 45)
// Maps forced thunks to their preserved Env chains
static std::unordered_map<void*, void*> forcedThunkEnvs;  // Value* -> Env*
static std::mutex forcedThunkEnvsMutex;

// Reference counting for preserved Envs
// Tracks how many forced thunks reference each Env
// When count reaches 0, Env can be safely unrooted
static std::unordered_map<void*, size_t> envRefCount;  // Env* -> refcount
static std::mutex envRefCountMutex;

// Iteration 64: All custom GC tracking removed - GHC RTS handles this

// ============================================================================
// Phase 4: Env Preservation Helpers (Task 4.3, Iteration 46)
// ============================================================================

// Register an Env for preservation after thunk forcing
// This prevents GC from freeing the Env chain even though the forced thunk
// no longer references it directly
// Thread-safe: Uses mutexes for both forcedThunkEnvs and envRefCount
void gcPreserveEnv(void* thunkValue, void* env) {
    if (!thunkValue || !env) return;

    // Register the Env in forcedThunkEnvs
    {
        std::lock_guard<std::mutex> lock(forcedThunkEnvsMutex);
        forcedThunkEnvs[thunkValue] = env;
    }

    // Increment reference count for this Env
    {
        std::lock_guard<std::mutex> lock(envRefCountMutex);
        envRefCount[env]++;
    }

    // Note: Env will be traced during GC via gcTraceFromRoots()
}

// Unregister an Env from preservation (called when cached value is evicted)
// Decrements reference count and removes Env from registry when count reaches 0
// Thread-safe: Uses mutexes for both forcedThunkEnvs and envRefCount
void gcUnpreserveEnv(void* thunkValue) {
    if (!thunkValue) return;

    void* env = nullptr;

    // Remove from forcedThunkEnvs and get the Env pointer
    {
        std::lock_guard<std::mutex> lock(forcedThunkEnvsMutex);
        auto it = forcedThunkEnvs.find(thunkValue);
        if (it == forcedThunkEnvs.end()) {
            return;  // Not registered
        }
        env = it->second;
        forcedThunkEnvs.erase(it);
    }

    // Decrement reference count
    {
        std::lock_guard<std::mutex> lock(envRefCountMutex);
        auto it = envRefCount.find(env);
        if (it != envRefCount.end()) {
            it->second--;
            if (it->second == 0) {
                // No more references - remove from refcount map
                // Env is now eligible for GC if not reachable otherwise
                envRefCount.erase(it);
            }
        }
    }
}

// ============================================================================
// Iteration 64: Custom C++ GC removed - GHC RTS handles all GC
// ============================================================================
// The following functions were removed:
// - cppTrackAllocation() - GHC tracks allocations
// - cppGcMark() - GHC handles marking
// - cppGcSweep() - GHC handles sweeping
// - Remembered set management - GHC handles generational GC
// - Write barrier helpers - GHC provides write barriers
// ============================================================================

// Write barrier stubs (Iteration 64: GHC RTS provides write barriers)
void gcWriteBarrier(void* /* oldObject */, void* /* youngObject */) {
    // GHC RTS handles write barriers internally - no-op
}

void gcRecordMutation(void* /* object */) {
    // GHC RTS handles mutation tracking internally - no-op
}

bool initGHCRuntime(int* argc, char*** argv)
{
    std::lock_guard<std::mutex> lock(ghcInitMutex);

    if (ghcInitialized.load(std::memory_order_relaxed)) {
        return true;  // Already initialized
    }

    // ========================================================================
    // ITERATION 64: Full GHC RTS Integration
    // ========================================================================
    // Use GHC's battle-tested garbage collector instead of custom implementation.
    // Load libghcalloc.so and initialize GHC's RTS with proper flags.
    // ========================================================================

    if (std::getenv("NIX_GHC_GC_DEBUG")) {
        fprintf(stderr, "GHC GC: Initializing GHC RTS\n");
    }

    // Step 1: Load libghcalloc.so (which will automatically load the GHC RTS as a dependency)
    const char* ghcallocPath = std::getenv("NIX_LIBGHCALLOC_PATH");
    if (!ghcallocPath) {
        // Try common locations
        const char* paths[] = {
            "libghcalloc.so",
            "./libghcalloc.so",
            "/usr/local/lib/libghcalloc.so",
            "/usr/lib/libghcalloc.so",
            "../src/libexpr/ghc-alloc/dist/libghcalloc.so",
            "src/libexpr/ghc-alloc/dist/libghcalloc.so",
            nullptr
        };

        for (int i = 0; paths[i]; i++) {
            ghcAllocHandle = dlopen(paths[i], RTLD_LAZY | RTLD_GLOBAL);
            if (ghcAllocHandle) {
                if (std::getenv("NIX_GHC_GC_DEBUG")) {
                    fprintf(stderr, "GHC GC: Loaded %s\n", paths[i]);
                }
                break;
            }
        }
    } else {
        ghcAllocHandle = dlopen(ghcallocPath, RTLD_LAZY | RTLD_GLOBAL);
        if (ghcAllocHandle && std::getenv("NIX_GHC_GC_DEBUG")) {
            fprintf(stderr, "GHC GC: Loaded %s\n", ghcallocPath);
        }
    }

    if (!ghcAllocHandle) {
        if (std::getenv("NIX_GHC_GC_DEBUG")) {
            fprintf(stderr, "GHC GC: Failed to load libghcalloc.so: %s\n", dlerror());
            fprintf(stderr, "GHC GC: Set NIX_LIBGHCALLOC_PATH to specify location\n");
        }
        return false;
    }

    // Step 2: Load hs_init_with_rtsopts and hs_exit from the RTS
    // These are in the GHC RTS libraries that libghcalloc.so depends on,
    // so use RTLD_DEFAULT to search all loaded libraries
    // Using hs_init_with_rtsopts to enable RTS options via GHCRTS environment variable
    fn_hs_init_with_rtsopts = reinterpret_cast<hs_init_with_rtsopts_t>(dlsym(RTLD_DEFAULT, "hs_init_with_rtsopts"));
    if (!fn_hs_init_with_rtsopts) {
        fprintf(stderr, "GHC GC: Failed to load hs_init_with_rtsopts: %s\n", dlerror());
        dlclose(ghcAllocHandle);
        ghcAllocHandle = nullptr;
        return false;
    }
    if (std::getenv("NIX_GHC_GC_DEBUG")) {
        fprintf(stderr, "GHC GC: Using hs_init_with_rtsopts\n");
    }
    fn_hs_exit = reinterpret_cast<void(*)()>(dlsym(RTLD_DEFAULT, "hs_exit"));
    if (!fn_hs_exit) {
        fprintf(stderr, "GHC GC: Failed to load hs_exit: %s\n", dlerror());
        dlclose(ghcAllocHandle);
        ghcAllocHandle = nullptr;
        return false;
    }

    // Step 3: Parse RTS flags from GHCRTS environment variable
    std::vector<char*> rtsArgs;
    rtsArgs.push_back(const_cast<char*>("nix"));  // Program name

    const char* ghcRtsEnv = std::getenv("GHCRTS");
    std::string rtsStr;  // Keep string alive for the duration
    if (ghcRtsEnv) {
        if (std::getenv("NIX_GHC_GC_DEBUG")) {
            fprintf(stderr, "GHC GC: Using RTS flags: %s\n", ghcRtsEnv);
        }
        // Parse GHCRTS flags (space-separated)
        // Must copy to modifiable buffer for strtok
        rtsStr = std::string(ghcRtsEnv);
        char* str = &rtsStr[0];  // Modifiable buffer
        char* token = strtok(str, " ");
        while (token) {
            rtsArgs.push_back(token);
            token = strtok(nullptr, " ");
        }
    } else {
        // Default RTS flags: enable stats, 1GB heap
        if (std::getenv("NIX_GHC_GC_DEBUG")) {
            fprintf(stderr, "GHC GC: Using default RTS flags: -T -H1G\n");
        }
        rtsArgs.push_back(const_cast<char*>("-T"));   // Enable GC stats
        rtsArgs.push_back(const_cast<char*>("-H1G")); // 1GB initial heap
    }

    rtsArgs.push_back(nullptr);  // NULL terminator

    // Step 4: Initialize GHC RTS
    // Iteration 66: Use hs_init_ghc for full RTS control
    int rtsArgc = rtsArgs.size() - 1;
    char** rtsArgv = rtsArgs.data();

    if (std::getenv("NIX_GHC_GC_DEBUG")) {
        fprintf(stderr, "GHC GC: Initializing RTS with %d args\n", rtsArgc);
        for (int i = 0; i < rtsArgc; i++) {
            fprintf(stderr, "  arg[%d]: %s\n", i, rtsArgv[i]);
        }
    }

    // Initialize RTS using hs_init_with_rtsopts
    // This enables RTS options while avoiding the RtsConfig complexity
    fn_hs_init_with_rtsopts(&rtsArgc, &rtsArgv);

    if (std::getenv("NIX_GHC_GC_DEBUG")) {
        fprintf(stderr, "GHC GC: RTS initialized via hs_init_with_rtsopts\n");
    }

    // Step 5: Load all FFI function pointers from libghcalloc.so
    bool ok = true;
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_alloc_bytes", fn_alloc_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_alloc_bytes_atomic", fn_alloc_bytes_atomic);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_alloc_value", fn_alloc_value);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_alloc_env", fn_alloc_env);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_alloc_bindings", fn_alloc_bindings);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_alloc_list", fn_alloc_list);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_alloc_many", fn_alloc_many);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_free", fn_free);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_new_stable_ptr", fn_new_stable_ptr);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_deref_stable_ptr", fn_deref_stable_ptr);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_free_stable_ptr", fn_free_stable_ptr);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_register_value_root", fn_register_value_root);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_unregister_value_root", fn_unregister_value_root);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_perform_gc", fn_perform_gc);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_gc_cycles", fn_get_gc_cycles);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_heap_size", fn_get_heap_size);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_allocated_bytes", fn_get_allocated_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_alloc_count", fn_get_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_traced_alloc_count", fn_get_traced_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_traced_alloc_bytes", fn_get_traced_alloc_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_atomic_alloc_count", fn_get_atomic_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_atomic_alloc_bytes", fn_get_atomic_alloc_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_value_alloc_count", fn_get_value_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_value_alloc_bytes", fn_get_value_alloc_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_env_alloc_count", fn_get_env_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_env_alloc_bytes", fn_get_env_alloc_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_bindings_alloc_count", fn_get_bindings_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_bindings_alloc_bytes", fn_get_bindings_alloc_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_list_alloc_count", fn_get_list_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_list_alloc_bytes", fn_get_list_alloc_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_live_alloc_count", fn_get_live_alloc_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_live_alloc_bytes", fn_get_live_alloc_bytes);

    // Iteration 67: Load additional GC statistics
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_major_gcs", fn_get_major_gcs);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_max_live_bytes", fn_get_max_live_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_max_mem_in_use_bytes", fn_get_max_mem_in_use_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_gc_cpu_ns", fn_get_gc_cpu_ns);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_gc_elapsed_ns", fn_get_gc_elapsed_ns);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_copied_bytes", fn_get_copied_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_par_max_copied_bytes", fn_get_par_max_copied_bytes);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_generations", fn_get_generations);

    // Mark-sweep functions (compatibility stubs in Haskell)
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_add_root", fn_gc_add_root);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_remove_root", fn_gc_remove_root);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_clear_roots", fn_gc_clear_roots);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_begin_mark", fn_gc_begin_mark);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_mark", fn_gc_mark);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_is_marked", fn_gc_is_marked);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_sweep", fn_gc_sweep);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_get_alloc_size", fn_get_alloc_size);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_get_root_count", fn_gc_get_root_count);
    ok = ok && loadSymbol(ghcAllocHandle, "nix_ghc_gc_get_root_at", fn_gc_get_root_at);

    if (!ok) {
        fprintf(stderr, "GHC GC: Failed to load some FFI functions\n");
        dlclose(ghcAllocHandle);
        ghcAllocHandle = nullptr;
        return false;
    }

    ghcInitialized.store(true, std::memory_order_release);

    if (std::getenv("NIX_GHC_GC_DEBUG")) {
        fprintf(stderr, "GHC GC: Fully initialized with GHC RTS!\n");
    }

    return true;
}

void shutdownGHCRuntime()
{
    if (!ghcInitialized.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lock(ghcInitMutex);

    if (!ghcInitialized.load(std::memory_order_relaxed)) {
        return;
    }

    // Shutdown the GHC RTS
    if (fn_hs_exit) {
        fn_hs_exit();
    }
    ghcInitialized.store(false, std::memory_order_release);

    // Optionally unload libraries (uncomment if needed)
    // unloadGHCLibraries();
    // ghcLibrariesLoaded.store(false, std::memory_order_release);
}

bool isGHCRuntimeInitialized()
{
    return ghcInitialized.load(std::memory_order_acquire);
}

StablePtr newStablePtr(void* ptr)
{
    if (!isGHCRuntimeInitialized() || !fn_new_stable_ptr) {
        return nullptr;
    }
    // Call Haskell to create a StablePtr wrapping the raw pointer.
    // The Haskell side creates a wrapper value and returns a StablePtr to it.
    return fn_new_stable_ptr(ptr);
}

void* deRefStablePtr(StablePtr stable)
{
    if (!stable || !isGHCRuntimeInitialized() || !fn_deref_stable_ptr) {
        return nullptr;
    }
    // Call Haskell to dereference the StablePtr and get the raw pointer back.
    return fn_deref_stable_ptr(stable);
}

void freeStablePtr(StablePtr stable)
{
    if (!stable || !isGHCRuntimeInitialized() || !fn_free_stable_ptr) {
        return;
    }
    // Call Haskell to free the StablePtr.
    // This allows the Haskell wrapper to be GC'd.
    // Note: The underlying raw pointer (C++ memory) is NOT freed.
    fn_free_stable_ptr(stable);
}

// ============================================================================
// Iteration 20: High-Performance Memory Pool Allocator
// ============================================================================
//
// Boehm GC's GC_malloc_many is fast because it returns objects from a
// pre-allocated heap pool - no malloc/calloc calls at all. We emulate this
// by pre-allocating large memory pools using mmap and carving out objects.
//
// Key design:
// - Thread-local memory pools (no locking needed)
// - Large pool chunks (1MB) to minimize mmap syscalls
// - Size-class-based allocation for common sizes (16, 24, 32 bytes)
// ============================================================================

#include <sys/mman.h>

// Pool configuration
// Iteration 64: Pool constants removed - GHC RTS handles all sizing

// Debug statistics (defined early for use in SizeClassPool)
static std::atomic<size_t> allocManyCount{0};
static std::atomic<size_t> allocValueCount{0};
static std::atomic<size_t> mmapCount{0};
// Iteration 63: Track total heap size for GC_MAXIMUM_HEAP_SIZE enforcement
static std::atomic<size_t> totalHeapBytes{0};

// Print stats at exit
static void printStatsAtExit() {
    if (std::getenv("NIX_GHC_GC_STATS")) {
        fprintf(stderr, "\n=== GHC GC Debug Stats ===\n");
        fprintf(stderr, "allocMany calls: %zu\n", allocManyCount.load(std::memory_order_relaxed));
        fprintf(stderr, "allocValue calls: %zu\n", allocValueCount.load(std::memory_order_relaxed));
        fprintf(stderr, "mmap calls: %zu\n", mmapCount.load(std::memory_order_relaxed));
        fprintf(stderr, "===========================\n");
    }
}

static int statsAtExitRegistered = (std::atexit(printStatsAtExit), 0);

// ============================================================================
// Iteration 64: All memory pools removed - GHC RTS handles all allocation
// ============================================================================

void* allocBytes(size_t size)
{
    // Iteration 64: ALWAYS use GHC allocator
    if (!fn_alloc_bytes) {
        throw std::runtime_error("GHC RTS not initialized - call initGHCRuntime() first");
    }

    void* ptr = fn_alloc_bytes(size);
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void* allocBytesAtomic(size_t size)
{
    // Iteration 64: ALWAYS use GHC allocator (atomic variant for non-pointer data)
    if (!fn_alloc_bytes_atomic) {
        throw std::runtime_error("GHC RTS not initialized - call initGHCRuntime() first");
    }

    void* ptr = fn_alloc_bytes_atomic(size);
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

// Batch allocate objects and return as linked list (like GC_malloc_many)
// Returns a linked list where the first word of each object points to the next
void* allocMany(size_t objectSize)
{
    if (!fn_alloc_many) {
        throw std::runtime_error("GHC RTS not initialized - call initGHCRuntime() first");
    }

    // Request batch of objects (128 at a time for optimal performance)
    // This amortizes the FFI call overhead across many allocations
    constexpr size_t BATCH_SIZE = 128;
    void* ptr = fn_alloc_many(objectSize, BATCH_SIZE);
    if (!ptr) {
        throw std::bad_alloc();
    }
    allocManyCount.fetch_add(1, std::memory_order_relaxed);
    return ptr;
}

// Note: getNext() and setNext() are now inline in ghc-gc.hh for performance

void performGC()
{
    if (isGHCRuntimeInitialized() && fn_perform_gc) {
        fn_perform_gc();
    }
}

size_t getGCCycles()
{
    if (!isGHCRuntimeInitialized() || !fn_get_gc_cycles) {
        return 0;
    }
    return fn_get_gc_cycles();
}

size_t getHeapSize()
{
    // Iteration 64: Delegate to GHC RTS
    if (isGHCRuntimeInitialized() && fn_get_heap_size) {
        return fn_get_heap_size();
    }
    return 0;
}

size_t getAllocatedBytes()
{
    if (!isGHCRuntimeInitialized() || !fn_get_allocated_bytes) {
        return 0;
    }
    return fn_get_allocated_bytes();
}

size_t getAllocCount()
{
    if (!isGHCRuntimeInitialized() || !fn_get_alloc_count) {
        return 0;
    }
    return fn_get_alloc_count();
}

size_t getTracedAllocCount()
{
    if (!isGHCRuntimeInitialized() || !fn_get_traced_alloc_count) {
        return 0;
    }
    return fn_get_traced_alloc_count();
}

size_t getTracedAllocBytes()
{
    if (!isGHCRuntimeInitialized() || !fn_get_traced_alloc_bytes) {
        return 0;
    }
    return fn_get_traced_alloc_bytes();
}

size_t getAtomicAllocCount()
{
    if (!isGHCRuntimeInitialized() || !fn_get_atomic_alloc_count) {
        return 0;
    }
    return fn_get_atomic_alloc_count();
}

size_t getAtomicAllocBytes()
{
    if (!isGHCRuntimeInitialized() || !fn_get_atomic_alloc_bytes) {
        return 0;
    }
    return fn_get_atomic_alloc_bytes();
}

// ============================================================================
// Value-specific allocation (Phase 3 preparation)
// Using C allocation for performance (Iteration 16)
// ============================================================================

// Iteration 64: Object size constants removed - GHC RTS handles sizing

void* allocValue()
{
    allocValueCount.fetch_add(1, std::memory_order_relaxed);

    // Iteration 64: ALWAYS use GHC allocator
    if (!fn_alloc_value) {
        throw std::runtime_error("GHC RTS not initialized - call initGHCRuntime() first");
    }

    void* ptr = fn_alloc_value();
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

size_t getValueAllocCount()
{
    if (!isGHCRuntimeInitialized() || !fn_get_value_alloc_count) {
        return 0;
    }
    return fn_get_value_alloc_count();
}

size_t getValueAllocBytes()
{
    if (!isGHCRuntimeInitialized() || !fn_get_value_alloc_bytes) {
        return 0;
    }
    return fn_get_value_alloc_bytes();
}

void* registerValueRoot(void* value)
{
    if (!isGHCRuntimeInitialized() || !value || !fn_register_value_root) {
        return nullptr;
    }
    return fn_register_value_root(value);
}

void unregisterValueRoot(void* handle)
{
    if (!isGHCRuntimeInitialized() || !handle || !fn_unregister_value_root) {
        return;
    }
    fn_unregister_value_root(handle);
}

// ============================================================================
// Env-specific allocation (Phase 3 preparation)
// Using C allocation for performance (Iteration 16)
// ============================================================================

void* allocEnv(size_t numSlots)
{
    // Iteration 64: ALWAYS use GHC allocator
    if (!fn_alloc_env) {
        throw std::runtime_error("GHC RTS not initialized - call initGHCRuntime() first");
    }

    void* ptr = fn_alloc_env(numSlots);
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

size_t getEnvAllocCount()
{
    if (!isGHCRuntimeInitialized() || !fn_get_env_alloc_count) {
        return 0;
    }
    return fn_get_env_alloc_count();
}

size_t getEnvAllocBytes()
{
    if (!isGHCRuntimeInitialized() || !fn_get_env_alloc_bytes) {
        return 0;
    }
    return fn_get_env_alloc_bytes();
}

// ============================================================================
// Bindings-specific allocation (Phase 3 preparation)
// Using C allocation for performance (Iteration 16)
// ============================================================================

void* allocBindings(size_t capacity)
{
    // Iteration 64: ALWAYS use GHC allocator
    if (!fn_alloc_bindings) {
        throw std::runtime_error("GHC RTS not initialized - call initGHCRuntime() first");
    }

    void* ptr = fn_alloc_bindings(capacity);
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

size_t getBindingsAllocCount()
{
    if (!isGHCRuntimeInitialized() || !fn_get_bindings_alloc_count) {
        return 0;
    }
    return fn_get_bindings_alloc_count();
}

size_t getBindingsAllocBytes()
{
    if (!isGHCRuntimeInitialized() || !fn_get_bindings_alloc_bytes) {
        return 0;
    }
    return fn_get_bindings_alloc_bytes();
}

// ============================================================================
// List-specific allocation (Phase 3 preparation)
// Using C allocation for performance (Iteration 16)
// ============================================================================

void* allocList(size_t numElems)
{
    // Iteration 64: ALWAYS use GHC allocator
    if (!fn_alloc_list) {
        throw std::runtime_error("GHC RTS not initialized - call initGHCRuntime() first");
    }

    void* ptr = fn_alloc_list(numElems);
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

size_t getListAllocCount()
{
    if (!isGHCRuntimeInitialized() || !fn_get_list_alloc_count) {
        return 0;
    }
    return fn_get_list_alloc_count();
}

size_t getListAllocBytes()
{
    if (!isGHCRuntimeInitialized() || !fn_get_list_alloc_bytes) {
        return 0;
    }
    return fn_get_list_alloc_bytes();
}

// Thread-local capability state
static thread_local bool threadRegistered = false;
static thread_local bool capabilityHeld = false;
static thread_local void* threadCapability = nullptr;  // Capability* replaced with void*

bool registerThread()
{
    // STUB MODE: No-op, threads don't need registration
    // TODO: Enable hs_thread_done() once full Haskell support is enabled.
    if (!isGHCRuntimeInitialized()) {
        return false;
    }
    threadRegistered = true;
    return true;
}

void unregisterThread()
{
    // STUB MODE: No-op
    // TODO: Enable hs_thread_done() once full Haskell support is enabled.
    if (!threadRegistered) {
        return;
    }
    if (capabilityHeld) {
        releaseCapability();
    }
    threadRegistered = false;
}

void acquireCapability()
{
    // STUB MODE: No-op, capabilities not needed
    // TODO: Enable rts_lock() once full Haskell support is enabled.
    if (!isGHCRuntimeInitialized()) {
        throw Error("GHC runtime not initialized");
    }
    capabilityHeld = true;
}

void releaseCapability()
{
    // STUB MODE: No-op
    // TODO: Enable rts_unlock() once full Haskell support is enabled.
    capabilityHeld = false;
    threadCapability = nullptr;
}

void* wrapCppException(std::exception_ptr ex)
{
    if (!isGHCRuntimeInitialized()) {
        return nullptr;
    }
    // Store the exception_ptr in a heap-allocated location
    // that can be passed to Haskell
    auto* stored = new std::exception_ptr(ex);
    return nix_ghc_wrap_exception(stored);
}

[[noreturn]] void rethrowCppException(void* wrapped)
{
    if (!wrapped) {
        throw Error("null exception wrapper");
    }
    nix_ghc_rethrow_exception(wrapped);
    // Should never reach here
    std::abort();
}

DebugStats getDebugStats()
{
    return DebugStats{
        .allocManyCount = allocManyCount.load(std::memory_order_relaxed),
        .allocValueCount = allocValueCount.load(std::memory_order_relaxed),
        .mmapCount = mmapCount.load(std::memory_order_relaxed),
    };
}

// ============================================================================
// Iteration 24: Mark-Sweep GC API
// ============================================================================
//
// The mark-sweep algorithm allows automatic garbage collection:
// 1. Register GC roots (RootValue, EvalState fields, stack Values)
// 2. Call gcBeginMark() to start a GC cycle
// 3. Trace from roots through Value/Env/Bindings, calling gcMark() for each
// 4. Call gcSweep() to free unmarked allocations
// 5. Call gcClearRoots() to reset for next cycle
//
// The tracing logic is implemented in C++ because it requires understanding
// the layout of Value, Env, and Bindings structures.

void gcAddRoot(void* ptr)
{
    if (!isGHCRuntimeInitialized() || !fn_gc_add_root || !ptr) {
        return;
    }
    fn_gc_add_root(ptr);
}

void gcRemoveRoot(void* ptr)
{
    if (!isGHCRuntimeInitialized() || !fn_gc_remove_root || !ptr) {
        return;
    }
    fn_gc_remove_root(ptr);
}

void gcClearRoots()
{
    if (!isGHCRuntimeInitialized() || !fn_gc_clear_roots) {
        return;
    }
    fn_gc_clear_roots();
}

size_t gcBeginMark()
{
    if (!isGHCRuntimeInitialized() || !fn_gc_begin_mark) {
        return 0;
    }
    return fn_gc_begin_mark();
}

int gcMark(void* ptr)
{
    // Iteration 64: GHC RTS handles marking - no-op
    (void)ptr;  // Suppress unused parameter warning
    return -1;  // Not tracked
}

int gcIsMarked(void* ptr)
{
    if (!isGHCRuntimeInitialized() || !fn_gc_is_marked || !ptr) {
        return -1;
    }
    return fn_gc_is_marked(ptr);
}

size_t gcSweep()
{
    // Iteration 64: GHC RTS handles sweeping - this is a no-op
    return 0;
}

size_t gcGetRootCount()
{
    if (!isGHCRuntimeInitialized() || !fn_gc_get_root_count) {
        return 0;
    }
    return fn_gc_get_root_count();
}

void* gcGetRootAt(size_t index)
{
    if (!isGHCRuntimeInitialized() || !fn_gc_get_root_at) {
        return nullptr;
    }
    return fn_gc_get_root_at(index);
}

size_t gcGetAllocSize(void* ptr)
{
    if (!isGHCRuntimeInitialized() || !fn_get_alloc_size || !ptr) {
        return 0;
    }
    return fn_get_alloc_size(ptr);
}

// ============================================================================
// Iteration 24: Mark Phase Tracing
// ============================================================================
//
// Tracing follows pointers from roots through the object graph:
// - Value: traces contained pointers (Bindings*, Env*, Value* in lists/thunks)
// - Env: traces parent env and all Value* slots
// - Bindings: traces baseLayer and all Attr.value pointers
//
// The mark function returns:
//   0 = newly marked
//   1 = already marked
//  -1 = not a tracked allocation (e.g., static data or non-GHC allocation)

// Forward declarations for mutual recursion
static void traceEnv(Env* env);
static void traceBindings(Bindings* bindings);
static void traceValue(Value* value);

static void traceEnv(Env* env)
{
    if (!env) return;

    // Mark this Env
    int result = gcMark(env);
    if (result != 0) return;  // Already marked or not tracked

    // Trace parent environment
    traceEnv(env->up);

    // Trace values in the env's values array
    // The Env structure is: struct { Env* up; Value* values[]; }
    // Allocation size = sizeof(Env*) + numSlots * sizeof(Value*)
    size_t allocSize = gcGetAllocSize(env);
    if (std::getenv("NIX_GHC_GC_DEBUG") && allocSize == 0) {
        static std::atomic<int> untracked_env_count{0};
        if (untracked_env_count.fetch_add(1) < 5) {
            fprintf(stderr, "GHC GC: Warning - Env %p has no tracked size (not allocated via Haskell allocator)\n", env);
        }
    }
    if (allocSize > sizeof(Env*)) {
        // Calculate number of Value* slots
        size_t numSlots = (allocSize - sizeof(Env*)) / sizeof(Value*);
        for (size_t i = 0; i < numSlots; ++i) {
            // Only trace if the value pointer is non-null
            // traceValue() will handle marking and checking if it's a tracked allocation
            Value* val = env->values[i];
            if (val) {
                traceValue(val);
            }
        }
    }
}

static void traceBindings(Bindings* bindings)
{
    if (!bindings) return;

    // Mark this Bindings
    int result = gcMark(bindings);
    if (result != 0) return;  // Already marked or not tracked

    // Trace base layer (linked list of Bindings layers)
    if (bindings->isLayered()) {
        // Can't directly access baseLayer due to it being private
        // We iterate through the bindings instead
    }

    // Trace all Value* in the attributes
    for (const auto& attr : *bindings) {
        traceValue(attr.value);
    }
}

static void traceValue(Value* value)
{
    if (!value) return;

    // Mark this Value
    int result = gcMark(value);
    if (result != 0) return;  // Already marked or not tracked

    // Trace based on the value's type using public interface
    ValueType vtype = value->type();

    switch (vtype) {
    case nAttrs: {
        // Trace the Bindings
        Bindings* bindings = const_cast<Bindings*>(value->attrs());
        traceBindings(bindings);
        break;
    }

    case nList: {
        // Trace all list elements using listView()
        ListView view = value->listView();
        for (Value* elem : view) {
            traceValue(elem);
        }
        break;
    }

    case nThunk: {
        // Need to handle thunks, apps, primOpApps
        // Check which specific thunk type it is
        if (value->isThunk()) {
            auto thunk = value->thunk();
            traceEnv(thunk.env);
        } else if (value->isApp()) {
            auto app = value->app();
            traceValue(app.left);
            traceValue(app.right);
        } else if (value->isPrimOpApp()) {
            auto primOpApp = value->primOpApp();
            traceValue(primOpApp.left);
            traceValue(primOpApp.right);
        }
        // Blackhole states (tPending, tAwaited) have no pointers to trace
        break;
    }

    case nFunction: {
        // Lambda or PrimOp
        if (value->isLambda()) {
            auto lambda = value->lambda();
            traceEnv(lambda.env);
        }
        // PrimOp has no pointers to trace
        break;
    }

    // Types with no pointers to trace
    case nInt:
    case nBool:
    case nString:
    case nPath:
    case nNull:
    case nFloat:
    case nFailed:
    case nExternal:
        break;
    }
}

// ============================================================================
// Conservative Stack Scanner (for finding strong roots on C++ stack)
// ============================================================================

// Phase 2: Simplified conservative stack scanning
// Get approximate stack bounds using portable frame pointer approach
static bool getStackBounds(void** stackLow, void** stackHigh)
{
    // Use compiler builtin to get current frame pointer
    void* framePtr = __builtin_frame_address(0);

    // Conservative scan: 64KB above current frame
    // Stack typically only a few KB deep during evaluation
    // This is simpler and more portable than platform-specific pthread calls
    *stackLow = framePtr;
    *stackHigh = static_cast<char*>(framePtr) + (64 * 1024);

    return true;
}

// Conservative stack scan: find potential pointers to GC heap on the stack
static void scanStackForRoots()
{
    void* stackLow;
    void* stackHigh;

    if (!getStackBounds(&stackLow, &stackHigh)) {
        if (std::getenv("NIX_GHC_GC_DEBUG")) {
            fprintf(stderr, "GHC GC: WARNING - Could not get stack bounds for scanning\n");
        }
        return;
    }

    // Get current stack pointer (approximate)
    void* currentSP = __builtin_frame_address(0);

    // Iteration 63: Limit stack scan to reasonable size for performance
    // Full stack scan (potentially megabytes) is too slow
    // Only scan recent frames (16KB is enough for deep recursion)
    constexpr size_t MAX_STACK_SCAN_SIZE = 16 * 1024;  // 16KB

    // Determine scan direction (stack grows down on most platforms)
    void* scanStart;
    void* scanEnd;

    if (currentSP < stackHigh) {
        // Stack grows downward (typical)
        scanStart = currentSP;
        scanEnd = static_cast<char*>(currentSP) + MAX_STACK_SCAN_SIZE;
        if (scanEnd > stackHigh) {
            scanEnd = stackHigh;  // Don't exceed stack bounds
        }
    } else {
        // Stack grows upward (unusual)
        scanStart = static_cast<char*>(currentSP) - MAX_STACK_SCAN_SIZE;
        if (scanStart < stackLow) {
            scanStart = stackLow;  // Don't exceed stack bounds
        }
        scanEnd = currentSP;
    }

    if (std::getenv("NIX_GHC_GC_DEBUG")) {
        fprintf(stderr, "GHC GC: Stack scan from %p to %p\n", scanStart, scanEnd);
    }

    // Scan each word on the stack
    uintptr_t* start = static_cast<uintptr_t*>(scanStart);
    uintptr_t* end = static_cast<uintptr_t*>(scanEnd);
    size_t potentialRoots = 0;
    size_t markedRoots = 0;

    for (uintptr_t* p = start; p < end; ++p) {
        uintptr_t word = *p;

        // Check if word looks like a pointer (aligned, reasonable range)
        // Pointers are typically aligned to 8 or 16 bytes
        if ((word & 0x7) != 0) {
            continue;  // Not aligned, skip
        }

        // Check if it's in a plausible heap range
        // (This is conservative - we check all reasonable addresses)
        if (word < 0x1000 || word > 0x7FFFFFFFFFFF) {
            continue;  // NULL or invalid address range
        }

        potentialRoots++;

        // Try to mark it - gcMark returns:
        //   0 = newly marked (valid allocation)
        //   1 = already marked
        //  -1 = not a tracked allocation
        void* potentialPtr = reinterpret_cast<void*>(word);
        int result = gcMark(potentialPtr);

        if (result == 0) {
            // Successfully marked a new root!
            markedRoots++;

            // Trace from this root
            // We don't know if it's a Value*, Env*, or Bindings*, so we try Value* first
            // (most common case). If it's actually an Env* or Bindings*, the tracing
            // will handle it correctly since they all get marked via gcMark().
            traceValue(static_cast<Value*>(potentialPtr));
        }
    }

    if (std::getenv("NIX_GHC_GC_DEBUG")) {
        fprintf(stderr, "GHC GC: Stack scan found %zu potential roots, %zu valid allocations\n",
                potentialRoots, markedRoots);
    }
}

// Custom callback for tracing fileEvalCache
// This is set by EvalState and called during GC to trace cached values
using FileCacheTracingCallback = std::function<void()>;
static FileCacheTracingCallback fileCacheTracingCallback = nullptr;

void setFileCacheTracingCallback(FileCacheTracingCallback callback)
{
    fileCacheTracingCallback = std::move(callback);
}

// Export traceValue for use by fileEvalCache tracing callback
void gcTraceFromValue(Value* value)
{
    if (value) {
        traceValue(value);
    }
}

void gcTraceFromRoots()
{
    if (!isGHCRuntimeInitialized()) return;

    // Begin mark phase (clears marked set)
    size_t rootCount = gcBeginMark();

    if (std::getenv("NIX_GHC_GC_DEBUG")) {
        fprintf(stderr, "GHC GC: Tracing from %zu registered roots\n", rootCount);
    }

    // Phase 1: Conservative stack scan for strong roots
    scanStackForRoots();

    // Phase 2: Trace from explicitly registered roots
    for (size_t i = 0; i < rootCount; ++i) {
        void* root = gcGetRootAt(i);
        if (root) {
            // Roots are Value* pointers
            traceValue(static_cast<Value*>(root));
        }
    }

    // Phase 3: Trace from fileEvalCache
    if (fileCacheTracingCallback) {
        fileCacheTracingCallback();
    }

    // Iteration 64: Remembered set and parallel tracing removed - GHC RTS handles this
    if (std::getenv("NIX_GHC_GC_DEBUG")) {
        fprintf(stderr, "GHC GC: Finished tracing from roots\n");
    }
}

// ============================================================================
// Soft Cache Callback System
// ============================================================================
//
// Soft caches (like fileEvalCache) can register callbacks to clear unmarked
// entries during GC. This allows cached data to be collected when not actively
// referenced.

using SoftCacheCallback = std::function<size_t()>;
static std::vector<SoftCacheCallback> softCacheCallbacks;
static std::mutex softCacheCallbacksMutex;

void registerSoftCacheCallback(SoftCacheCallback callback)
{
    std::lock_guard<std::mutex> lock(softCacheCallbacksMutex);
    softCacheCallbacks.push_back(std::move(callback));
}

void clearSoftCacheCallbacks()
{
    std::lock_guard<std::mutex> lock(softCacheCallbacksMutex);
    softCacheCallbacks.clear();
}

// ============================================================================
// Iteration 58+: Partial GC Heuristics (Priority 1.2)
// ============================================================================

// Iteration 68: All custom GC thresholds and counters removed
// GHC RTS handles all GC decisions and statistics

// ============================================================================
// Iteration 68: GC statistics now provided by GHC RTS
// See getGCStats() which calls GHC's getRTSStats()
// ============================================================================

// Iteration 64: clearSoftCaches() removed - not used with GHC RTS

size_t gcCollect()
{
    // Iteration 64: Delegate to GHC RTS - GHC handles all GC logic
    if (!isGHCRuntimeInitialized()) return 0;

    if (std::getenv("NIX_GHC_GC_DEBUG")) {
        fprintf(stderr, "GHC GC: Triggering GC via GHC RTS\n");
    }

    // Call GHC's garbage collector
    performGC();

    return 0;  // GHC doesn't report freed count
}

// ============================================================================
// Iteration 24: Memory Pressure Triggered GC
// ============================================================================
//
// Automatically run GC when memory usage exceeds a threshold.
// This helps prevent unbounded memory growth during large evaluations.

// Configuration for memory pressure GC
// Default: 1GB - set high to avoid GC during evaluation (stack values not rooted)
// Can be overridden via NIX_GHC_GC_THRESHOLD env var (in bytes)
// Iteration 68: initGCConfig() removed
// NIX_GHC_GC_STATS is handled by printStatsAtExit() registered earlier
// All other GC configuration is done via GHCRTS environment variable

void setGCThreshold(size_t bytes)
{
    // Iteration 64: GHC RTS handles GC thresholds via GHCRTS flags
    // This function is now a no-op but kept for API compatibility
    (void)bytes;  // Suppress unused parameter warning
}

size_t getGCThreshold()
{
    // Iteration 64: GHC RTS handles GC thresholds
    // Return 0 for compatibility
    return 0;
}

void setGCEnabled(bool enabled)
{
    // Iteration 68: GHC RTS handles GC enablement via GHCRTS flags
    // This function is now a no-op but kept for API compatibility
    (void)enabled;  // Suppress unused parameter warning
}

bool isGCEnabled()
{
    // Iteration 68: GHC RTS is always enabled
    return true;
}

void notifyAllocation(size_t bytes)
{
    // Iteration 64: GHC RTS handles allocation tracking and GC triggering
    // This function is now a no-op but kept for API compatibility
    (void)bytes;  // Suppress unused parameter warning
}

void enterSafePoint()
{
    // Iteration 68: GHC RTS handles safe points and GC triggering
    // This function is now a no-op but kept for API compatibility
}

void leaveSafePoint()
{
    // Iteration 68: GHC RTS handles safe points
    // This function is now a no-op but kept for API compatibility
}

void resetGCStats()
{
    // Iteration 64: GHC RTS tracks its own statistics
    // This function is now a no-op but kept for API compatibility
}

// Get bytes allocated since last GC
size_t getBytesSinceLastGC()
{
    // Iteration 64: GHC RTS tracks allocation statistics
    // Return 0 for compatibility
    return 0;
}

// Iteration 68: Tracked allocation removed - GHC RTS always used
void setTrackedAllocation(bool enabled)
{
    // This function is now a no-op but kept for API compatibility
    (void)enabled;  // Suppress unused parameter warning
}

bool isTrackedAllocationEnabled()
{
    // GHC RTS is always used
    return true;
}

// ============================================================================
// Iteration 60: GC Statistics API (Priority 2.1)
// ============================================================================

GCStats getGCStats()
{
    GCStats stats;

    // Iteration 68: All stats come from GHC RTS
    // Custom GC statistics removed - use GHC's native stats instead
    stats.nurseryGCCount = 0;  // Use stats.majorGCCount from GHC instead
    stats.nurseryGCTotalTimeMs = 0.0;
    stats.nurseryGCAvgTimeMs = 0.0;
    stats.fullGCCount = 0;
    stats.fullGCTotalTimeMs = 0.0;
    stats.fullGCAvgTimeMs = 0.0;
    stats.totalGCCount = 0;
    stats.totalGCTimeMs = 0.0;
    stats.gen0ToGen1Promotions = 0;
    stats.gen1ToGen2Promotions = 0;
    stats.rememberedSetSize = 0;
    stats.rememberedSetMaxSize = 0;
    stats.gen0AllocBytes = 0;
    stats.totalAllocBytes = 0;

    // Iteration 68: GHC RTS provides comprehensive statistics
    if (isGHCRuntimeInitialized()) {
        stats.majorGCCount = fn_get_major_gcs ? fn_get_major_gcs() : 0;
        stats.maxLiveBytes = fn_get_max_live_bytes ? fn_get_max_live_bytes() : 0;
        stats.maxMemInUseBytes = fn_get_max_mem_in_use_bytes ? fn_get_max_mem_in_use_bytes() : 0;
        stats.gcCpuNs = fn_get_gc_cpu_ns ? fn_get_gc_cpu_ns() : 0;
        stats.gcElapsedNs = fn_get_gc_elapsed_ns ? fn_get_gc_elapsed_ns() : 0;
        stats.copiedBytes = fn_get_copied_bytes ? fn_get_copied_bytes() : 0;
        stats.parMaxCopiedBytes = fn_get_par_max_copied_bytes ? fn_get_par_max_copied_bytes() : 0;
        stats.generations = fn_get_generations ? fn_get_generations() : 2;
    } else {
        stats.majorGCCount = 0;
        stats.maxLiveBytes = 0;
        stats.maxMemInUseBytes = 0;
        stats.gcCpuNs = 0;
        stats.gcElapsedNs = 0;
        stats.copiedBytes = 0;
        stats.parMaxCopiedBytes = 0;
        stats.generations = 2;
    }

    return stats;
}

void resetGCStatsCounters()
{
    // Iteration 68: GHC RTS manages all statistics
    // This function is now a no-op but kept for API compatibility
}

void printGCStats()
{
    GCStats stats = getGCStats();

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "GHC GC Performance Statistics\n");
    fprintf(stderr, "========================================\n\n");

    fprintf(stderr, "Nursery GC:\n");
    fprintf(stderr, "  Cycles: %zu\n", stats.nurseryGCCount);
    fprintf(stderr, "  Total Time: %.2f ms\n", stats.nurseryGCTotalTimeMs);
    fprintf(stderr, "  Avg Pause Time: %.2f ms\n", stats.nurseryGCAvgTimeMs);
    fprintf(stderr, "\n");

    fprintf(stderr, "Major GC:\n");
    fprintf(stderr, "  Cycles: %zu\n", stats.majorGCCount);
    fprintf(stderr, "\n");

    fprintf(stderr, "Full GC:\n");
    fprintf(stderr, "  Cycles: %zu\n", stats.fullGCCount);
    fprintf(stderr, "  Total Time: %.2f ms\n", stats.fullGCTotalTimeMs);
    fprintf(stderr, "  Avg Pause Time: %.2f ms\n", stats.fullGCAvgTimeMs);
    fprintf(stderr, "\n");

    fprintf(stderr, "Overall:\n");
    fprintf(stderr, "  Total GC Cycles: %zu\n", stats.totalGCCount);
    fprintf(stderr, "  Total GC Time: %.2f ms\n", stats.totalGCTimeMs);
    if (stats.totalGCCount > 0) {
        fprintf(stderr, "  Avg GC Pause: %.2f ms\n", stats.totalGCTimeMs / stats.totalGCCount);
    }
    fprintf(stderr, "\n");

    // Iteration 67: Additional GHC RTS statistics
    fprintf(stderr, "GHC RTS Statistics:\n");
    fprintf(stderr, "  Generations: %zu\n", stats.generations);
    fprintf(stderr, "  Peak Live Bytes: %zu (%.2f MB)\n", stats.maxLiveBytes, stats.maxLiveBytes / (1024.0 * 1024.0));
    fprintf(stderr, "  Max Heap Size: %zu (%.2f MB)\n", stats.maxMemInUseBytes, stats.maxMemInUseBytes / (1024.0 * 1024.0));
    fprintf(stderr, "  Copied Bytes (last GC): %zu\n", stats.copiedBytes);
    if (stats.parMaxCopiedBytes > 0) {
        fprintf(stderr, "  Parallel GC Work Balance: %zu bytes max per thread\n", stats.parMaxCopiedBytes);
    }
    fprintf(stderr, "  GC CPU Time: %.2f ms\n", stats.gcCpuNs / 1000000.0);
    fprintf(stderr, "  GC Elapsed Time: %.2f ms\n", stats.gcElapsedNs / 1000000.0);
    if (stats.gcElapsedNs > 0) {
        double gcEfficiency = (stats.gcCpuNs / (double)stats.gcElapsedNs) * 100.0;
        fprintf(stderr, "  GC Parallelism Efficiency: %.1f%%\n", gcEfficiency);
    }
    fprintf(stderr, "\n");

    fprintf(stderr, "Promotions:\n");
    fprintf(stderr, "  Gen0 -> Gen1: %zu\n", stats.gen0ToGen1Promotions);
    fprintf(stderr, "  Gen1 -> Gen2: %zu\n", stats.gen1ToGen2Promotions);
    fprintf(stderr, "\n");

    fprintf(stderr, "Remembered Set:\n");
    fprintf(stderr, "  Current Size: %zu\n", stats.rememberedSetSize);
    fprintf(stderr, "  Peak Size: %zu\n", stats.rememberedSetMaxSize);
    fprintf(stderr, "\n");

    fprintf(stderr, "Memory (since last GC):\n");
    fprintf(stderr, "  Gen0 Allocated: %zu bytes (%.2f MB)\n",
            stats.gen0AllocBytes, stats.gen0AllocBytes / (1024.0 * 1024.0));
    fprintf(stderr, "  Total Allocated: %zu bytes (%.2f MB)\n",
            stats.totalAllocBytes, stats.totalAllocBytes / (1024.0 * 1024.0));
    fprintf(stderr, "\n========================================\n\n");
}

} // namespace nix::ghc

#endif // NIX_USE_GHC_GC
