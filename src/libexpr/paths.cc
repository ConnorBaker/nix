#include "nix/store/store-api.hh"
#include "nix/expr/eval.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/attrs.hh"

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
    return {rootFS, CanonPath{store->printStorePath(path)}};
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
        /* Force a copy. mountInput does a dryRun to just calculate the storePath and tree hash. */
        FetchMode::Copy,
        path.name());

    /* This can happen if the source gets modified by another process while we are evaluaing
       from it. Alternatively, the caching might be unsound and fetcher cache is poisoned somehow.
       See https://github.com/NixOS/nix/issues/14317. */
    if (storePath != path) {
        throw Error(
            (unsigned int) 102,
            "store path ('%1%') was hashed to avoid a full copy at first, but upon reading it again, the contents have changed ('%2%'), so we can not proceed. Make sure files do not change during evaluation",
            store->printStorePath(path),
            store->printStorePath(storePath));
    }
}

void EvalState::ensureLazyPathsCopied(const NixStringContext & context)
{
    for (const auto & c : context)
        if (auto * o = std::get_if<NixStringContextElem::Opaque>(&c.raw))
            /* TODO: This could be done in parallel. */
            ensureLazyPathCopied(o->path);
}

StorePath EvalState::mountInput(fetchers::Input & input, ref<SourceAccessor> accessor)
{
    /* A dry run names the tree: the store path, to know where to mount it,
       and the tree hash (doc/lazy-store/04-derivation.md, section 1.9). */
    auto [storePath, hash] = fetchToStore2(fetchSettings, *store, accessor, FetchMode::DryRun, input.getName());

    allowPath(storePath); // FIXME: should just whitelist the entire virtual store

    storeFS->mount(CanonPath(store->printStorePath(storePath)), accessor);

    /* `input` is what `Input::getAccessor` returned: whatever the original
       asserted has been verified there and the `narHash` replaced by
       `treeHash`.  The tree hash is set here too, for an input that asserted
       neither. */
    if (input.getNarHash())
        throw Error("input '%s' still carries a narHash after verification", input.to_string());
    input.attrs.insert_or_assign("treeHash", hash.to_string(HashFormat::SRI, true));

    return storePath;
}

} // namespace nix
