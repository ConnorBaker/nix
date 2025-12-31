#pragma once
///@file

#include "nix/expr/eval-hash.hh"
#include "nix/expr/value-hash.hh"

#include <cstddef>
#include <vector>

namespace nix {

struct Env;
struct Value;
class SymbolTable;

/**
 * Get the size (number of Value* slots) of an environment.
 *
 * This returns the `size` field stored in the Env struct, which is
 * populated by allocEnv() at allocation time.
 *
 * @param env Pointer to the environment (may be null)
 * @return The number of Value* slots, or 0 if env is null
 */
size_t getEnvSize(const Env * env);

/**
 * Compute the structural hash of an environment.
 *
 * Structural hashes capture the identity of an environment based on its
 * parent chain and the values it contains. Two environments with the same
 * structural hash are semantically equivalent for thunk interning purposes.
 *
 * The size parameter specifies how many Value* slots this environment has.
 * Note that Env also stores its size internally (env.size), which is used
 * for recursively hashing parent environments.
 *
 * @param env The environment to hash
 * @param size The number of values in the environment
 * @param symbols The symbol table for resolving attribute names in values
 * @param envAncestors Stack of envs currently being hashed (for cycle detection)
 * @param valueAncestors Stack of values currently being hashed (for value cycle detection)
 * @return Structural hash of the environment
 *
 * @note Cycles are handled via back-references using De Bruijn-like indices
 *
 * ## Content-Based Hashing
 *
 * Parent environments are hashed recursively using their stored size (env.up->size),
 * enabling content-based hashing throughout the entire parent chain. This means:
 *
 * - The hash is stable across evaluations and machines
 * - Suitable for cross-evaluation persistent caching
 * - Portability depends only on the values contained in the env chain
 */
StructuralHash computeEnvStructuralHash(
    const Env & env,
    size_t size,
    const SymbolTable & symbols,
    std::vector<const Env *> & envAncestors,
    std::vector<const Value *> & valueAncestors,
    ValueHashCache * valueCache = nullptr);

/**
 * Convenience overload that creates fresh ancestor stacks.
 */
StructuralHash computeEnvStructuralHash(
    const Env & env,
    size_t size,
    const SymbolTable & symbols,
    ValueHashCache * valueCache = nullptr);

/**
 * Overload that takes existing valueAncestors but creates fresh envAncestors.
 * Useful when hashing thunk/lambda envs from within value content hashing.
 */
StructuralHash computeEnvStructuralHash(
    const Env & env,
    size_t size,
    const SymbolTable & symbols,
    std::vector<const Value *> & valueAncestors,
    ValueHashCache * valueCache = nullptr);

/**
 * Compute env structural hash with portability tracking.
 *
 * This variant returns both the hash and its portability classification.
 * Portability is determined by the values contained in the environment
 * and its parent chain. Values containing lambdas, thunks, or external
 * values make the hash non-portable.
 *
 * @param env The environment to hash
 * @param size The number of values in the environment
 * @param symbols The symbol table for resolving attribute names in values
 * @return StructuralHashResult containing hash and portability info
 */
StructuralHashResult computeEnvStructuralHashWithPortability(
    const Env & env, size_t size, const SymbolTable & symbols);

} // namespace nix
