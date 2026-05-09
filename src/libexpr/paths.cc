#include "nix/store/store-api.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/fetchers/fetch-to-store.hh"

namespace nix {

SourcePath EvalState::rootPath(CanonPath path)
{
    return {rootFS, std::move(path)};
}

SourcePath EvalState::rootPath(std::string_view path)
{
    /* FIXME: Move this out of EvalState, since it's using native
       std::filesystem::path and current working directory. */
    return {rootFS, CanonPath(absPath(path).string())};
}

SourcePath EvalState::storePath(const StorePath & path)
{
    /* Use `storeFS` (not `rootFS`) so store-path SourcePaths share
       accessor identity with paths produced via the flake machinery's
       mounted-store-path helper. */
    return {storeFS.cast<SourceAccessor>(), CanonPath{store->printStorePath(path)}};
}

void EvalState::ensureLazyPathCopied(const StorePath & path)
{
    if (settings.isReadOnly())
        return;

    auto mount = storeFS->getMount(CanonPath(store->printStorePath(path)));
    if (!mount)
        return;

    /* TODO: We could memoise this in-memory if necessary. */
    auto storePath = fetchToStore(
        fetchSettings,
        *store,
        SourcePath{ref(mount)},
        /* Force a copy. mountInput does a dryRun to just calculate the storePath and narHash. */
        FetchMode::Copy,
        path.name());

    /* Catch hash mismatches more loudly. This is more likely caused by unsound
       caching of different accessor types that fetch the same repo with
       the same git revision, but with different kinds of accessors (think
       tarball-based fetchers vs local/remote git accessors). */
    if (storePath != path) {
        panic(fmt(
            "hashed store path computed by the evaluator ('%1%') does not match what was computed when copying to the store ('%2%'), this is a bug",
            store->printStorePath(path),
            store->printStorePath(storePath)));
    }
}

void EvalState::ensureLazyPathsCopied(const NixStringContext & context)
{
    for (const auto & c : context)
        if (auto * o = std::get_if<NixStringContextElem::Opaque>(&c.raw))
            /* TODO: This could be done in parallel. */
            ensureLazyPathCopied(o->path);
}

} // namespace nix
