#pragma once
///@file
///
/// Strongly-typed ID types for eval trace component interning.
/// Each ID is a Tagged<Tag, uint32_t> with an inline tag struct —
/// the tag is never referenced by name outside this alias.

#include "nix/util/singleton/dispatch.hh"
#include "nix/util/tagged.hh"
#include "nix/util/util.hh"

#include <cstdint>
#include <limits>
#include <variant>

namespace nix {

/// Interned dep source ID (registered identity, index in StringInternTable).
/// uint32_t because it shares the same table as StringId/DepKeyId.
using DepSourceId = Tagged<struct DepSourceTag_, uint32_t>;

/// Interned file path ID (shared Strings table index).
using FilePathId = Tagged<struct FilePathTag_, uint32_t>;

/// Interned absolute git-repo-root path.  Identifies the repo a
/// file-content dep lives under at recording time.  ID 0 means "no
/// governing repo" — the file is not inside any git repo.  Shares the
/// same StringInternTable substrate as DepSourceId/FilePathId.
using RepoRootId = Tagged<struct RepoRootTag_, uint32_t>;

/// DataPath trie node ID (index into DataPathPool::nodes). ID 0 = root.
using DataPathId = Tagged<struct DataPathTag_, uint32_t>;

/// Interned dep key ID (dep key string index in StringInternTable).
/// Used by Dep to avoid per-dep string allocation in dep vectors.
using DepKeyId = Tagged<struct DepKeyTag_, uint32_t>;

/// Interned ID for ordinary string-like simple dep keys.
using SimpleDepKeyId = Tagged<struct SimpleDepKeyTag_, uint32_t>;

/// Shape of a simple (non-structured, non-trace-context) dep key.
/// Each shape corresponds to a distinct Tagged<singleton::Tag<shape>, DepKeyId>
/// so the three compound shapes are non-interconvertible at compile time.
enum class SimpleKeyShape : uint8_t {
    DerivedStorePath,
    StorePathAvailability,
    RuntimeFetchIdentity,
};

/// Tagged wrapper for compound simple-key IDs. The phantom tag is
/// `singleton::Tag<SimpleKeyShape::X>`, a distinct type per shape, so
/// `Tagged<_, DepKeyId>::Hash` delegates to `DepKeyId::Hash` automatically
/// and the three aliases below remain mutually non-convertible.
template<SimpleKeyShape S>
using TypedDepKeyIdFor = Tagged<singleton::Tag<S>, DepKeyId>;

using DerivedStorePathDepKeyId      = TypedDepKeyIdFor<SimpleKeyShape::DerivedStorePath>;
using StorePathAvailabilityDepKeyId = TypedDepKeyIdFor<SimpleKeyShape::StorePathAvailability>;
using RuntimeFetchIdentityDepKeyId  = TypedDepKeyIdFor<SimpleKeyShape::RuntimeFetchIdentity>;

using TypedDepKeyId = std::variant<
    SimpleDepKeyId,
    DerivedStorePathDepKeyId,
    StorePathAvailabilityDepKeyId,
    RuntimeFetchIdentityDepKeyId>;

// Exhaustive explicit dispatch — each alternative has its own overload.
// Using overloaded{} instead of a generic lambda prevents silent infinite
// recursion when a new alternative is added without a handler (compile error
// instead of stack overflow via implicit variant conversion).
static_assert(std::variant_size_v<TypedDepKeyId> == 4,
    "eraseDepKeyType: TypedDepKeyId gained an alternative — "
    "add the corresponding visitor branch below and update this count");

inline constexpr DepKeyId eraseDepKeyType(const TypedDepKeyId & keyId) noexcept
{
    return std::visit(overloaded{
        [](SimpleDepKeyId k) -> DepKeyId { return DepKeyId{k.value}; },
        [](DerivedStorePathDepKeyId k) -> DepKeyId { return k.value; },
        [](StorePathAvailabilityDepKeyId k) -> DepKeyId { return k.value; },
        [](RuntimeFetchIdentityDepKeyId k) -> DepKeyId { return k.value; },
    }, keyId);
}

/// Interned string ID for DB-level dep source/key storage.
/// Shares the same StringInternTable as DepSourceId and DepKeyId.
using StringId = Tagged<struct StringIdTag_, uint32_t>;

/// Interned attr name ID (index in AttrVocabStore::nameTable).
using AttrNameId = Tagged<struct AttrNameIdTag_, uint32_t>;

/// Interned attr path ID (trie node in AttrVocabStore::paths).
/// ID 0 = root sentinel.
using AttrPathId = Tagged<struct AttrPathIdTag_, uint32_t>;

/// Session-local monotonic stamp for a child definition slot.
using DefinitionStamp = Tagged<struct DefinitionStampTag_, uint32_t>;

/// Session-local monotonic stamp for a materialized child slot.
using SlotStamp = Tagged<struct SlotStampTag_, uint32_t>;

/// Store-local monotonic stamp for an in-memory current-node snapshot.
using NodeStamp = Tagged<struct NodeStampTag_, uint32_t>;

/// Session-local monotonic stamp for container/value identity.
using ValueIdentityStamp = Tagged<struct ValueIdentityStampTag_, uint32_t>;

/// Typed parent slot ID for sibling-parent locality.
using ParentSlot = Tagged<struct ParentSlotTag_, AttrPathId>;

/// Typed sibling identity for already-materialized sibling detection.
inline constexpr uint32_t invalidSiblingIndex = std::numeric_limits<uint32_t>::max();

struct SiblingIdentity
{
    ParentSlot parentSlot{};
    DefinitionStamp definitionStamp{};
    SlotStamp slotStamp{};
    uint32_t canonicalSiblingIdx = invalidSiblingIndex;

    bool operator==(const SiblingIdentity & other) const = default;
};

/// Typed value-context payload for trace-context emission keys.
using ValueContext = Tagged<struct ValueContextTag_, AttrPathId>;

} // namespace nix
