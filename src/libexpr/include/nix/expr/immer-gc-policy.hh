#pragma once
///@file
/// Immer persistent data structure configuration for Nix with Boehm GC integration.

#include "nix/expr/eval-gc.hh"

// Enable Boehm GC support in Immer
#if NIX_USE_BOEHMGC
#  define IMMER_HAS_LIBGC 1
#endif

// Silence Immer's use of undefined macros with -Werror=undef
#ifndef IMMER_NO_FREE_LIST
#  define IMMER_NO_FREE_LIST 0
#endif
#ifndef IMMER_NO_THREAD_SAFETY
#  define IMMER_NO_THREAD_SAFETY 0
#endif

#include <immer/heap/gc_heap.hpp>
#include <immer/heap/heap_policy.hpp>
#include <immer/refcount/no_refcount_policy.hpp>
#include <immer/lock/no_lock_policy.hpp>
#include <immer/transience/gc_transience_policy.hpp>
#include <immer/memory_policy.hpp>
#include <immer/flex_vector.hpp>
#include <immer/flex_vector_transient.hpp>

namespace nix {

struct Value;

#if NIX_USE_BOEHMGC

/**
 * Memory policy for Immer containers that uses Boehm GC.
 *
 * This policy:
 * - Uses gc_heap which calls GC_malloc internally
 * - Disables reference counting (GC handles memory reclamation)
 * - Disables locking (GC is thread-safe, and we don't need locks for refcounting)
 * - Uses gc_transience_policy for efficient transient mutations
 *
 * NOTE: We use a patched gc_transience_policy with lazy initialization
 * of the static `noone` sentinel to avoid static initialization order issues
 * with Boehm GC.
 */
using gc_memory_policy = immer::memory_policy<
    immer::heap_policy<immer::gc_heap>,  // Use GC_malloc for allocations
    immer::no_refcount_policy,            // GC handles deallocation, no refcounting needed
    immer::no_lock_policy,                // No locks needed without refcounting
    immer::gc_transience_policy,          // Transience tracking for efficient batch ops
    false,                                // prefer_fewer_bigger_objects
    false                                 // use_transient_rvalues (not useful with no_refcount)
>;

#else

// Fallback to default Immer memory policy when not using Boehm GC
using gc_memory_policy = immer::default_memory_policy;

#endif

/**
 * Persistent list type for Nix.
 *
 * Uses immer::flex_vector which supports:
 * - O(log n) concatenation via + operator
 * - O(log n) take() and drop() for slicing (efficient tail!)
 * - O(log₃₂ n) ≈ O(1) random access
 * - Structural sharing for memory efficiency
 */
using NixList = immer::flex_vector<Value*, gc_memory_policy>;

/**
 * Transient (mutable) version of NixList for efficient batch construction.
 */
using NixListTransient = typename NixList::transient_type;

} // namespace nix
