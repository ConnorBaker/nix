#pragma once
///@file
///
/// `SourceView` — internal abstraction for source accessors with
/// recipe-aware identity and provenance.
///
/// The proposal at doc/tecnix-survey/PROPOSAL.md §"Source Views"
/// argues that subtree narrowing, accepted-set subsets, dirty
/// overlays, arbitrary tree handles, and on-disk checkout roots are
/// four flavours of one model: a wrapped accessor with a *recipe*
/// recording how it was constructed plus *identity* and *provenance*
/// derived from the recipe.
///
/// We keep the existing `SourceAccessor` chain (Mounted, Filtering,
/// Caching, …) as the read substrate. `SourceView` is a thin
/// decorator that:
///
///   - records the `ViewRecipe` (a tagged union over the construction
///     patterns) so callers and identity-derivation code can ask
///     "how was this view built?",
///   - exposes `SourceViewIdentity` separating content-addressed
///     identity (`fingerprint`, `gitTree`) from capability identity
///     (purity flag), with `narHash` always virtual (deferred),
///   - exposes `SourceProvenance` for non-content-addressed local
///     facts (on-disk checkout root, sparse-checkout roots, workdir
///     delta).
///
/// Crucially, content-addressed identity and local provenance are
/// orthogonal types. Provenance MAY be queryable but MUST NOT be
/// substitutable; persistent cache rows must come from identity, not
/// provenance.
///
/// This abstraction does NOT add a new public Nix-language builtin.
/// `builtins.path`, `builtins.fetchTree`, etc. still go through the
/// existing flow; `SourceView` is what the C++ side constructs
/// internally so the various view shapes share one model.

#include "nix/util/canon-path.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"
#include "nix/util/source-accessor.hh"

#include <filesystem>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace nix {

/* ---------- ViewRecipe ----------
 *
 * A small `std::variant` over the construction patterns. Each
 * alternative is the minimal payload that defines the view shape;
 * the actual *read* substrate is the wrapped `SourceAccessor`.
 */

namespace recipe {

/** The whole accessor as-is. Content identity (if any) is the
 *  accessor's own. */
struct Root
{};

/** Narrow to a subpath of the underlying accessor. The base accessor
 *  still does the reads; identity (`tree:<sha>`) is computed by
 *  consulting the base's `getFingerprint` at the subpath. */
struct Subtree
{
    CanonPath subpath;
};

/** Apply a predicate-derived accepted set. The shape hash that
 *  identifies the subset has already been computed (track D), so
 *  the recipe stores it for later identity queries.
 *
 *  When the base accessor is Git-rooted, `acceptedPaths` lets the
 *  Track H stretch synthesise a Git tree OID via
 *  `git_treebuilder_*` without reading any blobs — and that
 *  synthetic OID can be looked up in the persistent
 *  `treeHashToNarHash` projection (Track A) for a walk-free first
 *  eval.
 *
 *  `subpath` re-anchors the view at a subtree of the base before
 *  applying the filter (the `doc/tecnix-survey/PROPOSAL.md` §1.4
 *  `Restrict(S) ∘ Translate(p)` composition). Default `/` means
 *  no re-anchoring; `acceptedPaths` are interpreted relative to
 *  `subpath` (i.e. relative to the wrapper's `/`-rooted namespace,
 *  not absolute on `base`). */
struct Subset
{
    Hash shapeHash;
    /** Absolute paths (on the source accessor) that the filter
     *  accepted. Stored so `toGitTree()` can synthesise. */
    std::set<CanonPath> acceptedPaths;
    /** Subpath of the base at which the view is re-anchored before
     *  the filter applies. Default is the base root. */
    CanonPath subpath = CanonPath::root;
};

/** Overlay file-level entries on top of a base. `whiteouts` are
 *  paths to be hidden from the base; `entries` are added/modified
 *  paths whose bytes live in the overlay accessor. */
struct Overlay
{
    /* Paths hidden from the base (rename-as-delete-add: the source
       half of a rename is a whiteout). */
    std::set<CanonPath> whiteouts;
    /* Paths whose bytes come from the overlay accessor instead of
       the base. */
    std::set<CanonPath> entries;
};

/** Materialise an arbitrary git tree (not anchored at a commit).
 *  Identity is exactly the tree OID. */
struct TreeHandle
{
    Hash treeOid;
};

/** A local checkout — on-disk path is exposed as a capability, but
 *  identity is "local" (NOT a substituter key). */
struct LocalCheckout
{
    std::filesystem::path checkoutPath;
};

} // namespace recipe

using ViewRecipe = std::
    variant<recipe::Root, recipe::Subtree, recipe::Subset, recipe::Overlay, recipe::TreeHandle, recipe::LocalCheckout>;

/* ---------- Per-recipe policy matrix (the single source of truth) ----------
 *
 * Adding a recipe variant means deciding its answer to FOUR independent
 * questions. `std::visit(overloaded{…})` over `ViewRecipe` is exhaustive
 * (omitting an arm is a COMPILE error), so the compiler already forces you
 * to touch every site; this table is the *semantic* anchor that says what
 * each arm should return and WHY the four are NOT one policy.
 *
 *   (1) recipeBasePath()                  [source-view.cc]
 *       How a wrapper-namespace path re-anchors onto `base`.
 *       Subtree/Subset → `subpath / path`;  others → `path`.
 *
 *   (2) SourceViewAccessor::getRootTreeHash()   [source-view.cc]
 *       PATH-LESS, whole-view root-NAR identity. Returns a tree OID ONLY
 *       when the view's whole NAR == that tree's NAR byte-for-byte.
 *       Root → base->getRootTreeHash();  TreeHandle → its OID;  everything
 *       else (incl. Subtree) → nullopt. Subtree is nullopt HERE because a
 *       path-less query cannot pick the sub-OID and the base already
 *       bridges `tree:<sub-sha>` via getFingerprint.
 *
 *   (3) resolveViewToGitTreeWith()        [source-view-git.cc]
 *       PATH-ANCHORED OID resolution/synthesis at a given path.
 *       Root/Subtree → baseTreeOid(at the re-anchored path);  Subset →
 *       synthesise(baseTreeOid, acceptedPaths);  TreeHandle → its OID;
 *       Overlay/LocalCheckout → nullopt.
 *
 *   NOTE the deliberate asymmetry between (2) and (3) on Subtree: (2) is
 *   nullopt (no path to anchor), (3) is baseTreeOid() (path supplied).
 *   They answer DIFFERENT questions, so they are intentionally NOT folded
 *   into a single descriptor — a shared `RecipePolicy` struct would have
 *   to special-case Subtree anyway and would obscure, not clarify, this.
 *
 *   (4) factory construction              [source-view-factories.cc]
 *       Which operator stack each recipe assembles (Translate/Restrict/…).
 *
 * When you add a variant: answer (1)–(4) here first, then fill the arms;
 * the compiler will not let you forget a site, and this comment tells you
 * what each arm's answer must be. */

/* ---------- Recipe regions ----------
 *
 * Per `doc/tecnix-survey/PROPOSAL.md` §1.4 (recipe → operator
 * composition): each recipe variant
 * declares two path-sets, used by the source-view factory to wire
 * the right operator with the right input. The two regions are
 * algebraically distinct — conflating them was the bug the §11
 * lock-in #5 corrected.
 *
 *   `layer1Region`: paths whose ancestors need walkability. The
 *     **introduction set** — places the recipe puts leaves the base
 *     accessor doesn't have. Drives `DirectorySynthesizer`'s S
 *     argument. Whiteouts do NOT contribute (deletion masks base
 *     content but doesn't introduce leaves).
 *
 *   `layer2Region`: paths whose fingerprint must bypass. The
 *     **modified-content set** — places the recipe changes what
 *     the merged view's bytes are. Drives `SoundnessGuard`'s R
 *     argument. Whiteouts DO contribute (a directory's listing
 *     loses children — merged content differs from base).
 *
 * Both regions are interpreted in the **wrapper's `/`-rooted
 * namespace** (post-Translate, pre-Restrict). For Subset, that
 * means `subpath` does NOT prepend onto either region — Subset
 * `acceptedPaths` are already relative to the wrapper's `/` per
 * `recipe::Subset`'s doc-comment.
 */
/** Re-anchor a wrapper-namespace path onto the base accessor
 *  (Subtree/Subset prepend `subpath`; all others pass through). Single
 *  source of truth for `SourceViewAccessor::getIdentity` and
 *  `resolveViewToGitTree`, which must agree on the translation. */
CanonPath recipeBasePath(const ViewRecipe & recipe, const CanonPath & path);

/** Both region sets, computed in one visit over the recipe. */
struct ViewRegions
{
    std::set<CanonPath> layer1;
    std::set<CanonPath> layer2;
};

/** Compute layer1 + layer2 regions together (one visit). Prefer this
 *  when a caller needs both (e.g. the Overlay factory); the single-set
 *  `layer1Region`/`layer2Region` wrappers below are kept for callers
 *  (and tests) that need only one. */
ViewRegions regions(const ViewRecipe & recipe);

std::set<CanonPath> layer1Region(const ViewRecipe & recipe);
std::set<CanonPath> layer2Region(const ViewRecipe & recipe);

/* ---------- SourceViewIdentity ----------
 *
 * Separates the three shapes of identity a view can carry:
 *
 *   - `fingerprint`: the persistent content-keyed cache key (the
 *     same string master's `SourceAccessor::fingerprint` field
 *     stores). Empty for `LocalCheckout` purity.
 *
 *   - `gitTree`: a real or synthetic Git tree OID, when the recipe
 *     can produce one cheaply (Root/Subtree/TreeHandle, and
 *     synthesizable Subset/Overlay over a Git base). Lets
 *     downstream callers prefer Git-shaped paths over NAR paths
 *     without first computing a NAR hash.
 *
 *   - `narHash`: deferred. The virtual-narHash design makes
 *     this an `optional<LazyAttr>` so callers that don't need it
 *     don't force a tree walk. For now we keep it as an
 *     `optional<Hash>` populated only when the recipe yielded one
 *     eagerly (e.g. via the substitution fast path); the virtual
 *     thunk path is the deferred Track O work.
 */
enum class ViewPurity { ContentAddressed, LocalCapability };

struct SourceViewIdentity
{
    std::optional<std::string> fingerprint;
    std::optional<Hash> gitTree;
    std::optional<Hash> narHash;
    ViewPurity purity = ViewPurity::ContentAddressed;
};

/* ---------- SourceProvenance ----------
 *
 * Local facts a view exposes that are NOT substituter keys.
 *
 * The proposal explicitly forbids using these as cache rows:
 * on-disk checkout paths and sparse-checkout roots are local
 * capability state and must not survive `nix copy` to another
 * machine.
 */
struct SourceProvenance
{
    /** If the view has an on-disk checkout path (`LocalCheckout` or
     *  an overlay rooted at one), expose it. Otherwise empty. */
    std::optional<std::filesystem::path> checkoutRoot;

    /** Subpaths the local checkout was sparsely materialised at —
     *  parsed from `.git/info/sparse-checkout` or the tecnix
     *  equivalent. Empty when sparse checkout is not in use or not
     *  applicable. */
    std::set<CanonPath> sparseCheckoutRoots;

    /** Workdir delta entries (added/modified/deleted/renamed/
     *  untracked under explicit policy). */
    std::set<CanonPath> dirtyFiles;
    std::set<CanonPath> deletedFiles;
};

/* ---------- SourceViewAccessor ---------- */

/**
 * Decorates an underlying `SourceAccessor` with view-recipe-aware
 * identity and provenance metadata.
 *
 * Reads dispatch through `readImpl`, the operator stack the factory
 * built from `recipe`. The operator stack is the read substrate;
 * `recipe`/`identity`/`provenance` are recipe-derived metadata used
 * by `getIdentity`, `getProvenance`, and downstream consumers (e.g.
 * `resolveViewToGitTree` in libfetchers).
 *
 * `getFingerprint` is the three-step "identity override → readImpl
 * → own-fingerprint fallback" pattern documented in
 * `doc/tecnix-survey/PROPOSAL.md` §2.Q (Stage 2).
 */
struct SourceViewAccessor : SourceAccessor
{
    /** The underlying source. Used by `getIdentity` (recipe-aware
     *  path translation) and `synthesiseTree`/`resolveViewToGitTree`.
     *  NOT consulted on read paths — those go through `readImpl`. */
    ref<SourceAccessor> base;

    /** The operator stack built by the factory. All read methods on
     *  `SourceViewAccessor` forward to this. For trivial recipes
     *  (Root, empty Subset, empty Overlay, …) factories short-circuit
     *  to `readImpl = base`. */
    ref<SourceAccessor> readImpl;

    /** Sealed-tag construction record. Drives `getIdentity` path
     *  translation, Git-tree synthesis (`resolveViewToGitTree`), and
     *  display routing for Overlay views. */
    ViewRecipe recipe;

    /** Recipe-derived identity. */
    SourceViewIdentity identity;

    /** Recipe-derived provenance (local-capability; never a cache key). */
    SourceProvenance provenance;

    /** For `recipe::Overlay`: the original overlay accessor. Used
     *  for `showPath` display routing (the user-meaningful workdir
     *  path) and exposing provenance to consumers. Empty otherwise. */
    std::shared_ptr<SourceAccessor> overlay;

    /** For `recipe::LocalCheckout`: the on-disk checkout root. The
     *  factory pre-stores it here so `getPhysicalPath` doesn't need
     *  to traverse the recipe variant on every call. Empty otherwise. */
    std::optional<std::filesystem::path> checkoutPath;

    SourceViewAccessor(
        ref<SourceAccessor> base,
        ref<SourceAccessor> readImpl,
        ViewRecipe recipe,
        SourceViewIdentity identity,
        SourceProvenance provenance,
        std::shared_ptr<SourceAccessor> overlay = nullptr,
        std::optional<std::filesystem::path> checkoutPath = std::nullopt);

    /* SourceAccessor virtuals — read methods are pure forwards to
       `readImpl`. Identity/provenance/display methods consult the
       recipe-derived metadata. */
    using SourceAccessor::readFile;
    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override;
    /* No `pathExists` override: inherits `SourceAccessor::pathExists`
       (routes through this wrapper's `maybeLstat`). See the contract
       on SourceAccessor::pathExists in source-accessor.hh. */
    Stat lstat(const CanonPath & path) override;
    std::optional<Stat> maybeLstat(const CanonPath & path) override;
    DirEntries readDirectory(const CanonPath & path) override;
    std::string readLink(const CanonPath & path) override;
    std::string showPath(const CanonPath & path) override;
    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override;
    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override;
    void invalidateCache() override;
    void prefetchSubtree(const CanonPath & subpath, unsigned depth = 1) override;

    /** Forward the structured content-identity (root tree OID) through
     *  the view, so the `treeHashToNarHash` bridge (Track Z) is not
     *  blinded by the wrapper. Visits the recipe directly (it does NOT
     *  call `getIdentity`) and gates on NAR-preservation: only a Root or
     *  TreeHandle view has the SAME NAR as its base's root tree, so only
     *  those forward an OID — Root via `base->getRootTreeHash()`,
     *  TreeHandle via its `identity.gitTree`; Subtree defers to its
     *  `tree:<sub-sha>` fingerprint (the leaf already bridges it), and
     *  Subset/Overlay/LocalCheckout alter the NAR and return nullopt.
     *  This makes `getRootTreeHash` the single forwarded
     *  content-identity channel. */
    std::optional<Hash> getRootTreeHash() override;

    /** View-aware identity query. Distinct from `getFingerprint`
     *  because the latter returns the wire-format string for cache
     *  keys; `getIdentity` returns the structured identity record. */
    SourceViewIdentity getIdentity(const CanonPath & path) const;

    /** View-aware provenance query. Local-capability data that
     *  callers can use to drive UX (showing the user a real
     *  checkout path, etc.) but must not feed into substituter
     *  keys. */
    SourceProvenance getProvenance() const;
};

/* Note: `resolveViewToGitTree` lives in libfetchers
   (`nix/fetchers/source-view-git.hh`) because it needs `GitRepo`. */

/* ---------- Factories ----------
 *
 * One factory per recipe shape. Each constructs the appropriate
 * underlying read substrate (often a `MountedSourceAccessor` or
 * `FilteringSourceAccessor`) and wraps it in a `SourceViewAccessor`
 * with the right recipe + identity.
 */

/** Wrap an accessor as a Root view. Identity comes from the
 *  accessor's own `fingerprint` field; provenance is empty. */
ref<SourceViewAccessor> sourceViewRoot(ref<SourceAccessor> base);

/** Narrow `base` to `subpath`. The wrapped accessor is base
 *  itself; the recipe records the subpath so `getIdentity` can
 *  ask for the subpath-aware fingerprint. */
ref<SourceViewAccessor> sourceViewSubtree(ref<SourceAccessor> base, CanonPath subpath);

/** Construct a Subset view from a base accessor plus a
 *  pre-computed shape hash plus the accepted-path set plus an
 *  optional subpath at which to re-anchor before applying the
 *  filter. The composition is `Restrict(S) ∘ Translate(p)` —
 *  `acceptedPaths` are checked against the wrapper's `/`-rooted
 *  namespace, then `subpath` is prepended to read from base.
 *
 *  When `subpath != CanonPath::root`, the factory also seeds
 *  `view->fingerprint` from `base.getFingerprint(subpath)` so
 *  cache rows for filtered-subpath sources can share across revs
 *  that share the same subtree (the Track-B win documented in
 *  §9). */
ref<SourceViewAccessor> sourceViewSubset(
    ref<SourceAccessor> filteredBase,
    Hash shapeHash,
    std::set<CanonPath> acceptedPaths = {},
    CanonPath subpath = CanonPath::root);

/** Overlay file-level entries (`overlay`) on top of `base`. Reads
 *  go to `overlay` for paths in `entries`, returns missing for
 *  `whiteouts`, and falls back to `base` otherwise. */
ref<SourceViewAccessor> sourceViewOverlay(
    ref<SourceAccessor> base, ref<SourceAccessor> overlay, std::set<CanonPath> entries, std::set<CanonPath> whiteouts);

/** Wrap a Git tree-rooted accessor (constructed from a bare tree
 *  OID, not anchored at a commit). */
ref<SourceViewAccessor> sourceViewTreeHandle(ref<SourceAccessor> treeRooted, Hash treeOid);

/** Local checkout view: wraps a POSIX accessor over a real
 *  checkout path; identity is `LocalCapability`. */
ref<SourceViewAccessor>
sourceViewLocalCheckout(ref<SourceAccessor> base, std::filesystem::path checkoutPath, SourceProvenance provenance = {});

} // namespace nix
