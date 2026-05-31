#pragma once
///@file
///
/// Structured fingerprint helpers.
///
/// A `SourceAccessor` fingerprint is a string of the form
///
///     <root-tag>[;<suffix>]*
///
/// where `<root-tag>` identifies what content the fingerprint refers to
/// (`git:<rev>`, `tree:<sha>`, `blob:<sha>;m=<mode>`, `nar:<hash>`, etc.)
/// and each `<suffix>` is `<tag>` or `<tag>=<value>` recording an
/// accessor-level concern that affects content identity (`;d=<hash>`
/// dirty workdir, `;e` export-ignore, `;l` LFS, `;s` submodules,
/// `;a=<hash>` attribute-context).
///
/// Wrappers compose by appending. `mergeSuffix` keeps suffixes in
/// alphabetical order so two stacks producing the same `(root, suffix
/// set)` produce byte-identical fingerprints regardless of wrapper
/// nesting order — the soundness invariant.
///
/// This is deliberately string-based: existing on-disk cache rows are
/// keyed on the rendered string and must remain readable. The structure
/// is in the discipline, not the storage.

#include "nix/util/hash.hh"

#include <optional>
#include <string>
#include <string_view>

namespace nix {

/**
 * Splice `;<suffix>` into `fingerprint` at the alphabetically-correct
 * position. `suffix` must start with `;`. The wrapper-stack-order
 * independence law requires alphabetical ordering: two wrappers
 * appending distinct tags must produce the same string regardless of
 * stack order.
 *
 * Throws if `suffix`'s tag (the substring up to `=` or end) already
 * appears in `fingerprint` — wrappers competing for the same identity
 * dimension is a programming error.
 */
std::string mergeFingerprintSuffix(std::string_view fingerprint, std::string_view suffix);

/**
 * Render a "blob" fingerprint for a Git blob OID + tree-entry mode.
 * The mode is required because Git blob OIDs do not encode the
 * executable bit or symlink-vs-regular interpretation, but the NAR
 * encoding distinguishes those.
 */
std::string blobFingerprint(std::string_view oidHex, std::string_view modeOctal);

/**
 * Render a "tree" fingerprint for a Git tree OID.
 */
std::string treeFingerprint(std::string_view oidHex);

/**
 * The inverse of `treeFingerprint`: if `fingerprint` is a BARE
 * `tree:<sha>` (a Git tree OID with NO suffixes), parse and return the
 * SHA-1 OID; otherwise `std::nullopt`.
 *
 * The bare-ness requirement is the soundness gate, not a parsing
 * convenience: ANY suffix (`;e` export-ignore, `;l` LFS, `;s`
 * submodules, `;d=<hash>` dirty workdir, `;a=<hash>` attr-context, or
 * the `;shape=<hash>` filter sentinel) means the accessor's NAR is NOT
 * the vanilla dump of that tree, so the OID must NOT be used as a
 * content-identity bridge key (`treeHashToNarHash`). This is the single
 * canonical decoder — `fetch-to-store.cc`'s `peekTreeHashBridge`/
 * writeback, `source-view-git.cc`, and `SourceViewAccessor::getRootTreeHash`
 * all route through it so the bridge-eligibility predicate cannot drift
 * (it previously did: one site called `Hash::parseAny` un-guarded and
 * threw on a malformed `tree:<nonhex>` where the others returned nullopt).
 */
std::optional<Hash> bareTreeOid(std::string_view fingerprint);

} // namespace nix
