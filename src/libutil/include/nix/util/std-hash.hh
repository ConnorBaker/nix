#pragma once
/**
 * @file
 *
 * Hashing utilities for use with `std::unordered_map`, etc. (i.e. low
 * level implementation logic, not domain logic like Nix hashing).
 */

#include <functional>

namespace nix {

/**
 * `hash_combine()` from Boost. Hash several hashable values together
 * into a single hash.
 */
inline void hash_combine(std::size_t & seed) {}

template<typename T, typename... Rest>
inline void hash_combine(std::size_t & seed, const T & v, Rest... rest)
{
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    hash_combine(seed, rest...);
}

/**
 * Value-returning variant of hash_combine.
 */
template<typename... Args>
inline std::size_t hashValues(const Args &... args)
{
    std::size_t seed = 0;
    hash_combine(seed, args...);
    return seed;
}

} // namespace nix
