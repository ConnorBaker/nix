#pragma once
///@file
///
/// Strongly-typed ID wrappers for eval trace component interning.
/// Zero-cost abstractions: same layout as raw integers, type-safe at compile time.
/// Lives in libutil so that Pos::TracedData (position.hh) can use them.

#include <cstdint>
#include <functional>

namespace nix {

/**
 * Phantom-tagged integer ID. Each tag type produces a distinct,
 * non-interconvertible ID type with zero runtime overhead.
 *
 * Usage:
 *   struct DepSourceTag {};
 *   using DepSourceId = StrongId<DepSourceTag>;
 */
template<typename Tag, typename Repr = uint16_t>
struct StrongId {
    Repr value{};

#ifndef NDEBUG
    /// Pool generation at creation time. Pools increment their generation on
    /// clear(). resolve() asserts this matches, catching use-after-clear bugs.
    /// Zero cost in release builds (field is absent).
    uint32_t generation{};
#endif

    explicit constexpr StrongId() = default;
    explicit constexpr StrongId(Repr v) : value(v) {}

#ifndef NDEBUG
    explicit constexpr StrongId(Repr v, uint32_t gen) : value(v), generation(gen) {}
#endif

    constexpr bool operator==(const StrongId & o) const { return value == o.value; }
    constexpr auto operator<=>(const StrongId & o) const { return value <=> o.value; }
    constexpr explicit operator bool() const { return value != Repr{}; }

    /// Hash functor for boost::unordered_flat_map/set.
    /// Hashes only value, not generation (identity is the index).
    struct Hash {
        using is_avalanching = void;
        std::size_t operator()(const StrongId & id) const noexcept {
            return std::hash<Repr>{}(id.value);
        }
    };
};

struct DepSourceTag {};
struct FilePathTag {};
struct DataPathTag {};
struct DepKeyTag {};

/// Interned dep source ID (flake input name index in StringPool16).
using DepSourceId = StrongId<DepSourceTag>;

/// Interned file path ID (file path index in StringPool16).
using FilePathId = StrongId<FilePathTag>;

/// DataPath trie node ID (index into DataPathPool::nodes). ID 0 = root.
using DataPathId = StrongId<DataPathTag, uint32_t>;

/// Interned dep key ID (dep key string index in DepKeyPool).
/// Used by CompactDep to avoid per-dep string allocation in sessionTraces.
using DepKeyId = StrongId<DepKeyTag, uint32_t>;

} // namespace nix
