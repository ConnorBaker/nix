#include "nix/fetchers/input-cache.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/fetchers/registry.hh"
#include "nix/util/sync.hh"

namespace nix::fetchers {

namespace {

bool shouldCacheInput(const Settings & settings, const Input & input)
{
    /* Local source-backed unlocked inputs (e.g. a working tree path or
       fetchGit on a local repo without a fixed rev) are semantically mutable.
       Caching their resolved accessor/locked input by the original request
       silently reuses stale content across tracked dirty changes. */
    if (input.isLocked(settings))
        return true;

    if (input.getSourcePath().has_value())
        return false;

    if (auto url = maybeGetStrAttr(input.attrs, "url"))
        return !url->starts_with("file://");

    return true;
}

}

InputCache::CachedResult InputCache::getAccessor(
    const Settings & settings, Store & store, const Input & originalInput, UseRegistries useRegistries)
{
    const bool cacheOriginal = shouldCacheInput(settings, originalInput);
    auto fetched = cacheOriginal ? lookup(originalInput) : std::nullopt;
    Input resolvedInput = originalInput;

    if (!fetched) {
        if (originalInput.isDirect()) {
            auto [accessor, lockedInput] = originalInput.getAccessor(settings, store);
            fetched.emplace(CachedInput{.lockedInput = lockedInput, .accessor = accessor});
        } else {
            if (useRegistries != UseRegistries::No) {
                auto [res, extraAttrs] = lookupInRegistries(settings, store, originalInput, useRegistries);
                resolvedInput = std::move(res);
                const bool cacheResolved = shouldCacheInput(settings, resolvedInput);
                fetched = cacheResolved ? lookup(resolvedInput) : std::nullopt;
                if (!fetched) {
                    auto [accessor, lockedInput] = resolvedInput.getAccessor(settings, store);
                    fetched.emplace(
                        CachedInput{.lockedInput = lockedInput, .accessor = accessor, .extraAttrs = extraAttrs});
                }
                else {
                    // Registry metadata (notably `dir`) belongs to the alias
                    // resolution edge, not to the resolved input tree itself.
                    // Preserve the current lookup's extra attrs even when the
                    // underlying resolved tree came from cache.
                    fetched->extraAttrs = extraAttrs;
                }
                if (cacheResolved)
                    upsert(resolvedInput, *fetched);
            } else {
                throw Error(
                    "'%s' is an indirect flake reference, but registry lookups are not allowed",
                    originalInput.to_string());
            }
        }
        if (cacheOriginal)
            upsert(originalInput, *fetched);
    }

    debug("got tree '%s' from '%s'", fetched->accessor, fetched->lockedInput.to_string());

    return {fetched->accessor, resolvedInput, fetched->lockedInput, fetched->extraAttrs};
}

struct InputCacheImpl : InputCache
{
    Sync<std::map<Input, CachedInput>> cache_;

    std::optional<CachedInput> lookup(const Input & originalInput) const override
    {
        auto cache(cache_.readLock());
        auto i = cache->find(originalInput);
        if (i == cache->end())
            return std::nullopt;
        debug(
            "mapping '%s' to previously seen input '%s' -> '%s",
            originalInput.to_string(),
            i->first.to_string(),
            i->second.lockedInput.to_string());
        return i->second;
    }

    void upsert(Input key, CachedInput cachedInput) override
    {
        cache_.lock()->insert_or_assign(std::move(key), std::move(cachedInput));
    }

    void clear() override
    {
        cache_.lock()->clear();
        /* The workdir info cache has the same "per evaluation" lifetime
           as the input cache, so flush it here as well so that e.g.
           `:reload` in `nix repl` picks up changes in git work trees. */
        GitRepo::invalidateWorkdirInfoCache();
    }
};

ref<InputCache> InputCache::create()
{
    return make_ref<InputCacheImpl>();
}

} // namespace nix::fetchers
