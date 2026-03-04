#pragma once
///@file
///
/// Strongly-typed ID types for eval trace component interning.
/// Tags and aliases for StrongId, specific to the eval-trace subsystem.

#include "nix/util/strong-id.hh"

#include <cstdint>

namespace nix {

struct DepSourceTag {};
struct FilePathTag {};
struct DataPathTag {};
struct DepKeyTag {};

struct StringIdTag {};

struct AttrNameIdTag {};
struct AttrPathIdTag {};

/// Interned dep source ID (flake input name, index in StringInternTable).
/// uint32_t because it shares the same table as StringId/DepKeyId.
using DepSourceId = StrongId<DepSourceTag, uint32_t>;

/// Interned file path ID (file path index in StringPool16).
using FilePathId = StrongId<FilePathTag>;

/// DataPath trie node ID (index into DataPathPool::nodes). ID 0 = root.
using DataPathId = StrongId<DataPathTag, uint32_t>;

/// Interned dep key ID (dep key string index in StringInternTable).
/// Used by Dep to avoid per-dep string allocation in dep vectors.
using DepKeyId = StrongId<DepKeyTag, uint32_t>;

/// Interned string ID for DB-level dep source/key storage.
/// Shares the same StringInternTable as DepSourceId and DepKeyId.
using StringId = StrongId<StringIdTag, uint32_t>;

/// Interned attr name ID (index in AttrVocabStore::nameTable).
using AttrNameId = StrongId<AttrNameIdTag, uint32_t>;

/// Interned attr path ID (trie node in AttrVocabStore::paths).
/// ID 0 = root sentinel.
using AttrPathId = StrongId<AttrPathIdTag, uint32_t>;

} // namespace nix
