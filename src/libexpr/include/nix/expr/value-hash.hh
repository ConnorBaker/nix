#pragma once
///@file

#include "nix/util/hash.hh"
#include "nix/expr/value.hh"

#include <atomic>
#include <optional>

namespace nix {

class EvalState;

/**
 * Maximum number of attributes in an attrset for it to be considered hashable.
 * Larger attrsets are not hashed to avoid expensive hash computations.
 */
constexpr size_t maxHashableAttrs = 32;

/**
 * Maximum depth for recursive hashing of values.
 * Prevents stack overflow and excessive computation on deeply nested structures.
 */
constexpr size_t maxHashDepth = 8;

/**
 * Try to compute a content hash of a value.
 *
 * Returns std::nullopt if the value cannot be hashed, which occurs when:
 * - The value is a thunk (not yet forced)
 * - The value is a function
 * - The value contains unhashable nested values
 * - The value is too large (attrsets with > maxHashableAttrs)
 * - The value is too deeply nested (> maxHashDepth)
 *
 * This is used for lambda call memoization, where we want to cache
 * function calls by (lambda_identity, argument_hash).
 *
 * Note: The value must already be forced. This function does NOT force
 * thunks, as that could have side effects.
 */
std::optional<Hash> tryHashValue(EvalState & state, Value & v, size_t depth = 0);

/**
 * Check if a value is "simple" enough to be worth hashing.
 * Returns false for:
 * - Thunks
 * - Functions
 * - Large attrsets (> maxHashableAttrs)
 * - Deeply nested structures (> maxHashDepth)
 */
bool isHashableValue(EvalState & state, Value & v, size_t depth = 0);

/**
 * Try to force a value to a "simple" form and then hash it.
 *
 * Unlike tryHashValue, this will force thunks before checking if they're
 * hashable. This is useful for memoizing function calls where the argument
 * might contain thunks that evaluate to simple values.
 *
 * Safety limits:
 * - Only forces values that appear to be "small" (attrsets < maxHashableAttrs)
 * - Only recurses to maxHashDepth
 * - Gives up if a value is a function, path, or external
 * - Gives up if any thunk produces a non-simple value
 */
std::optional<Hash> tryForceAndHashValue(EvalState & state, Value & v, size_t depth = 0);

// Debug counters for understanding hash skip reasons (declared extern, defined in value-hash.cc)
extern std::atomic<size_t> nrHashSkipDepth;
extern std::atomic<size_t> nrHashSkipThunk;
extern std::atomic<size_t> nrHashSkipLargeAttrs;
extern std::atomic<size_t> nrHashSkipLargeList;
extern std::atomic<size_t> nrHashSkipExternal;
extern std::atomic<size_t> nrHashSkipNonCheapThunk;
extern std::atomic<size_t> nrHashOK;
extern std::atomic<size_t> nrHashSkipNestedThunk;
extern std::atomic<size_t> nrHashSkipNestedNonCheap;

} // namespace nix
