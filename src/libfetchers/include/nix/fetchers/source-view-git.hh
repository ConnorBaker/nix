#pragma once
///@file
///
/// `resolveViewToGitTree` — bridge from the layering-clean
/// `SourceView` abstraction (libutil) to libgit2 tree synthesis
/// (libfetchers' `GitRepo`).
///
/// The Track H stretch: when a `SourceView`'s recipe can be expressed
/// as a Git tree OID *cheaply* — by looking up an existing OID
/// (`Root`/`TreeHandle`/`Subtree`) or by synthesising one from
/// existing tree/blob OIDs (`Subset`) — return it. The OID can then
/// be looked up in the persistent `treeHashToNarHash` projection
/// (Track A) for a walk-free first-eval cache hit.
///
/// "Cheap" here means metadata-only: we read tree objects to find
/// entry names + child OIDs, but we never read blob bytes. Synthesis
/// via `git_treebuilder_*` writes a new tree object whose entries
/// reuse the base tree's blob OIDs verbatim.

#include "nix/fetchers/git-utils.hh"
#include "nix/util/source-view.hh"

#include <optional>

namespace nix {

namespace fetchers {
struct Settings;
}

struct ContentAddressMethod;

/** Try to express `view` as a Git tree OID using `repo`'s ODB.
 *
 * Returns the OID on success. Returns nullopt for views whose
 * recipe doesn't admit cheap tree expression (e.g. `LocalCheckout`,
 * `Overlay` over non-Git entries, `Subset` with empty
 * `acceptedPaths`).
 *
 * `path` selects which sub-element of the view we're asking about
 * — for `Root`/`Subset`/`Overlay` this is the view root; for
 * `Subtree` it's used to derive the subpath OID.
 */
std::optional<Hash>
resolveViewToGitTree(SourceViewAccessor & view, GitRepo & repo, const CanonPath & path = CanonPath::root);

/** Compute the synthetic Git tree OID for a filtered-subset `view`,
 *  WITHOUT flushing it to a packfile (`GitRepo::synthesiseTreeOid`).
 *
 *  Returns the OID iff:
 *    - `view.base` is a Git-rooted accessor (`getGitRepoOf` succeeds),
 *    - the recipe is a `Subset` with a non-empty `acceptedPaths`, and
 *    - the base subtree OID can be recovered (`resolveViewToGitTree`'s
 *      Subset arm).
 *
 *  Returns `nullopt` for non-Git bases, non-Subset recipes, an empty
 *  accept-set, or any synthesis failure. The returned OID is meaningful
 *  ONLY as a `treeHashToNarHash` cache key (Track Z.gap5) — it is the
 *  cross-base/cross-pipeline normaliser: two different base trees (or
 *  filter pipelines) that accept an identical subset synthesise the
 *  SAME OID and therefore share one `treeHashToNarHash` row, which the
 *  `;shape`-suffixed `filteredSourcePathToHash` key (tied to the source
 *  fingerprint) does not give them.
 *
 *  This is NOT walk-avoidance: on a cold `treeHashToNarHash` miss the
 *  caller still walks the filtered view to fill the row. Only the tree
 *  CONSTRUCTION here is blob-free. */
std::optional<Hash> synthesiseViewTreeOid(const fetchers::Settings & settings, SourceViewAccessor & view);

/* NB: the scheduler consults the `treeHashToNarHash` projection on the
   synthetic OID directly (it computes the OID once via
   `synthesiseViewTreeOid` and threads it through both the peek and the
   writeback — PF-4), so there is no separate `peekSynthesisedNarHash`
   helper. */

/** Build an overlay view from a dirty-workdir snapshot.
 *
 * The base accessor is the committed-tree view (rooted at
 * `wd.headRev`); the overlay accessor backs reads of modified/added
 * paths with workdir bytes. Deletions become whiteouts.
 *
 * The view's `entries` is exactly `wd.dirtyFiles`; its `whiteouts`
 * is exactly `wd.deletedFiles`. Untracked files are deliberately
 * out of scope — they are not part of `git ls-tree HEAD` and
 * surfacing them would change `git+file://` flake input semantics.
 *
 * Identity for the resulting view is `LocalCapability` today: the
 * Track H stretch can synthesise a tree OID from base + delta via
 * `git_treebuilder_*` (issue: must hash the overlay bytes for new
 * blobs), but that synthesis isn't in the current substrate. The
 * cache row for an overlay view therefore comes from `base`'s
 * `headRev`, not the overlay itself; the workdir delta is local
 * provenance only.
 */
ref<SourceViewAccessor> makeWorkdirOverlay(
    ref<SourceAccessor> baseAccessor, ref<SourceAccessor> workdirAccessor, const GitRepo::WorkdirInfo & wd);

} // namespace nix
