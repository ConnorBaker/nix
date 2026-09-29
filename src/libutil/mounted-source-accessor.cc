#include "nix/util/mounted-source-accessor.hh"

#include <algorithm>
#include <tuple>

#include <boost/unordered/concurrent_flat_map.hpp>
#include <nlohmann/json.hpp>

namespace nix {

namespace {

/**
 * A tree with other trees grafted onto it at mount points.  A graft is
 * authoritative: below a mount point only the mounted tree is consulted.
 * A mount point is listed in its parent directory, and the directories
 * leading to it replace absent or non-directory ancestors, so the
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
        auto keepAlive = weak_from_this().lock();
        auto view = resolveTree(path);
        if (!view.below.empty())
            throw NotARegularFile("file '%s' is not a regular file", path.abs());
        if (view.hidden)
            throw FileNotFound("file '%s' does not exist", path.abs());
        return view.accessor->readFile(view.subpath, sink, std::move(sizeCallback));
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        auto keepAlive = weak_from_this().lock();
        auto view = resolveTree(path);
        if (!view.below.empty())
            return Stat{.type = tDirectory};
        if (view.hidden)
            return std::nullopt;
        return view.accessor->maybeLstat(view.subpath);
    }

    Stat lstat(const CanonPath & path) override
    {
        auto keepAlive = weak_from_this().lock();
        auto view = resolveTree(path);
        if (!view.below.empty())
            return Stat{.type = tDirectory};
        if (view.hidden)
            throw FileNotFound("file '%s' does not exist", path.abs());
        /* Unchanged subtrees retain their own errors, including explanations
           from access-control accessors. */
        return view.accessor->lstat(view.subpath);
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        auto keepAlive = weak_from_this().lock();
        auto view = resolveTree(path, /*needDirectoryContents=*/true);
        if (view.below.empty()) {
            if (view.hidden)
                throw FileNotFound("directory '%s' does not exist", path.abs());
            return view.accessor->readDirectory(view.subpath);
        }
        DirEntries result;
        if (!view.hidden)
            result = view.accessor->readDirectory(view.subpath);
        /* Resolve child shapes before asking for mounted root types. A
           deeper binding already proves its first component is a directory,
           even if a direct mount at that component cannot be inspected. */
        for (auto & [mountPoint, mounted] : view.below) {
            auto rel = mountPoint.removePrefix(path);
            std::string name(*rel.begin());
            if (rel.parent()->isRoot())
                result.erase(name);
            else
                result.insert_or_assign(name, tDirectory);
        }
        for (auto & [mountPoint, mounted] : view.below) {
            auto rel = mountPoint.removePrefix(path);
            if (rel.parent()->isRoot()) {
                std::string name(*rel.begin());
                if (!result.contains(name))
                    if (auto st = mounted->maybeLstat(CanonPath::root))
                        result.emplace(name, st->type);
            }
        }
        return result;
    }

    std::string readLink(const CanonPath & path) override
    {
        auto keepAlive = weak_from_this().lock();
        auto view = resolveTree(path);
        if (!view.below.empty())
            throw NotASymlink("file '%s' is not a symbolic link", path.abs());
        if (view.hidden)
            throw FileNotFound("file '%s' does not exist", path.abs());
        return view.accessor->readLink(view.subpath);
    }

    std::string showPath(const CanonPath & path) override
    {
        auto keepAlive = weak_from_this().lock();
        auto [accessor, subpath, mountPoint] = resolve(path);
        return displayPrefix + accessor->showPath(subpath) + displaySuffix;
    }

    std::tuple<ref<SourceAccessor>, CanonPath, CanonPath> resolve(const CanonPath & path)
    {
        // Find the nearest parent of `path` that is a mount point.
        auto prefix = path;
        while (true) {
            if (auto mount = getConcurrent(mounts, prefix))
                return {std::move(*mount), path.removePrefix(prefix), std::move(prefix)};

            assert(!prefix.isRoot());
            prefix.pop();
        }
    }

    struct ResolvedTree
    {
        ref<SourceAccessor> accessor;
        CanonPath subpath;
        std::vector<std::pair<CanonPath, ref<SourceAccessor>>> below;
        bool hidden = false;
    };

    ResolvedTree resolveTree(const CanonPath & path, bool needDirectoryContents = false)
    {
        auto [accessor, subpath, mountPoint] = resolve(path);
        auto nested = mountsBelow(mountPoint);
        ResolvedTree result{std::move(accessor), std::move(subpath), {}};
        if (nested.empty())
            return result;
        for (auto & binding : nested)
            if (binding.first != path && binding.first.isWithin(path))
                result.below.push_back(binding);

        /* A descendant binding proves the directory's shape without forcing
           the fallback. Only listing and naming need its surviving contents. */
        if (!result.below.empty() && !needDirectoryContents)
            return result;

        /* Grafting through a non-directory discards its old subtree. Inspect
           only prefixes required by an explicit descendant; once our path
           diverges, the remaining original subtree can be delegated whole.
           The owning snapshot is complete before invoking any child. */
        auto prefix = mountPoint;
        auto component = result.subpath.begin();
        while (std::any_of(nested.begin(), nested.end(), [&](auto & binding) {
            return binding.first != prefix && binding.first.isWithin(prefix);
        })) {
            auto st = result.accessor->maybeLstat(prefix.removePrefix(mountPoint));
            if (!st || st->type != tDirectory) {
                result.hidden = true;
                break;
            }
            if (prefix == path)
                break;
            prefix.push(*component++);
        }
        return result;
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
        /* This call invalidates the observed mounts. A reentrant mount is
           for the next call; no child callback runs under the map's lock. */
        std::vector<ref<SourceAccessor>> children;
        mounts.visit_all([&](auto & kv) { children.push_back(kv.second); });
        for (auto & child : children)
            child->invalidateCache();
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        auto keepAlive = weak_from_this().lock();
        auto view = resolveTree(path);
        if (view.hidden || !view.below.empty())
            return std::nullopt;
        return view.accessor->getPhysicalPath(view.subpath);
    }

    void mount(CanonPath mountPoint, ref<SourceAccessor> accessor) override
    {
        /* Insert only: a fact already handed out about this tree must stay
           true, so a mount is never replaced. */
        mounts.emplace(std::move(mountPoint), std::move(accessor));
    }

    std::shared_ptr<SourceAccessor> getMount(const CanonPath & mountPoint) override
    {
        if (auto res = getConcurrent(mounts, mountPoint))
            return *res;
        else
            return nullptr;
    }

    std::optional<merkle::TreeEntry> getKnownEntry(const CanonPath & path) override
    {
        auto keepAlive = weak_from_this().lock();
        auto view = resolveTree(path);
        if (view.hidden || !view.below.empty())
            return std::nullopt;
        return view.accessor->getKnownEntry(view.subpath);
    }

    std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & path) override
    {
        auto keepAlive = weak_from_this().lock();
        if (fingerprint)
            return std::pair{path, *fingerprint};
        auto view = resolveTree(path, /*needDirectoryContents=*/true);
        if (view.below.empty()) {
            if (view.hidden)
                return std::nullopt;
            return view.accessor->getFingerprint(view.subpath);
        }
        /* A promoted ancestor contributes an empty directory, independent of
           the discarded source node. Surviving fallback contents must be
           named, as must every explicit graft. */
        nlohmann::json tree = nullptr;
        if (!view.hidden) {
            auto name = view.accessor->getFingerprint(view.subpath);
            if (!name)
                return std::nullopt;
            tree = {name->second, name->first.abs()};
        }
        auto grafts = nlohmann::json::array();
        for (auto & [mountPoint, mounted] : view.below) {
            auto graft = mounted->getFingerprint(CanonPath::root);
            if (!graft)
                return std::nullopt;
            grafts.push_back({mountPoint.removePrefix(path).abs(), graft->second, graft->first.abs()});
        }
        /* v1 assumed directory ancestors and could name a file even when a
           descendant was mounted. Its names cannot identify this total graft. */
        return std::pair{
            CanonPath::root, nlohmann::json::array({"graft-v2", std::move(tree), std::move(grafts)}).dump()};
    }
};

} // namespace

MountedSourceAccessor::~MountedSourceAccessor() {}

ref<MountedSourceAccessor> makeMountedSourceAccessor(std::map<CanonPath, ref<SourceAccessor>> mounts)
{
    return make_ref<MountedSourceAccessorImpl>(std::move(mounts));
}

} // namespace nix
