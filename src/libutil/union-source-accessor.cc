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
    UnionCoherence coherence;

    UnionSourceAccessor(std::vector<ref<SourceAccessor>> _accessors, UnionCoherence coherence)
        : accessors(std::move(_accessors))
        , coherence(coherence)
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
     * Whether `accessor` hides `path`: it has a non-directory node at a
     * proper ancestor of `path`, so nothing below that node exists in the
     * union, whatever later children contain.
     */
    static bool hides(SourceAccessor & accessor, const CanonPath & path)
    {
        auto p = path;
        while (!p.isRoot()) {
            p.pop();
            if (auto st = node(accessor, p))
                return st->type != tDirectory;
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
                /* Children that agree wherever both have a node cannot
                   hide one another, so the first node is the answer and
                   the ancestors need no probing (the evaluation root:
                   doc/lazy-store/01-specification.md, section 8.4). */
                if (coherence != UnionCoherence::ChildrenAgree)
                    for (size_t j = 0; j < i; ++j)
                        if (hides(*accessors[j], path))
                            return std::nullopt;
                return std::pair{accessors[i], *st};
            }
        }
        return std::nullopt;
    }

    ref<SourceAccessor> requireOwner(const CanonPath & path)
    {
        auto o = owner(path);
        if (!o)
            throw FileNotFound("path '%s' does not exist", showPath(path));
        return o->first;
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
        auto o = owner(path);
        return o ? std::optional{o->second} : std::nullopt;
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
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
        for (auto & accessor : accessors)
            return accessor->showPath(path);
        return SourceAccessor::showPath(path);
    }

    void invalidateCache() override
    {
        for (auto & accessor : accessors)
            accessor->invalidateCache();
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        for (auto & accessor : accessors) {
            auto p = accessor->getPhysicalPath(path);
            if (p)
                return p;
        }
        return std::nullopt;
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        if (fingerprint)
            return {path, fingerprint};

        if (coherence == UnionCoherence::ChildrenAgree) {
            /* Every child's subtree here is the union's subtree, so any
               child's name for it will do. */
            for (auto & accessor : accessors) {
                auto [subpath, fp] = accessor->getFingerprint(path);
                if (fp)
                    return {subpath, fp};
            }
            return {path, std::nullopt};
        }

        auto o = owner(path);
        if (!o)
            return {path, std::nullopt};
        auto & [own, st] = *o;
        auto later = st.type == tDirectory ? laterDirectories(path, *own) : std::vector<ref<SourceAccessor>>{};
        if (later.empty())
            /* The subtree here is one child's alone. */
            return own->getFingerprint(path);

        /* A merged directory is named by the names of its parts, or not at
           all.  The name is for this subtree exactly, hence the root path. */
        auto parts = nlohmann::json::array();
        auto add = [&](SourceAccessor & accessor) {
            auto [subpath, fp] = accessor.getFingerprint(path);
            if (!fp)
                return false;
            parts.push_back({*fp, subpath.abs()});
            return true;
        };
        if (!add(*own))
            return {path, std::nullopt};
        for (auto & accessor : later)
            if (!add(*accessor))
                return {path, std::nullopt};
        return {CanonPath::root, nlohmann::json::array({"union", std::move(parts)}).dump()};
    }
};

} // namespace

ref<SourceAccessor> makeUnionSourceAccessor(std::vector<ref<SourceAccessor>> && accessors, UnionCoherence coherence)
{
    return make_ref<UnionSourceAccessor>(std::move(accessors), coherence);
}

} // namespace nix
