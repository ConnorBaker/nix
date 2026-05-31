#include "nix/fetchers/source-view-git.hh"
#include "nix/fetchers/projection.hh"
#include "nix/store/content-address.hh"
#include "nix/util/fingerprint.hh"

#include <functional>
#include <variant>

namespace nix {

ref<SourceViewAccessor> makeWorkdirOverlay(
    ref<SourceAccessor> baseAccessor, ref<SourceAccessor> workdirAccessor, const GitRepo::WorkdirInfo & wd)
{
    return sourceViewOverlay(std::move(baseAccessor), std::move(workdirAccessor), wd.dirtyFiles, wd.deletedFiles);
}

namespace {

/* Shared core for `resolveViewToGitTree` and `synthesiseViewTreeOid`:
   the recipe→OID dispatch is identical; the two callers differ ONLY in
   whether the `Subset` arm flushes the synthesised tree to a packfile.
   `synthesise(baseTreeOid, acceptedPaths)` is the seam — pass
   `GitRepo::synthesiseTree` (flushing, retrievable) or
   `GitRepo::synthesiseTreeOid` (non-flushing, identity-only). */
std::optional<Hash> resolveViewToGitTreeWith(
    SourceViewAccessor & view,
    const CanonPath & path,
    const std::function<Hash(const Hash &, const std::set<CanonPath> &)> & synthesise)
{
    /* Re-anchor onto the base via the SAME translation `getIdentity`
       uses (`recipeBasePath`), so the tree we synthesise/look up keys
       on the same base path the identity/cache row does. */
    auto basePath = recipeBasePath(view.recipe, path);
    auto baseTreeOid = [&]() -> std::optional<Hash> {
        auto [_, fp] = view.base->getFingerprint(basePath);
        return fp ? bareTreeOid(*fp) : std::nullopt;
    };

    /* Policy question (3) — see the per-recipe policy matrix at the
       `ViewRecipe` definition in source-view.hh. NB Subtree returns
       baseTreeOid() HERE (path-anchored), unlike getRootTreeHash
       (question 2, path-less → nullopt). */
    return std::visit(
        overloaded{
            [&](const recipe::Root &) -> std::optional<Hash> { return baseTreeOid(); },
            [&](const recipe::Subtree &) -> std::optional<Hash> { return baseTreeOid(); },
            [&](const recipe::Subset & sub) -> std::optional<Hash> {
                if (sub.acceptedPaths.empty())
                    return std::nullopt;
                auto oid = baseTreeOid();
                if (!oid)
                    return std::nullopt;
                try {
                    return synthesise(*oid, sub.acceptedPaths);
                } catch (Error & e) {
                    debug("synthesiseTree failed: %s", e.what());
                    return std::nullopt;
                }
            },
            [&](const recipe::Overlay &) -> std::optional<Hash> { return std::nullopt; },
            [&](const recipe::TreeHandle & th) -> std::optional<Hash> { return th.treeOid; },
            [&](const recipe::LocalCheckout &) -> std::optional<Hash> { return std::nullopt; },
        },
        view.recipe);
}

} // namespace

std::optional<Hash> resolveViewToGitTree(SourceViewAccessor & view, GitRepo & repo, const CanonPath & path)
{
    return resolveViewToGitTreeWith(
        view, path, [&](const Hash & base, const std::set<CanonPath> & accepted) {
            return repo.synthesiseTree(base, accepted);
        });
}

std::optional<Hash> synthesiseViewTreeOid(const fetchers::Settings & settings, SourceViewAccessor & view)
{
    (void) settings;
    /* Only a Git-rooted base can synthesise a tree OID. Non-git bases
       (memory/POSIX/non-git view substrate) return nullopt. */
    auto * repo = getGitRepoOf(*view.base);
    if (!repo)
        return std::nullopt;

    /* Non-flushing synthesis: the OID is a `treeHashToNarHash` cache key
       only, never re-opened, so we skip the permanent packfile flush.
       The `resolveViewToGitTreeWith` dispatch returns the synthetic OID
       ONLY for a non-empty `Subset` (the other recipe arms return a
       pre-existing OID or nullopt — none of which are the cross-base
       normalisation case Track Z.gap5 targets). We restrict to Subset
       here so a non-Subset view doesn't masquerade as a synthesis hit. */
    if (!std::holds_alternative<recipe::Subset>(view.recipe))
        return std::nullopt;

    return resolveViewToGitTreeWith(
        view, CanonPath::root, [&](const Hash & base, const std::set<CanonPath> & accepted) {
            return repo->synthesiseTreeOid(base, accepted);
        });
}

} // namespace nix
