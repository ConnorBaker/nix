#include "nix/util/source-accessor.hh"

#include <nlohmann/json.hpp>

namespace nix {

namespace {

/**
 * The union of trees, defined top-down as an overlay is: at each path the
 * first child that has a node decides the type; a directory is the merge of
 * the directories every child has there, earlier children winning name
 * collisions; a non-directory hides the later children's subtrees at and
 * below it.
 */
struct UnionSourceAccessor : SourceAccessor
{
private:
    void anchor() override {};

public:
    std::vector<ref<SourceAccessor>> accessors;

    UnionSourceAccessor(std::vector<ref<SourceAccessor>> _accessors)
        : accessors(std::move(_accessors))
    {
        displayPrefix.clear();
    }

    /**
     * A child's node at `path`, or nothing.  A path that passes through a
     * symlink has no node in a tree, whatever the accessor does about it.
     */
    static std::optional<Stat> node(SourceAccessor & accessor, const CanonPath & path)
    {
        try {
            return accessor.maybeLstat(path);
        } catch (SymlinkNotAllowed &) {
            return std::nullopt;
        }
    }

    /**
     * Whether an earlier layer hides the candidate owner's node at `path`.
     * A directory in layer j hides later layers' non-directories at every
     * ancestor above it. Only layers before j can still hide this path.
     */
    bool hides(size_t before, const CanonPath & path)
    {
        if (!before || path.isRoot())
            return false;
        auto p = path;
        while (before && !p.isRoot()) {
            p.pop();
            for (size_t j = 0; j < before; ++j)
                if (auto st = node(*accessors[j], p)) {
                    if (st->type != tDirectory)
                        return true;
                    before = j;
                    break;
                }
        }
        return false;
    }

    /**
     * The child whose node `path` denotes in the union, with that node; or
     * nothing if `path` is not in the union.
     */
    std::optional<std::pair<ref<SourceAccessor>, Stat>> owner(const CanonPath & path)
    {
        for (size_t i = 0; i < accessors.size(); ++i) {
            if (auto st = node(*accessors[i], path)) {
                if (hides(i, path))
                    return std::nullopt;
                return std::pair{accessors[i], *st};
            }
        }
        return std::nullopt;
    }

    ref<SourceAccessor> requireOwner(const CanonPath & path)
    {
        auto keepAlive = weak_from_this().lock();
        auto o = owner(path);
        if (!o)
            throw FileNotFound("path '%s' does not exist", showPath(path));
        return std::move(o->first);
    }

    /**
     * The children after `own` that have a directory at `path` and so
     * contribute entries to the union's directory there.  Children before
     * `own` have no node at `path` and do not hide it, or `own` would not
     * own it; a later child's non-directory at `path` or at an ancestor is
     * hidden by `own`'s directories.
     */
    std::vector<ref<SourceAccessor>> laterDirectories(const CanonPath & path, const SourceAccessor & own)
    {
        std::vector<ref<SourceAccessor>> res;
        bool after = false;
        for (auto & accessor : accessors) {
            if (after) {
                auto st = node(*accessor, path);
                if (st && st->type == tDirectory)
                    res.push_back(accessor);
            } else if (*accessor == own)
                after = true;
        }
        return res;
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        requireOwner(path)->readFile(path, sink, sizeCallback);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        auto keepAlive = weak_from_this().lock();
        auto o = owner(path);
        return o ? std::optional{o->second} : std::nullopt;
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        auto keepAlive = weak_from_this().lock();
        auto o = owner(path);
        if (!o)
            throw FileNotFound("path '%s' does not exist", showPath(path));
        auto & [own, st] = *o;
        if (st.type != tDirectory)
            /* Let the owner raise its usual error for a non-directory. */
            return own->readDirectory(path);
        DirEntries result = own->readDirectory(path);
        for (auto & accessor : laterDirectories(path, *own))
            for (auto & entry : accessor->readDirectory(path))
                /* Earlier children win name collisions. */
                result.insert(entry);
        return result;
    }

    std::string readLink(const CanonPath & path) override
    {
        return requireOwner(path)->readLink(path);
    }

    std::string showPath(const CanonPath & path) override
    {
        if (accessors.empty())
            return SourceAccessor::showPath(path);
        auto first = accessors.front();
        return first->showPath(path);
    }

    void invalidateCache() override
    {
        auto keepAlive = weak_from_this().lock();
        for (auto & accessor : accessors)
            accessor->invalidateCache();
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        auto keepAlive = weak_from_this().lock();
        for (auto & accessor : accessors) {
            auto p = accessor->getPhysicalPath(path);
            if (p)
                return p;
        }
        return std::nullopt;
    }

    std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & path) override
    {
        if (fingerprint)
            return std::pair{path, *fingerprint};

        auto keepAlive = weak_from_this().lock();
        auto o = owner(path);
        if (!o)
            return std::nullopt;
        auto & [own, st] = *o;
        auto later = st.type == tDirectory ? laterDirectories(path, *own) : std::vector<ref<SourceAccessor>>{};
        /* Selection borrows the layers; naming now owns every part. */
        keepAlive.reset();
        if (later.empty())
            /* The subtree here is one child's alone. */
            return own->getFingerprint(path);

        /* A merged directory is named by the names of its parts, or not at
           all.  The name is for this subtree exactly, hence the root path. */
        auto parts = nlohmann::json::array();
        auto add = [&](SourceAccessor & accessor) {
            auto identity = accessor.getFingerprint(path);
            if (!identity)
                return false;
            parts.push_back({identity->second, identity->first.abs()});
            return true;
        };
        if (!add(*own))
            return std::nullopt;
        for (auto & accessor : later)
            if (!add(*accessor))
                return std::nullopt;
        return std::pair{CanonPath::root, nlohmann::json::array({"union", std::move(parts)}).dump()};
    }
};

} // namespace

ref<SourceAccessor> makeUnionSourceAccessor(std::vector<ref<SourceAccessor>> && accessors)
{
    return make_ref<UnionSourceAccessor>(std::move(accessors));
}

} // namespace nix
