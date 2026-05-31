#pragma once
///@file
///
/// Content-keyed parse cache.
///
/// Headline workload: `builtins.fromJSON (builtins.readFile X)`. When
/// `X` resolves to a content-fingerprintable source (e.g. a Git
/// blob), the parse can be skipped on subsequent evaluations — the
/// parsed Value tree is persisted under the source's content
/// fingerprint plus a parser-version tag.
///
/// Two pieces:
///
///   1. A per-`EvalState` side-table from `const StringData *` to the
///      source fingerprint of the bytes that produced it. `prim_readFile`
///      writes; `prim_fromJSON` reads. Cleared on `EvalState::resetFileCache`
///      (REPL `:reload`) — the GC may reuse `StringData` addresses
///      across that boundary, but never within one eval session.
///
///   2. A persistent per-machine SQLite at
///      `~/.cache/nix/parse-cache-v1.sqlite` with
///      `(fingerprint, format, parserKey, path) → encoded Value tree`.

#include "nix/util/canon-path.hh"
#include "nix/util/ref.hh"
#include "nix/util/sync.hh"

#include <boost/unordered/concurrent_flat_map.hpp>
#include <memory>
#include <optional>
#include <string>

namespace nix {

class StringData;
struct Value;
struct EvalState;

struct StringFingerprint
{
    CanonPath returnedPath = CanonPath::root;
    std::string fingerprint;
};

/**
 * Per-`EvalState` side-table associating result strings of
 * `prim_readFile` with the source's content fingerprint, so that a
 * later `prim_fromJSON` of that same string can construct a
 * persistent cache key.
 *
 * Keyed on `StringData *` allocation identity. The map does NOT keep
 * StringData alive — clearing on `resetFileCache` is the safe boundary
 * against GC address reuse.
 */
using StringFingerprintMap = boost::concurrent_flat_map<const StringData *, StringFingerprint>;

/**
 * Persistent parse cache (SQLite-backed). One instance per process,
 * obtained lazily on first use.
 *
 * The cache stores `Value` trees as a tagged binary blob. Symbol names
 * are encoded as bytes (not symbol IDs) so two processes deserialise
 * identical trees regardless of intern order — required for
 * cross-process determinism (REVIEW-DISCIPLINE.md R2).
 */
struct ParseCache
{
    virtual ~ParseCache() = default;

    /** Look up a previously-parsed Value tree. On hit `out` is
     *  populated and we return true. */
    virtual bool lookup(
        std::string_view fingerprint,
        std::string_view format,
        std::string_view parserKey,
        const CanonPath & path,
        EvalState & state,
        Value & out) = 0;

    /** Encode `value` to the cache's wire format and persist under the
     *  given key. Returns false (and doesn't write) if `value`
     *  contains non-data forms (functions, thunks, externals) — those
     *  shapes can't be deterministically replayed. */
    virtual bool upsert(
        std::string_view fingerprint,
        std::string_view format,
        std::string_view parserKey,
        const CanonPath & path,
        EvalState & state,
        const Value & value) = 0;
};

/** Default singleton accessor — opens the SQLite db on first call. */
ref<ParseCache> getParseCache();

} // namespace nix
