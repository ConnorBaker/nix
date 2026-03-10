#pragma once
///@file

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/source-accessor.hh"

#include <filesystem>
#include <optional>

namespace nix {

class SourcePath;

/**
 * Compute a BLAKE3 hash of data. Zero-allocation, returns stack-allocated Blake3Hash.
 */
Blake3Hash depHash(std::string_view data);

/**
 * Compute a BLAKE3 hash of a path's NAR serialization using streaming API.
 * Unlike depHash() which hashes raw file bytes, this captures the executable
 * bit via the NAR format. Used for builtins.path filtered file deps where
 * the resulting store path depends on permissions.
 */
Blake3Hash depHashPath(const SourcePath & path);

/**
 * Compute a BLAKE3 hash of a directory listing using streaming API.
 * Each entry is hashed as "name:typeInt;" where typeInt is the
 * numeric value of the optional file type (-1 if unknown).
 * The entries map is iterated in its natural (lexicographic) order.
 */
Blake3Hash depHashDirListing(const SourceAccessor::DirEntries & entries);

/**
 * Compute a BLAKE3 hash of a git repo's identity (HEAD rev + dirty state).
 * Returns std::nullopt if the repo has no commits yet.
 * May throw on git or filesystem errors — callers should catch as appropriate.
 */
std::optional<Blake3Hash> computeGitIdentityHash(const std::filesystem::path & repoRoot);

/**
 * Convert a directory entry type to its canonical string form.
 * Must be consistent between DirScalarNode::canonicalValue() (primops.cc)
 * and computeCurrentHash 'd' format handler (trace-store.cc).
 */
std::string dirEntryTypeString(std::optional<SourceAccessor::Type> type);

/**
 * Cached constant Blake3Hash values used in shape dep recording and verification.
 * Function-local statics avoid static initialization order issues across TUs.
 */
const Blake3Hash & kHashZero();
const Blake3Hash & kHashOne();
const Blake3Hash & kHashObject();
const Blake3Hash & kHashArray();

} // namespace nix
