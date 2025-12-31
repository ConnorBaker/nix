#pragma once
///@file

#include "nix/expr/eval-hash.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <vector>

namespace nix {

struct Value;
class SymbolTable;

/**
 * Cache for value content hashes.
 *
 * Values can be cached by pointer because:
 * - Forced values are immutable (their content never changes)
 * - Thunk values hash their (expr, env) which is also stable
 *
 * This cache dramatically improves env hashing performance by avoiding
 * redundant value tree walks.
 */
using ValueHashCache = boost::unordered_flat_map<const Value *, ContentHash>;

/**
 * Compute the content hash of a forced/evaluated Nix value.
 *
 * Content hashes capture the semantic identity of values. For most value types
 * (Int, Float, Bool, Null, String, Path, Attrs, List), these hashes are:
 * - Stable across evaluations
 * - Stable across machines (with little-endian normalization)
 * - Suitable for cross-evaluation persistent caching
 *
 * The ancestors stack is used for cycle detection: when encountering
 * a value already in the stack, a back-reference hash is returned
 * with the depth indicating how many levels up the cycle points.
 *
 * @param v The value to hash (should be forced/evaluated, not a thunk)
 * @param symbols The symbol table for resolving attribute names
 * @param ancestors Stack of values currently being hashed (for cycle detection)
 * @return Content hash of the value
 *
 * ## Stability Limitations
 *
 * Some value types use pointer-based fallback hashing for components that
 * cannot be content-hashed. These hashes are:
 * - Stable WITHIN a single evaluation (same pointer = same hash)
 * - NOT stable across evaluations (different pointer addresses)
 * - NOT suitable for cross-evaluation persistent caching
 *
 * Affected types:
 * - **Lambda (nFunction)**: Expression hash is stable, but environment pointer
 *   is used for within-evaluation stability. This is intentional: lambdas are
 *   typically called immediately, so caching forced results is more valuable
 *   than caching lambdas themselves. (Note: env sizes are now tracked in Env
 *   struct; content-based lambda hashing could be added if needed.)
 * - **Thunk (nThunk)**: Same as lambdas. Thunks should be forced before caching;
 *   their content hash comes from the forced result.
 * - **External (nExternal)**: Uses type name + pointer address. External values
 *   from plugins cannot be content-hashed without a hook from the plugin.
 * - **Path (nPath)**: Uses content-based fingerprinting via SourceAccessor when
 *   available. Falls back to raw path string only if accessor is null or path
 *   doesn't exist (e.g., after deserialization with incomplete state).
 */
ContentHash computeValueContentHash(
    const Value & v,
    const SymbolTable & symbols,
    std::vector<const Value *> & ancestors,
    ValueHashCache * cache = nullptr);

/**
 * Convenience overload that creates a fresh ancestors stack.
 */
ContentHash computeValueContentHash(
    const Value & v,
    const SymbolTable & symbols,
    ValueHashCache * cache = nullptr);

/**
 * Compute value content hash with portability tracking.
 *
 * This variant returns both the hash and its portability classification,
 * allowing callers to determine if the hash is safe for persistent caching.
 *
 * @param v The value to hash
 * @param symbols The symbol table for resolving attribute names
 * @return ContentHashResult containing hash and portability info
 */
ContentHashResult computeValueContentHashWithPortability(const Value & v, const SymbolTable & symbols);

} // namespace nix
