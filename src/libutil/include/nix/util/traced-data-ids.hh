#pragma once
///@file
///
/// Strongly-typed ID wrappers for eval trace component interning.
/// Zero-cost abstractions: same layout as raw integers, type-safe at compile time.
/// Lives in libutil so that Pos::TracedData (position.hh) can use them.

#include <cstdint>

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
    explicit constexpr StrongId() = default;
    explicit constexpr StrongId(Repr v) : value(v) {}
    constexpr bool operator==(const StrongId &) const = default;
    constexpr auto operator<=>(const StrongId &) const = default;
    constexpr explicit operator bool() const { return value != Repr{}; }
};

struct DepSourceTag {};
struct FilePathTag {};
struct DataPathTag {};

/// Interned dep source ID (flake input name index in StringPool16).
using DepSourceId = StrongId<DepSourceTag>;

/// Interned file path ID (file path index in StringPool16).
using FilePathId = StrongId<FilePathTag>;

/// DataPath trie node ID (index into DataPathPool::nodes). ID 0 = root.
using DataPathId = StrongId<DataPathTag, uint32_t>;

} // namespace nix
