#pragma once
///@file

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/source-accessor.hh"

#include <filesystem>
#include <optional>

namespace nix {

struct SourcePath;

/**
 * Compute an eval-trace dep hash of data. Zero-allocation, returns stack-allocated DepHash.
 */
DepHash depHash(std::string_view data);

/**
 * Compute an eval-trace dep hash of a path's NAR serialization using streaming API.
 * Unlike depHash() which hashes raw file bytes, this captures the executable
 * bit via the NAR format. Used for builtins.path filtered file deps where
 * the resulting store path depends on permissions.
 */
DepHash depHashPath(const SourcePath & path);

/**
 * Compute an eval-trace dep hash of a directory listing using streaming API.
 * Each entry is hashed as "name:typeInt;" where typeInt is the
 * numeric value of the optional file type (-1 if unknown).
 * The entries map is iterated in its natural (lexicographic) order.
 */
DepHash depHashDirListing(const SourceAccessor::DirEntries & entries);

/**
 * Compute an eval-trace hash of a git repo's identity (HEAD rev + dirty state).
 * Returns std::nullopt if the repo has no commits yet.
 * May throw on git or filesystem errors — callers should catch as appropriate.
 *
 * Returns CurrentGitIdentityHash (phantom-typed) to prevent BUG-1: passing
 * a current hash where a stored hash is expected (or vice versa) is a compile
 * error. StoredGitIdentityHash is produced by TraceStore::extractGitIdentityHash.
 */
std::optional<CurrentGitIdentityHash> computeGitIdentityHash(const std::filesystem::path & repoRoot);

/**
 * Convert a directory entry type to its canonical string form.
 * Must be consistent between DirScalarNode::canonicalValue() (primops.cc)
 * and computeCurrentHash 'd' format handler (trace-store.cc).
 */
std::string dirEntryTypeString(std::optional<SourceAccessor::Type> type);

// ── Sentinel hash constants ───────────────────────────────────────────
//
// Sentinel DepHash constants used throughout eval-trace.  Accessed via
// `sentinel(SentinelHash::X)`.  Exhaustive switch on -Wswitch-enum catches
// any new variant at compile time.
//
// Why a named enum instead of inline depHash("..."):
//   depHash("<missing>") previously appeared in 6 places across 2 files.
//   A typo in any one of them (e.g., depHash("<mising>")) would silently
//   produce a different hash, causing verification to fail or pass
//   incorrectly. The enum indirection makes the sentinel a single source
//   of truth with exhaustive dispatch.
enum class SentinelHash : uint8_t {
    Zero,      ///< depHash("0") — zero-length array, hasAttr=false
    One,       ///< depHash("1") — hasAttr=true
    Object,    ///< depHash("object") — container type tag
    Array,     ///< depHash("array") — container type tag
    Empty,     ///< depHash("") — empty-object blocking #keys dep
    Missing,   ///< depHash("<missing>") — missing file/path sentinel
};

const DepHash & sentinel(SentinelHash kind) noexcept;

} // namespace nix
