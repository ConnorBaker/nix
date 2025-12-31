#include "nix/expr/thunk-hash.hh"

#include "nix/expr/env-hash.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/expr-hash.hh"
#include "nix/util/hash.hh"

#include <bit>

namespace nix {

namespace {

/**
 * Type tag for thunk hashing to distinguish from other hash types.
 */
constexpr uint8_t THUNK_HASH_TAG = 0xD0;

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
 * Helper to feed raw bytes to a HashSink.
 */
inline void feedBytes(HashSink & sink, const void * data, size_t size)
{
    sink({reinterpret_cast<const char *>(data), size});
}

} // anonymous namespace

StructuralHash computeThunkHash(
    const Expr * expr,
    const Env * env,
    size_t envSize,
    int tryLevel,
    const SymbolTable & symbols,
    ExprHashCache * exprCache,
    ValueHashCache * valueCache)
{
    HashSink sink(evalHashAlgo);

    // Tag to identify this as a thunk hash
    uint8_t tag = 0xD0; // Thunk tag
    feedBytes(sink, &tag, sizeof(tag));

    // Include tryEval depth in hash: the same expression may behave differently
    // inside vs outside a tryEval (e.g., `assert false` throws outside tryEval
    // but returns { success = false; } inside). This ensures we don't incorrectly
    // reuse cached results across different tryEval contexts.
    int32_t tryLevelLE = static_cast<int32_t>(tryLevel);
    if constexpr (std::endian::native == std::endian::big) {
        tryLevelLE = static_cast<int32_t>(
            ((static_cast<uint32_t>(tryLevel) & 0xFF000000) >> 24) |
            ((static_cast<uint32_t>(tryLevel) & 0x00FF0000) >> 8) |
            ((static_cast<uint32_t>(tryLevel) & 0x0000FF00) << 8) |
            ((static_cast<uint32_t>(tryLevel) & 0x000000FF) << 24));
    }
    feedBytes(sink, &tryLevelLE, sizeof(tryLevelLE));

    // Content-based expression hash for cross-evaluation portability.
    // Expression hashes are cached by pointer in exprCache for performance.
    ContentHash exprHash = hashExpr(expr, symbols, exprCache);
    feedBytes(sink, exprHash.data(), exprHash.size());

    // Content-based environment hash for cross-evaluation portability.
    // Value hashes are cached by pointer in valueCache for performance.
    if (env) {
        uint8_t hasEnv = 1;
        feedBytes(sink, &hasEnv, sizeof(hasEnv));

        StructuralHash envHash = computeEnvStructuralHash(*env, envSize, symbols, valueCache);
        feedBytes(sink, envHash.data(), envHash.size());
    } else {
        uint8_t hasEnv = 0;
        feedBytes(sink, &hasEnv, sizeof(hasEnv));
    }

    auto result = sink.finish();
    return StructuralHash{result.hash};
}

StructuralHash computeThunkStructuralHash(
    const Expr * expr,
    const Env * env,
    int tryLevel,
    const SymbolTable & symbols,
    ExprHashCache * exprCache,
    ValueHashCache * valueCache)
{
    // Env now stores its size (env->size), enabling content-based hashing.
    size_t envSize = env ? env->size : 0;
    return computeThunkHash(expr, env, envSize, tryLevel, symbols, exprCache, valueCache);
}

} // namespace nix
