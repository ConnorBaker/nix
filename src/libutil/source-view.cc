#include "nix/util/source-view.hh"
#include "nix/util/error.hh"
#include "nix/util/util.hh"

namespace nix {

SourceViewAccessor::SourceViewAccessor(
    ref<SourceAccessor> base_,
    ref<SourceAccessor> readImpl_,
    ViewRecipe recipe_,
    SourceViewIdentity identity_,
    SourceProvenance provenance_,
    std::shared_ptr<SourceAccessor> overlay_,
    std::optional<std::filesystem::path> checkoutPath_)
    : base(std::move(base_))
    , readImpl(std::move(readImpl_))
    , recipe(std::move(recipe_))
    , identity(std::move(identity_))
    , provenance(std::move(provenance_))
    , overlay(std::move(overlay_))
    , checkoutPath(std::move(checkoutPath_))
{
    /* For Overlay views, prefer the overlay accessor's display (the
       user-meaningful workdir path) over the base's synthetic tree
       display. For others, inherit from base. */
    if (std::holds_alternative<recipe::Overlay>(recipe) && overlay) {
        displayPrefix = overlay->displayPrefix;
        displaySuffix = overlay->displaySuffix;
    } else {
        displayPrefix = base->displayPrefix;
        displaySuffix = base->displaySuffix;
    }
    if (identity.fingerprint)
        fingerprint = *identity.fingerprint;
    /* For Overlay, do not inherit base's per-tree fingerprint — it
       would be unsound for dirty paths. A dirty git workdir overlay's
       fingerprint is instead set by `git.cc`'s workdir-overlay path
       (the `tree:R;d=H` delta form). */
    else if (!std::holds_alternative<recipe::Overlay>(recipe))
        fingerprint = base->fingerprint;
}

/* Read methods — pure forwards to `readImpl`. The factory is
   responsible for building an operator stack whose semantics match
   the recipe; SourceViewAccessor itself doesn't dispatch. */

void SourceViewAccessor::readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback)
{
    readImpl->readFile(path, sink, sizeCallback);
}

/* No `pathExists` override: per the SourceAccessor contract, wrapper
   accessors inherit the base's default (`maybeLstat(path).has_value()`)
   so that synthesis-adding overrides on `maybeLstat` (e.g.
   DirectorySynthesizer down the operator stack) surface through
   pathExists automatically. */

SourceAccessor::Stat SourceViewAccessor::lstat(const CanonPath & path)
{
    return readImpl->lstat(path);
}

std::optional<SourceAccessor::Stat> SourceViewAccessor::maybeLstat(const CanonPath & path)
{
    return readImpl->maybeLstat(path);
}

SourceAccessor::DirEntries SourceViewAccessor::readDirectory(const CanonPath & path)
{
    return readImpl->readDirectory(path);
}

std::string SourceViewAccessor::readLink(const CanonPath & path)
{
    return readImpl->readLink(path);
}

std::string SourceViewAccessor::showPath(const CanonPath & path)
{
    /* Overlay views show the on-disk workdir path; others go through
       the operator stack, which handles any subpath translation. */
    if (std::holds_alternative<recipe::Overlay>(recipe) && overlay)
        return overlay->showPath(path);
    return readImpl->showPath(path);
}

std::pair<CanonPath, std::optional<std::string>> SourceViewAccessor::getFingerprint(const CanonPath & path)
{
    /* Three-step fallback per doc/tecnix-survey/PROPOSAL.md §2.Q (Stage 2):
         1. identity override (recipe-supplied);
         2. operator stack (path-aware composition);
         3. wrapper's own `fingerprint` field (Input-level seed). */
    if (identity.fingerprint)
        return {path, *identity.fingerprint};
    auto [returnedPath, fp] = readImpl->getFingerprint(path);
    if (fp)
        return {returnedPath, fp};
    return {path, fingerprint};
}

std::optional<std::filesystem::path> SourceViewAccessor::getPhysicalPath(const CanonPath & path)
{
    /* LocalCheckout's view metadata wins over the operator stack. */
    if (checkoutPath)
        return *checkoutPath / path.rel();
    return readImpl->getPhysicalPath(path);
}

void SourceViewAccessor::invalidateCache()
{
    readImpl->invalidateCache();
}

void SourceViewAccessor::prefetchSubtree(const CanonPath & subpath, unsigned depth)
{
    readImpl->prefetchSubtree(subpath, depth);
}

CanonPath recipeBasePath(const ViewRecipe & recipe, const CanonPath & path)
{
    /* Re-anchor a wrapper-namespace path onto the base accessor: for
       Subtree(p)/Subset(S, p) the wrapper exposes `x` as base `p / x`;
       all other recipes are root-anchored (path passes through). This
       is the single source of truth shared by `getIdentity` and
       `resolveViewToGitTree` — they MUST agree on the translation or
       the Git-synthesis path would key on a different tree than the
       identity/cache row. */
    /* Policy question (1) — see the per-recipe policy matrix at the
       `ViewRecipe` definition in source-view.hh. */
    return std::visit(
        overloaded{
            [&](const recipe::Subtree & s) { return s.subpath / path; },
            [&](const recipe::Subset & s) { return s.subpath / path; },
            [&](const recipe::Root &) { return path; },
            [&](const recipe::Overlay &) { return path; },
            [&](const recipe::TreeHandle &) { return path; },
            [&](const recipe::LocalCheckout &) { return path; }},
        recipe);
}

SourceViewIdentity SourceViewAccessor::getIdentity(const CanonPath & path) const
{
    /* Identity vs. cache key — two distinct queries.
     *
     *   - `getFingerprint(path)` (the SourceAccessor virtual): the
     *     **cache-key function**. Routes through the operator stack
     *     (`readImpl`) so Layer-2 bypass fires for paths in
     *     `closure(S)` (Restrict/Mask/SoundnessGuard). A bypass means
     *     "this wrapper changed merged content, don't share the cache
     *     row" — and that's the right answer for cache lookups.
     *
     *   - `getIdentity(path)` (this method): the **content-keyed
     *     identity** of the underlying base at the translated path.
     *     Deliberately *pre-bypass*: it reads through `base`, not
     *     through the operator stack, so a Subset over a Git base
     *     can return the unfiltered tree-SHA at the subpath even
     *     when Restrict's bypass would have nullopt'd the cache key.
     *
     * The path translation here mirrors the one Translate applies in
     * the operator stack: for Subtree(p) and Subset(S, p), the path
     * x_view that the wrapper exposes corresponds to base path p / x.
     * Both encodings derive from the same `subpath` field — there's
     * one subpath per recipe, used at two layers (read substrate via
     * Translate, identity derivation via this method). */
    auto result = identity;
    if (!result.fingerprint) {
        auto basePath = recipeBasePath(recipe, path);
        if (auto [_, fp] = const_cast<SourceAccessor &>(*base).getFingerprint(basePath); fp)
            result.fingerprint = std::move(fp);
    }
    return result;
}

std::optional<Hash> SourceViewAccessor::getRootTreeHash()
{
    /* The view's NAR equals its base's root tree NAR ONLY for recipes
       that preserve the root NAR byte-for-byte. Forward the OID only in
       those cases; everything that alters the NAR returns nullopt so the
       `treeHashToNarHash` bridge never keys a filtered/overlaid/subtree
       NAR under the unfiltered tree OID.

       This consolidates the content-identity channels: a TreeHandle's
       OID lives in `identity.gitTree` (set by its factory); a Root view's
       OID comes from the base's own `getRootTreeHash` (the leaf
       GitSourceAccessor surfaces it). Subtree DEFERS to its
       `tree:<sub-sha>` fingerprint — the base already bridges that via
       `getFingerprint`, and forwarding here would double-handle it (and
       a path-less query cannot pick the subtree OID anyway). Subset /
       Overlay / LocalCheckout change or localise the NAR ⇒ nullopt. */
    /* Policy question (2) — see the per-recipe policy matrix at the
       `ViewRecipe` definition in source-view.hh. NB the deliberate
       Subtree asymmetry with resolveViewToGitTreeWith (question 3). */
    return std::visit(
        overloaded{
            [&](const recipe::Root &) -> std::optional<Hash> { return base->getRootTreeHash(); },
            [&](const recipe::TreeHandle & th) -> std::optional<Hash> { return th.treeOid; },
            [&](const recipe::Subtree &) -> std::optional<Hash> { return std::nullopt; },
            [&](const recipe::Subset &) -> std::optional<Hash> { return std::nullopt; },
            [&](const recipe::Overlay &) -> std::optional<Hash> { return std::nullopt; },
            [&](const recipe::LocalCheckout &) -> std::optional<Hash> { return std::nullopt; },
        },
        recipe);
}

SourceProvenance SourceViewAccessor::getProvenance() const
{
    return provenance;
}

ViewRegions regions(const ViewRecipe & recipe)
{
    /* Both regions in one visit (one arm per recipe variant), so
       adding a recipe forces updating exactly one place. layer1 =
       introduction set (ancestors needing walkability); layer2 =
       modified-content set (fingerprints that must bypass). They
       differ only for Overlay — see the per-arm notes. */
    return std::visit(
        overloaded{
            [](const recipe::Root &) -> ViewRegions { return {}; },
            [](const recipe::Subtree &) -> ViewRegions { return {}; },
            [](const recipe::Subset & s) -> ViewRegions { return {s.acceptedPaths, s.acceptedPaths}; },
            [](const recipe::Overlay & o) -> ViewRegions {
                /* layer1 = entries only: whiteouts deliberately
                   excluded — deletion masks base content but doesn't
                   introduce leaves needing walkability; including them
                   would synthesise ghost directories for whiteouts
                   whose parents don't exist in base (SKETCH §11
                   lock-in #5). layer2 = entries ∪ whiteouts: both
                   modified AND deleted paths change merged content. */
                std::set<CanonPath> l2 = o.entries;
                l2.insert(o.whiteouts.begin(), o.whiteouts.end());
                return {o.entries, std::move(l2)};
            },
            [](const recipe::TreeHandle &) -> ViewRegions { return {}; },
            [](const recipe::LocalCheckout &) -> ViewRegions { return {}; },
        },
        recipe);
}

std::set<CanonPath> layer1Region(const ViewRecipe & recipe)
{
    return regions(recipe).layer1;
}

std::set<CanonPath> layer2Region(const ViewRecipe & recipe)
{
    return regions(recipe).layer2;
}

} // namespace nix
