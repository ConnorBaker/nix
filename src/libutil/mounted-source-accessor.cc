#include "nix/util/mounted-source-accessor.hh"

#include <algorithm>
#include <iterator>
#include <tuple>

#include "nix/util/sync.hh"
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
    Sync<std::map<CanonPath, ref<SourceAccessor>>> mounts;

    MountedSourceAccessorImpl(std::map<CanonPath, ref<SourceAccessor>> _mounts)
        : mounts(std::move(_mounts))
    {
        displayPrefix.clear();

        // Currently we require a root filesystem. This could be relaxed.
        assert(mounts.readLock()->contains(CanonPath::root));
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
        /* CanonPath order makes all grafts through one child contiguous. */
        for (auto i = view.below.begin(); i != view.below.end();) {
            auto relative = i->first.removePrefix(path);
            std::string name(*relative.begin());
            auto child = path / name;
            auto next = std::find_if_not(i + 1, view.below.end(), [&](const auto & binding) {
                return binding.first.isWithin(child);
            });
            if (i->first != child || next != i + 1)
                result.insert_or_assign(name, tDirectory);
            else if (auto st = i->second->maybeLstat(CanonPath::root))
                result.insert_or_assign(name, st->type);
            else
                result.erase(name);
            i = next;
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
        auto state = mounts.readLock();
        for (auto prefix = path;; prefix.pop()) {
            if (auto mount = state->find(prefix); mount != state->end())
                return {mount->second, path.removePrefix(prefix), std::move(prefix)};
            assert(!prefix.isRoot());
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
        ResolvedTree result{std::move(accessor), std::move(subpath), {}};
        auto last = CanonPath::root;
        {
            auto state = mounts.readLock();
            auto next = state->upper_bound(path);
            auto before = std::prev(next); // The required root mount is at or before every query.
            if (before->first == mountPoint && (next == state->end() || !next->first.isWithin(mountPoint)))
                return result;
            for (auto i = next; i != state->end() && i->first.isWithin(path); ++i)
                result.below.emplace_back(*i);
            /* Shape needs no fallback observation; listing and naming need its contents. */
            if (!result.below.empty() && !needDirectoryContents)
                return result;
            /* Strict descendants of each query prefix form an ordered interval.
               If nonempty, it contains one of the query's two neighbours. */
            last = path;
            while ((next == state->end() || !next->first.isWithin(last))
                   && (before->first == last || !before->first.isWithin(last)))
                last.pop();
        }
        last = last.removePrefix(mountPoint);
        auto component = result.subpath.begin();
        for (auto prefix = CanonPath::root;; prefix.push(*component++)) {
            auto st = result.accessor->maybeLstat(prefix);
            result.hidden = !st || st->type != tDirectory;
            if (result.hidden || prefix == last)
                break;
        }
        return result;
    }

    void invalidateCache() override
    {
        /* This call invalidates the observed mounts. A reentrant mount is
           for the next call; no child callback runs under the map's lock. */
        std::vector<ref<SourceAccessor>> children;
        for (auto state = mounts.readLock(); const auto & [path, child] : *state)
            children.push_back(child);
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
        mounts.lock()->try_emplace(std::move(mountPoint), std::move(accessor));
    }

    std::shared_ptr<SourceAccessor> getMount(const CanonPath & mountPoint) override
    {
        auto state = mounts.readLock();
        auto i = state->find(mountPoint);
        return i != state->end() ? i->second.get_ptr() : nullptr;
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
