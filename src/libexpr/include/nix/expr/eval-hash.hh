#pragma once
///@file

#include "nix/util/hash.hh"

#include <compare>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace nix {

/**
 * Algorithm used for eval-time content hashing.
 * SHA256 is chosen for stability (BLAKE3 is experimental in Nix).
 */
constexpr HashAlgorithm evalHashAlgo = HashAlgorithm::SHA256;

/**
 * Portability classification for hashes.
 *
 * Portable hashes are stable across evaluations, machines, and time.
 * Non-portable hashes are only stable within a single evaluation.
 *
 * The persistent cache MUST reject non-portable hashes.
 */
enum class HashPortability : uint8_t {
    /**
     * Hash is stable across evaluations and machines.
     * Safe for persistent caching.
     */
    Portable,

    /**
     * Hash contains pointer-based components (env pointers, external pointers).
     * Only stable within a single evaluation. NOT safe for persistent caching.
     */
    NonPortable_Pointer,

    /**
     * Hash contains session-local components (PosIdx::hash()).
     * Only stable within a single evaluation. NOT safe for persistent caching.
     */
    NonPortable_SessionLocal,

    /**
     * Hash contains raw path strings (no fingerprint available).
     * Machine-specific. NOT safe for persistent caching.
     */
    NonPortable_RawPath,
};

/**
 * Check if a portability classification allows persistent caching.
 */
constexpr bool isPortable(HashPortability p) noexcept
{
    return p == HashPortability::Portable;
}

/**
 * Combine two portability classifications.
 * The result is the "least portable" of the two.
 */
constexpr HashPortability combinePortability(HashPortability a, HashPortability b) noexcept
{
    // Portable is the most permissive; any non-portable makes the result non-portable
    if (a == HashPortability::Portable)
        return b;
    return a; // a is already non-portable
}

/**
 * Base class for eval-specific hash wrappers.
 *
 * This provides type-safe distinction between StructuralHash and ContentHash
 * to prevent accidentally mixing them (see Decision 1 in the plan).
 */
struct EvalHashBase
{
    Hash hash;

    EvalHashBase() : hash(evalHashAlgo) {}
    explicit EvalHashBase(Hash h) : hash(std::move(h)) {}

    bool operator==(const EvalHashBase & other) const noexcept
    {
        return hash == other.hash;
    }

    std::strong_ordering operator<=>(const EvalHashBase & other) const noexcept
    {
        return hash <=> other.hash;
    }

    /**
     * Get a hex string representation of the hash.
     */
    std::string toHex() const
    {
        return hash.to_string(HashFormat::Base16, false);
    }

    /**
     * Get the raw hash bytes for low-level operations.
     */
    const uint8_t * data() const noexcept
    {
        return hash.hash;
    }

    size_t size() const noexcept
    {
        return hash.hashSize;
    }
};

/**
 * Structural hash for thunks and unevaluated expressions.
 *
 * Computed from: (exprHash, envHash)
 * Stable within: Single evaluation
 *
 * Used as cache key for within-evaluation memoization.
 * NOT suitable for cross-evaluation caching (use ContentHash for that).
 */
struct StructuralHash : EvalHashBase
{
    using EvalHashBase::EvalHashBase;

    /**
     * Create a zero/placeholder hash for cycle handling.
     */
    static StructuralHash placeholder()
    {
        return StructuralHash{Hash(evalHashAlgo)};
    }

    /**
     * Create a back-reference hash for cycle detection.
     * The depth indicates how many levels up in the ancestor stack.
     *
     * This is used during hash computation to handle cycles:
     * when we encounter a value already being hashed, we emit
     * BackRef(depth) instead of recursing infinitely.
     */
    static StructuralHash backRef(size_t depth);

    /**
     * Combine multiple structural hashes into one.
     */
    static StructuralHash combine(std::initializer_list<StructuralHash> hashes);

    /**
     * Hash a string value.
     */
    static StructuralHash fromString(std::string_view s);
};

/**
 * Content hash for forced/evaluated values.
 *
 * Computed from: Actual value content (ints, strings, attrs, etc.)
 * Stable across: Evaluations, machines, time
 *
 * Used as cache key for cross-evaluation persistent caching.
 * This is the foundation for the goal: 90%+ cache hits across nixpkgs commits.
 */
struct ContentHash : EvalHashBase
{
    using EvalHashBase::EvalHashBase;

    /**
     * Create a zero/placeholder hash for cycle handling.
     */
    static ContentHash placeholder()
    {
        return ContentHash{Hash(evalHashAlgo)};
    }

    /**
     * Create a back-reference hash for cycle detection.
     */
    static ContentHash backRef(size_t depth);

    /**
     * Combine multiple content hashes into one.
     */
    static ContentHash combine(std::initializer_list<ContentHash> hashes);

    /**
     * Hash a string value.
     */
    static ContentHash fromString(std::string_view s);

    /**
     * Hash raw bytes.
     */
    static ContentHash fromBytes(std::span<const uint8_t> bytes);
};

/**
 * A hash result that includes portability information.
 *
 * This is returned by hash functions that need to track whether the
 * resulting hash is portable (safe for persistent caching) or not.
 *
 * Usage:
 *   auto result = computeValueContentHashWithPortability(v, symbols);
 *   if (isPortable(result.portability)) {
 *       persistentCache.store(result.hash, serializedValue);
 *   }
 *
 * Note: Named EvalHashResult to avoid conflict with nix::HashResult in libutil.
 */
template<typename HashType>
struct EvalHashResult
{
    HashType hash;
    HashPortability portability;

    EvalHashResult(HashType h, HashPortability p = HashPortability::Portable)
        : hash(std::move(h))
        , portability(p)
    {
    }

    /**
     * Check if this hash is safe for persistent caching.
     */
    bool isPortable() const noexcept
    {
        return nix::isPortable(portability);
    }

    /**
     * Combine this result with another, propagating non-portability.
     */
    EvalHashResult combine(const EvalHashResult & other) const
    {
        return EvalHashResult{
            HashType::combine({hash, other.hash}), combinePortability(portability, other.portability)};
    }
};

using ContentHashResult = EvalHashResult<ContentHash>;
using StructuralHashResult = EvalHashResult<StructuralHash>;

/**
 * Boost hash_value() functions for use in Boost hash containers.
 */
inline std::size_t hash_value(const StructuralHash & h) noexcept
{
    return hash_value(h.hash);
}

inline std::size_t hash_value(const ContentHash & h) noexcept
{
    return hash_value(h.hash);
}

} // namespace nix

/**
 * std::hash specializations for use in hash tables.
 */
template<>
struct std::hash<nix::StructuralHash>
{
    std::size_t operator()(const nix::StructuralHash & h) const noexcept
    {
        return std::hash<nix::Hash>{}(h.hash);
    }
};

template<>
struct std::hash<nix::ContentHash>
{
    std::size_t operator()(const nix::ContentHash & h) const noexcept
    {
        return std::hash<nix::Hash>{}(h.hash);
    }
};
