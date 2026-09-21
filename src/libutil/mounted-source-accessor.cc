#include "nix/util/mounted-source-accessor.hh"

#include <algorithm>

#include <boost/unordered/concurrent_flat_map.hpp>
#include <nlohmann/json.hpp>

namespace nix {

namespace {

/**
 * A tree with other trees grafted onto it at mount points.  A graft is
 * authoritative: below a mount point only the mounted tree is consulted.
 * A mount point is listed in its parent directory, and the directories
 * leading to it exist even where the underlying tree lacks them, so the
 * result is a tree.
 */
struct MountedSourceAccessorImpl : MountedSourceAccessor
{
private:
    void anchor() override {};

public:
    boost::concurrent_flat_map<CanonPath, ref<SourceAccessor>> mounts;

    MountedSourceAccessorImpl(std::map<CanonPath, ref<SourceAccessor>> _mounts)
    {
        displayPrefix.clear();

        // Currently we require a root filesystem. This could be relaxed.
        assert(_mounts.contains(CanonPath::root));

        for (auto & [path, accessor] : _mounts)
            mount(path, accessor);
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        auto [accessor, subpath] = resolve(path);
        return accessor->readFile(subpath, sink, sizeCallback);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        auto [accessor, subpath] = resolve(path);
        if (auto st = accessor->maybeLstat(subpath))
            return st;
        /* A mount point below `path` implies the directories leading to it. */
        if (!mountsBelow(path).empty())
            return Stat{.type = tDirectory};
        return std::nullopt;
    }

    Stat lstat(const CanonPath & path) override
    {
        auto [accessor, subpath] = resolve(path);
        if (mountsBelow(path).empty())
            /* The resolved tree answers, and for a missing path raises its own
               error, which may say more than "does not exist" (an access
               control accessor says why). */
            return accessor->lstat(subpath);
        if (auto st = accessor->maybeLstat(subpath))
            return *st;
        return Stat{.type = tDirectory};
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        auto [accessor, subpath] = resolve(path);
        auto below = mountsBelow(path);
        auto st = accessor->maybeLstat(subpath);
        if (below.empty() || (st && st->type != tDirectory))
            /* Nothing grafted below: the resolved tree's listing, or its
               error for a non-directory or a missing path. */
            return accessor->readDirectory(subpath);
        DirEntries result;
        if (st)
            result = accessor->readDirectory(subpath);
        for (auto & [mountPoint, mounted] : below) {
            auto rel = mountPoint.removePrefix(path);
            std::string name(*rel.begin());
            if (rel.parent()->isRoot()) {
                /* A direct child: the mounted tree replaces whatever the
                   resolved tree has there, so its root's type is the entry's. */
                auto mst = mounted->maybeLstat(CanonPath::root);
                result.insert_or_assign(name, mst ? std::optional{mst->type} : std::nullopt);
            } else
                /* A directory on the way to a deeper mount point; the resolved
                   tree's own entry, a directory by the graft precondition, is kept. */
                result.emplace(name, tDirectory);
        }
        return result;
    }

    std::string readLink(const CanonPath & path) override
    {
        auto [accessor, subpath] = resolve(path);
        return accessor->readLink(subpath);
    }

    std::string showPath(const CanonPath & path) override
    {
        auto [accessor, subpath] = resolve(path);
        return displayPrefix + accessor->showPath(subpath) + displaySuffix;
    }

    std::pair<ref<SourceAccessor>, CanonPath> resolve(CanonPath path)
    {
        // Find the nearest parent of `path` that is a mount point.
        std::vector<std::string> subpath;
        while (true) {
            if (auto mount = getMount(path)) {
                std::reverse(subpath.begin(), subpath.end());
                return {ref(mount), CanonPath(subpath)};
            }

            assert(!path.isRoot());
            subpath.push_back(std::string(*path.baseName()));
            path.pop();
        }
    }

    /**
     * The mounts whose mount point lies strictly below `path`, in path order.
     */
    std::vector<std::pair<CanonPath, ref<SourceAccessor>>> mountsBelow(const CanonPath & path)
    {
        std::vector<std::pair<CanonPath, ref<SourceAccessor>>> res;
        mounts.visit_all([&](auto & kv) {
            if (kv.first != path && kv.first.isWithin(path))
                res.emplace_back(kv.first, kv.second);
        });
        std::sort(res.begin(), res.end(), [](auto & a, auto & b) { return a.first < b.first; });
        return res;
    }

    void invalidateCache() override
    {
        mounts.visit_all([](auto & kv) { kv.second->invalidateCache(); });
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        auto [accessor, subpath] = resolve(path);
        return accessor->getPhysicalPath(subpath);
    }

    void mount(CanonPath mountPoint, ref<SourceAccessor> accessor) override
    {
        /* Insert only: a fact already handed out about this tree must stay
           true, so a mount is never replaced. */
        mounts.emplace(std::move(mountPoint), std::move(accessor));
    }

    std::shared_ptr<SourceAccessor> getMount(CanonPath mountPoint) override
    {
        if (auto res = getConcurrent(mounts, mountPoint))
            return *res;
        else
            return nullptr;
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        if (fingerprint)
            return {path, fingerprint};
        auto [accessor, subpath] = resolve(path);
        auto below = mountsBelow(path);
        if (below.empty())
            /* Below a mount point, or away from all of them: the resolved
               tree's subtree is the subtree here. */
            return accessor->getFingerprint(subpath);
        /* Above a mount point the subtree is the resolved tree's subtree
           with the mounted trees grafted on.  It is named by the names of
           all of them, or not at all; the name is for this subtree exactly,
           hence the root path. */
        auto [tSub, tFp] = accessor->getFingerprint(subpath);
        if (!tFp)
            return {path, std::nullopt};
        auto grafts = nlohmann::json::array();
        for (auto & [mountPoint, mounted] : below) {
            auto [uSub, uFp] = mounted->getFingerprint(CanonPath::root);
            if (!uFp)
                return {path, std::nullopt};
            grafts.push_back({mountPoint.removePrefix(path).abs(), *uFp, uSub.abs()});
        }
        return {CanonPath::root, nlohmann::json::array({"graft", *tFp, tSub.abs(), std::move(grafts)}).dump()};
    }
};

} // namespace

MountedSourceAccessor::~MountedSourceAccessor() {}

ref<MountedSourceAccessor> makeMountedSourceAccessor(std::map<CanonPath, ref<SourceAccessor>> mounts)
{
    return make_ref<MountedSourceAccessorImpl>(std::move(mounts));
}

} // namespace nix
