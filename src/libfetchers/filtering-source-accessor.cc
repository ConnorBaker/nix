#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/sync.hh"
#include "nix/util/util.hh"
#include "nix/util/hash.hh"

#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/unordered/concurrent_flat_set.hpp>
#include <iterator>
#include <nlohmann/json.hpp>

namespace nix {

std::optional<std::filesystem::path> FilteringSourceAccessor::getPhysicalPath(const CanonPath & path)
{
    checkAccess(path);
    return next->getPhysicalPath(prefix / path);
}

void FilteringSourceAccessor::readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback)
{
    checkAccess(path);
    return next->readFile(prefix / path, sink, sizeCallback);
}

bool FilteringSourceAccessor::pathExists(const CanonPath & path)
{
    return isAllowed(path) && next->pathExists(prefix / path);
}

std::optional<SourceAccessor::Stat> FilteringSourceAccessor::maybeLstat(const CanonPath & path)
{
    return isAllowed(path) ? next->maybeLstat(prefix / path) : std::nullopt;
}

SourceAccessor::Stat FilteringSourceAccessor::lstat(const CanonPath & path)
{
    checkAccess(path);
    return next->lstat(prefix / path);
}

SourceAccessor::DirEntries FilteringSourceAccessor::readDirectory(const CanonPath & path)
{
    checkAccess(path);
    DirEntries entries;
    for (auto & entry : next->readDirectory(prefix / path)) {
        if (isAllowed(path / entry.first))
            entries.insert(std::move(entry));
    }
    return entries;
}

void FilteringSourceAccessor::readDirectoryScoped(
    const CanonPath & path, std::function<void(SourceAccessor &, const CanonPath &, const DirEntries &)> callback)
{
    checkAccess(path);
    next->readDirectoryWithEntries(
        prefix / path, [&](SourceAccessor & inner, const CanonPath & relative, const DirEntries & entries) {
            DirEntries admitted;
            for (auto & entry : entries)
                if (isAllowed(path / entry.first))
                    admitted.insert(entry);

            struct Scoped : FilteringSourceAccessor
            {
                void anchor() override {}

                FilteringSourceAccessor * owner;
                CanonPath original;
                const DirEntries & admitted;

                Scoped(
                    SourceAccessor & inner,
                    const CanonPath & relative,
                    FilteringSourceAccessor & owner,
                    const CanonPath & original,
                    const DirEntries & admitted)
                    /* Borrowed only during readDirectoryWithEntries's scope.
                       No accessor or visitor may retain it after callback. */
                    : FilteringSourceAccessor(
                          SourcePath(
                              ref<SourceAccessor>(std::shared_ptr<SourceAccessor>(&inner, [](auto *) {})), relative),
                          [&owner, original](const CanonPath & p) { return owner.makeNotAllowedError(original / p); })
                    , owner(&owner)
                    , original(original)
                    , admitted(admitted)
                {
                    /* Nested scopes change coordinates, not the admission
                       policy. Compose those coordinates once instead of
                       rebuilding the path through every enclosing scope
                       for each descendant. The physical accessor remains
                       the innermost borrowed directory above. */
                    if (auto parent = dynamic_cast<Scoped *>(&owner)) {
                        this->original = parent->original / original;
                        this->owner = parent->owner;
                    }
                }

                bool isAllowed(const CanonPath & p) override
                {
                    if (p.isRoot())
                        return true;
                    if (p.parent()->isRoot())
                        return admitted.contains(std::string(*p.baseName()));
                    return owner->isAllowed(original / p);
                }

                std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & p) override
                {
                    return owner->getFingerprint(original / p);
                }

                void readDirectoryWithEntries(
                    const CanonPath & p,
                    std::function<void(SourceAccessor &, const CanonPath &, const DirEntries &)> callback) override
                {
                    readDirectoryScoped(p, std::move(callback));
                }
            } scoped{inner, relative, *this, path, admitted};
            callback(scoped, CanonPath::root, admitted);
        });
}

std::string FilteringSourceAccessor::readLink(const CanonPath & path)
{
    checkAccess(path);
    return next->readLink(prefix / path);
}

std::string FilteringSourceAccessor::showPath(const CanonPath & path)
{
    return displayPrefix + next->showPath(prefix / path) + displaySuffix;
}

std::pair<CanonPath, std::optional<std::string>> FilteringSourceAccessor::getFingerprint(const CanonPath & path)
{
    /* A filtered subtree is not the inner subtree, so the inner name does
       not name it.  A subclass that knows what it admits below `path`
       names the subtree (the fixed-set accessor below, the allow list
       under an allowed prefix); the rest give no name. */
    return {path, fingerprint};
}

void FilteringSourceAccessor::checkAccess(const CanonPath & path)
{
    if (!isAllowed(path))
        throw makeNotAllowedError(path);
}

struct AllowListSourceAccessorImpl : AllowListSourceAccessor
{
    void readDirectoryWithEntries(
        const CanonPath & path,
        std::function<void(SourceAccessor &, const CanonPath &, const DirEntries &)> callback) override
    {
        readDirectoryScoped(path, std::move(callback));
    }
private:
    void anchor() override {};
public:
    SharedSync<std::set<CanonPath>> allowedPrefixes;
    boost::concurrent_flat_set<CanonPath> allowedPaths;

    AllowListSourceAccessorImpl(
        ref<SourceAccessor> next,
        const std::set<CanonPath> & allowedPrefixes,
        const std::unordered_set<CanonPath> & allowedPaths,
        MakeNotAllowedError && makeNotAllowedError)
        : AllowListSourceAccessor(SourcePath(next), std::move(makeNotAllowedError))
        , allowedPrefixes(allowedPrefixes.begin(), allowedPrefixes.end())
        , allowedPaths(allowedPaths.begin(), allowedPaths.end())
    {
        /* Ancestor prefixes already admit every descendant prefix.  Keep
           just the minimal prefixes; CanonPath orders each subtree
           contiguously, so covered entries immediately follow its root. */
        auto prefixes = this->allowedPrefixes.lock();
        for (auto i = prefixes->begin(); i != prefixes->end();) {
            auto next = std::next(i);
            while (next != prefixes->end() && next->isWithin(*i))
                next = prefixes->erase(next);
            i = next;
        }
    }

    bool isAllowed(const CanonPath & path) override
    {
        /* Read lock is held for the duration of the full expression if the || doesn't short-circuit. */
        return allowedPaths.contains(path) || path.isAllowed(*allowedPrefixes.readLock());
    }

    void allowPrefix(CanonPath prefix) override
    {
        auto prefixes = allowedPrefixes.lock();
        auto after = prefixes->upper_bound(prefix);
        if (after != prefixes->begin() && prefix.isWithin(*std::prev(after)))
            return;
        auto inserted = prefixes->emplace_hint(after, std::move(prefix));
        auto first = after;
        while (after != prefixes->end() && after->isWithin(*inserted))
            ++after;
        prefixes->erase(first, after);
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        if (fingerprint)
            return {path, fingerprint};
        /* At or under an allowed prefix everything is admitted, so the
           subtree is the inner one and carries its name.  Elsewhere, at an
           ancestor of a prefix for instance, entries are hidden, and the
           subtree has no name. */
        auto prefixes(allowedPrefixes.readLock());
        auto after = prefixes->upper_bound(path);
        if (after != prefixes->begin() && path.isWithin(*std::prev(after)))
            return next->getFingerprint(prefix / path);
        return {path, std::nullopt};
    }
};

struct FixedSetFilteringSourceAccessor : FilteringSourceAccessor
{
    void readDirectoryWithEntries(
        const CanonPath & path,
        std::function<void(SourceAccessor &, const CanonPath &, const DirEntries &)> callback) override
    {
        readDirectoryScoped(path, std::move(callback));
    }
private:
    void anchor() override {};
public:
    const std::set<CanonPath> accepted;

    FixedSetFilteringSourceAccessor(const SourcePath & src, std::set<CanonPath> accepted)
        : FilteringSourceAccessor(
              src,
              [](const CanonPath & path) {
                  return RestrictedPathError("access to path '%s' is forbidden: it is filtered out", path);
              })
        , accepted(rebase(std::move(accepted), src.path))
    {
    }

    /**
     * The set is given in the inner accessor's coordinates, as
     * `filteredPaths()` returns it; this accessor is rooted at `prefix`.
     */
    static std::set<CanonPath> rebase(std::set<CanonPath> accepted, const CanonPath & prefix)
    {
        std::set<CanonPath> res;
        for (auto & p : accepted)
            if (p.isWithin(prefix))
                res.insert(p.removePrefix(prefix));
        return res;
    }

    bool isAllowed(const CanonPath & path) override
    {
        return accepted.contains(path);
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        if (fingerprint)
            return {path, fingerprint};
        auto [innerPath, innerName] = next->getFingerprint(prefix / path);
        if (!innerName)
            return {path, std::nullopt};
        /* The admitted set below `path`, relative to it, in canonical form:
           the NAR of the filtered subtree depends on nothing else beyond
           the inner subtree.  The name is for this subtree exactly, hence
           the root path. */
        std::string below;
        for (auto & p : accepted)
            if (p.isWithin(path)) {
                below += p.removePrefix(path).abs();
                below += '\0';
            }
        return {
            CanonPath::root,
            nlohmann::json::array({"filter",
                                   *innerName,
                                   innerPath.abs(),
                                   hashString(HashAlgorithm::SHA256, below).to_string(HashFormat::Nix32, false)})
                .dump()};
    }
};

ref<SourceAccessor> makeFixedSetFilteringSourceAccessor(const SourcePath & src, std::set<CanonPath> accepted)
{
    return make_ref<FixedSetFilteringSourceAccessor>(src, std::move(accepted));
}

ref<AllowListSourceAccessor> AllowListSourceAccessor::create(
    ref<SourceAccessor> next,
    const std::set<CanonPath> & allowedPrefixes,
    const std::unordered_set<CanonPath> & allowedPaths,
    MakeNotAllowedError && makeNotAllowedError)
{
    return make_ref<AllowListSourceAccessorImpl>(next, allowedPrefixes, allowedPaths, std::move(makeNotAllowedError));
}

CachingFilteringSourceAccessor::CachingFilteringSourceAccessor(
    const SourcePath & src, MakeNotAllowedError && makeNotAllowedError)
    : FilteringSourceAccessor(src, std::move(makeNotAllowedError))
    , cache(make_ref<boost::concurrent_flat_map<CanonPath, bool>>())
{
}

bool CachingFilteringSourceAccessor::isAllowed(const CanonPath & path)
{
    if (auto allowed = getConcurrent(*cache, path))
        return *allowed;
    auto res = isAllowedUncached(path);
    cache->emplace(path, res);
    return res;
}

} // namespace nix
