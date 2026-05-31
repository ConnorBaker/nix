#include "nix/util/switch-source-accessor.hh"

namespace nix {

std::pair<ref<SourceAccessor>, CanonPath> SwitchSourceAccessor::resolveMount(CanonPath path)
{
    /* Walk up from `path` to the nearest ancestor that is a mount
       point; the matching accessor is authoritative for the whole
       subtree below it. `mountPoint` is returned (rather than the
       pre-stripped subpath) so each method computes
       `path.removePrefix(mountPoint)` itself — the same strip
       `StripPrefix(mountPoint)` would apply. */
    auto cur = path;
    while (true) {
        if (auto mount = getMount(cur))
            return {ref(mount), cur};
        if (cur.isRoot())
            /* No mount matched, not even at the root. A Switch is
               REQUIRED to have a root mount (so every path resolves and
               this loop terminates) — `makeSwitch`/the ctor enforce it,
               but that enforcement is an `assert`, compiled out in
               release builds. Throw unconditionally here so a Switch
               built without a root mount fails loudly (and `showPath`,
               which routes through here when formatting an error, can't
               spin) instead of looping in a release build. */
            throw Error("internal error: SwitchSourceAccessor has no root mount; cannot resolve path '%s'", path);
        cur.pop();
    }
}

void SwitchSourceAccessor::readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback)
{
    auto [accessor, mountPoint] = resolveMount(path);
    return accessor->readFile(path.removePrefix(mountPoint), sink, sizeCallback);
}

SourceAccessor::Stat SwitchSourceAccessor::lstat(const CanonPath & path)
{
    auto [accessor, mountPoint] = resolveMount(path);
    return accessor->lstat(path.removePrefix(mountPoint));
}

std::optional<SourceAccessor::Stat> SwitchSourceAccessor::maybeLstat(const CanonPath & path)
{
    auto [accessor, mountPoint] = resolveMount(path);
    return accessor->maybeLstat(path.removePrefix(mountPoint));
}

SourceAccessor::DirEntries SwitchSourceAccessor::readDirectory(const CanonPath & path)
{
    auto [accessor, mountPoint] = resolveMount(path);
    return accessor->readDirectory(path.removePrefix(mountPoint));
}

std::string SwitchSourceAccessor::readLink(const CanonPath & path)
{
    auto [accessor, mountPoint] = resolveMount(path);
    return accessor->readLink(path.removePrefix(mountPoint));
}

std::string SwitchSourceAccessor::showPath(const CanonPath & path)
{
    auto [accessor, mountPoint] = resolveMount(path);
    return displayPrefix + accessor->showPath(path.removePrefix(mountPoint)) + displaySuffix;
}

std::optional<std::filesystem::path> SwitchSourceAccessor::getPhysicalPath(const CanonPath & path)
{
    auto [accessor, mountPoint] = resolveMount(path);
    return accessor->getPhysicalPath(path.removePrefix(mountPoint));
}

void SwitchSourceAccessor::prefetchSubtree(const CanonPath & subpath, unsigned depth)
{
    /* The accessor OWNING `subpath` (nearest ancestor mount) prefetches
       `subpath` and everything down to any deeper mount point. */
    auto [accessor, mountPoint] = resolveMount(subpath);
    accessor->prefetchSubtree(subpath.removePrefix(mountPoint), depth);

    /* Mounts STRICTLY BELOW `subpath` are distinct accessors the owner
       can't see (e.g. Git submodules mounted at `/sub`). An exhaustive
       prefetch (`subpath` = root, depth = max — the only real caller,
       the materialisation/force walk) must reach them too, or their
       blobs fall back to the slow per-blob on-demand path. Fan out to
       each. We pass the SAME `depth` from the sub-mount's own root
       rather than discounting the prefix descent: it can only
       over-approximate the set to prefetch (harmless — same as the
       filtered-Subset over-fetch), and for the unbounded exhaustive
       case it is exactly right. No-op when the concrete Switch doesn't
       enumerate (default `forEachMountUnder`). */
    forEachMountUnder(subpath, [&](const CanonPath &, SourceAccessor & sub) {
        sub.prefetchSubtree(CanonPath::root, depth);
    });
}

std::pair<CanonPath, std::optional<std::string>> SwitchSourceAccessor::getFingerprint(const CanonPath & path)
{
    auto [accessor, mountPoint] = resolveMount(path);
    /* Same Layer-2 profile as `StripPrefix(mountPoint)`: empty
       `computeOwnSuffix` + unset `fingerprint`, threading the stripped
       inner path. (If anyone ever sets `fingerprint` on a Switch, this
       equivalence breaks — keep it unset.) */
    return composeFingerprint(*this, *accessor, path, path.removePrefix(mountPoint));
}

namespace {

/**
 * Immutable `Switch`: a fixed `std::map` of mounts, looked up by exact
 * key in `getMount`. No runtime mutation; for the mutable face see
 * `MountedSourceAccessorImpl` in `mounted-source-accessor.cc`.
 */
struct ImmutableSwitchSourceAccessor final : SwitchSourceAccessor
{
    std::map<CanonPath, ref<SourceAccessor>> mounts;

    ImmutableSwitchSourceAccessor(std::map<CanonPath, ref<SourceAccessor>> _mounts)
        : mounts(std::move(_mounts))
    {
        displayPrefix.clear();

        // A root filesystem is required so every path resolves.
        assert(mounts.contains(CanonPath::root));
    }

    std::shared_ptr<SourceAccessor> getMount(CanonPath mountPoint) override
    {
        auto i = mounts.find(mountPoint);
        return i == mounts.end() ? nullptr : i->second.get_ptr();
    }

    void forEachMountUnder(const CanonPath & subpath, fun<void(const CanonPath &, SourceAccessor &)> fn) override
    {
        for (auto & [mp, accessor] : mounts)
            if (mp != subpath && mp.isWithin(subpath)) // strict descendant of subpath
                fn(mp, *accessor);
    }

    void invalidateCache() override
    {
        for (auto & [_, accessor] : mounts)
            accessor->invalidateCache();
    }
};

} // namespace

ref<SourceAccessor> makeSwitch(std::map<CanonPath, ref<SourceAccessor>> mounts)
{
    return make_ref<ImmutableSwitchSourceAccessor>(std::move(mounts));
}

} // namespace nix
