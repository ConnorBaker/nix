#include "nix/expr/env-hash.hh"

#include "nix/expr/eval.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value-hash.hh"
#include "nix/expr/value.hh"
#include "nix/util/hash.hh"

#include <bit>

namespace nix {

/**
 * Maximum reasonable env size for safety bounds checking.
 * This prevents massive over-reads from corrupted env->size values.
 * Even the largest Nix expressions rarely have more than ~10K bindings.
 */
constexpr size_t MAX_REASONABLE_ENV_SIZE = 1048576; // 1M entries

size_t getEnvSize(const Env * env)
{
    if (!env) {
        return 0;
    }
    return env->size;
}

namespace {

/**
 * Type tag for env hashing to distinguish from other hash types.
 */
constexpr uint8_t ENV_HASH_TAG = 0xE0;

/**
 * Convert a 64-bit integer to little-endian byte order.
 * This ensures consistent hashes across big-endian and little-endian machines.
 */
inline uint64_t toLittleEndian64(uint64_t v)
{
    if constexpr (std::endian::native == std::endian::big) {
        return ((v & 0xFF00000000000000ULL) >> 56) | ((v & 0x00FF000000000000ULL) >> 40)
            | ((v & 0x0000FF0000000000ULL) >> 24) | ((v & 0x000000FF00000000ULL) >> 8)
            | ((v & 0x00000000FF000000ULL) << 8) | ((v & 0x0000000000FF0000ULL) << 24)
            | ((v & 0x000000000000FF00ULL) << 40) | ((v & 0x00000000000000FFULL) << 56);
    }
    return v;
}

/**
 * Check if an environment is already in the ancestors stack (cycle detection).
 * Returns the depth from the top of the stack if found, or -1 if not found.
 */
ssize_t findEnvInAncestors(const Env * env, const std::vector<const Env *> & ancestors)
{
    for (size_t i = ancestors.size(); i > 0; --i) {
        if (ancestors[i - 1] == env) {
            return static_cast<ssize_t>(ancestors.size() - i);
        }
    }
    return -1;
}

/**
 * RAII helper to push/pop an env on the ancestors stack.
 */
class EnvAncestorGuard
{
    std::vector<const Env *> & ancestors;

public:
    EnvAncestorGuard(std::vector<const Env *> & ancestors, const Env * env)
        : ancestors(ancestors)
    {
        ancestors.push_back(env);
    }

    ~EnvAncestorGuard()
    {
        ancestors.pop_back();
    }
};

/**
 * Helper to feed raw bytes to a HashSink.
 */
inline void feedBytes(HashSink & sink, const void * data, size_t size)
{
    sink({reinterpret_cast<const char *>(data), size});
}

/**
 * Helper to feed a StructuralHash to a HashSink.
 */
inline void feedStructuralHash(HashSink & sink, const StructuralHash & h)
{
    feedBytes(sink, h.data(), h.size());
}

/**
 * Helper to feed a ContentHash to a HashSink.
 */
inline void feedContentHash(HashSink & sink, const ContentHash & h)
{
    feedBytes(sink, h.data(), h.size());
}

} // anonymous namespace

StructuralHash computeEnvStructuralHash(
    const Env & env,
    size_t size,
    const SymbolTable & symbols,
    std::vector<const Env *> & envAncestors,
    std::vector<const Value *> & valueAncestors,
    ValueHashCache * valueCache)
{
    // Cycle detection: check if this env is already being hashed
    ssize_t depth = findEnvInAncestors(&env, envAncestors);
    if (depth >= 0) {
        return StructuralHash::backRef(static_cast<size_t>(depth));
    }

    // Bounds validation: prevent massive over-reads from corrupted env->size
    if (size > MAX_REASONABLE_ENV_SIZE) {
        // Return a placeholder hash for corrupt envs rather than crashing
        return StructuralHash::placeholder();
    }

    // Push onto ancestor stack
    EnvAncestorGuard guard(envAncestors, &env);

    HashSink sink(evalHashAlgo);

    // Tag to identify this as an env hash
    feedBytes(sink, &ENV_HASH_TAG, sizeof(ENV_HASH_TAG));

    // Hash the size (little-endian for cross-machine stability)
    uint64_t sizeVal = toLittleEndian64(size);
    feedBytes(sink, &sizeVal, sizeof(sizeVal));

    // Hash the parent env (with cycle detection)
    if (env.up) {
        uint8_t hasParent = 1;
        feedBytes(sink, &hasParent, sizeof(hasParent));

        // Recursively hash the parent env using its stored size.
        // This enables content-based hashing across the entire parent chain,
        // making env hashes stable across evaluations and machines.
        StructuralHash parentHash =
            computeEnvStructuralHash(*env.up, env.up->size, symbols, envAncestors, valueAncestors, valueCache);
        feedStructuralHash(sink, parentHash);
    } else {
        uint8_t hasParent = 0;
        feedBytes(sink, &hasParent, sizeof(hasParent));
    }

    // Hash each value in the environment
    for (size_t i = 0; i < size; ++i) {
        const Value * v = env.values[i];
        // Check if value exists, is valid (not uninitialized), and not a blackhole
        // Blackholes indicate a value currently being forced (infinite recursion detection)
        // Uninitialized values haven't been set yet
        if (v && v->isValid() && !v->isBlackhole()) {
            uint8_t hasValue = 1;
            feedBytes(sink, &hasValue, sizeof(hasValue));

            // Compute content hash of the value (with cycle detection and caching)
            ContentHash valueHash = computeValueContentHash(*v, symbols, valueAncestors, valueCache);
            feedContentHash(sink, valueHash);
        } else {
            // Null, uninitialized, or blackhole slot - use placeholder
            uint8_t hasValue = 0;
            feedBytes(sink, &hasValue, sizeof(hasValue));
        }
    }

    auto result = sink.finish();
    return StructuralHash{result.hash};
}

StructuralHash computeEnvStructuralHash(const Env & env, size_t size, const SymbolTable & symbols, ValueHashCache * valueCache)
{
    std::vector<const Env *> envAncestors;
    std::vector<const Value *> valueAncestors;
    return computeEnvStructuralHash(env, size, symbols, envAncestors, valueAncestors, valueCache);
}

StructuralHash computeEnvStructuralHash(
    const Env & env,
    size_t size,
    const SymbolTable & symbols,
    std::vector<const Value *> & valueAncestors,
    ValueHashCache * valueCache)
{
    std::vector<const Env *> envAncestors;
    return computeEnvStructuralHash(env, size, symbols, envAncestors, valueAncestors, valueCache);
}

StructuralHashResult computeEnvStructuralHashWithPortability(
    const Env & env, size_t size, const SymbolTable & symbols)
{
    // Bounds validation: prevent massive over-reads from corrupted env->size
    if (size > MAX_REASONABLE_ENV_SIZE) {
        // Return a placeholder hash and mark as non-portable for corrupt envs
        return StructuralHashResult{StructuralHash::placeholder(), HashPortability::NonPortable_Pointer};
    }

    // Portability is now determined entirely by value content.
    // Parent envs are hashed by content (using stored size), not by pointer,
    // so they no longer cause non-portability.
    HashPortability portability = HashPortability::Portable;

    // Check value portability in the env
    // (values containing lambdas/thunks/externals are non-portable)
    std::vector<const Value *> ancestors;
    for (size_t i = 0; i < size; ++i) {
        const Value * v = env.values[i];
        // Skip null, uninitialized, and blackhole values
        if (v && v->isValid() && !v->isBlackhole()) {
            auto valueResult = computeValueContentHashWithPortability(*v, symbols);
            portability = combinePortability(portability, valueResult.portability);
            if (!isPortable(portability))
                break;
        }
    }

    // Also check parent env portability recursively
    if (isPortable(portability) && env.up) {
        auto parentResult = computeEnvStructuralHashWithPortability(*env.up, env.up->size, symbols);
        portability = combinePortability(portability, parentResult.portability);
    }

    StructuralHash hash = computeEnvStructuralHash(env, size, symbols);
    return StructuralHashResult{hash, portability};
}

} // namespace nix
