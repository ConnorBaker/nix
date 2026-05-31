#include "nix/util/mounted-source-accessor.hh"

#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix {

/**
 * The mutable face of `Switch`: a `boost::concurrent_flat_map` of
 * mounts that grows at runtime via `mount()`. All resolve + strip +
 * dispatch is inherited from `SwitchSourceAccessor`; this impl only
 * supplies the mount storage, the runtime mutator, and the exact-key
 * lookup `getMount` (O(1) — relied on as a membership test by
 * `EvalState::ensureLazyPathCopied` and friends, so it must stay a
 * direct lookup, never a prefix scan).
 */
struct MountedSourceAccessorImpl : MountedSourceAccessor
{
    boost::concurrent_flat_map<CanonPath, ref<SourceAccessor>> mounts;

    MountedSourceAccessorImpl(std::map<CanonPath, ref<SourceAccessor>> _mounts)
    {
        displayPrefix.clear();

        // Currently we require a root filesystem. This could be relaxed.
        assert(_mounts.contains(CanonPath::root));

        for (auto & [path, accessor] : _mounts)
            mount(path, accessor);

        // FIXME: return dummy parent directories automatically?
    }

    void mount(CanonPath mountPoint, ref<SourceAccessor> accessor) override
    {
        /* `insert_or_assign`, not `emplace`: mounting a key establishes
           that accessor at the key, overwriting any prior mount. For
           content-keyed real store paths this is observably identical
           to `emplace` (the same key implies the same content, hence an
           equivalent accessor). It matters for Item 2's identity-keyed
           deferred-mount stand-ins (§6.1.1): after a `resetFileCache`
           (REPL `:reload`, `nix_flake_lock`) the on-disk content may
           have moved while the (attrs-stable) fake key is unchanged, so
           a re-mint must replace the now-stale accessor rather than be
           silently dropped. */
        mounts.insert_or_assign(std::move(mountPoint), std::move(accessor));
    }

    std::shared_ptr<SourceAccessor> getMount(CanonPath mountPoint) override
    {
        if (auto res = getConcurrent(mounts, mountPoint))
            return *res;
        else
            return nullptr;
    }

    void forEachMountUnder(const CanonPath & subpath, fun<void(const CanonPath &, SourceAccessor &)> fn) override
    {
        /* Enumerate sub-mounts so an exhaustive `prefetchSubtree`
           reaches them (see `SwitchSourceAccessor::prefetchSubtree`).
           `visit_all` is the concurrent-map iteration primitive. */
        mounts.visit_all([&](auto & kv) {
            if (kv.first != subpath && kv.first.isWithin(subpath))
                fn(kv.first, *kv.second);
        });
    }

    void invalidateCache() override
    {
        mounts.visit_all([](auto & kv) { kv.second->invalidateCache(); });
    }
};

ref<MountedSourceAccessor> makeMountedSourceAccessor(std::map<CanonPath, ref<SourceAccessor>> mounts)
{
    return make_ref<MountedSourceAccessorImpl>(std::move(mounts));
}

} // namespace nix
