#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/sync.hh"
#include "nix/util/util.hh"
#include "nix/util/hash.hh"

#include <boost/unordered/concurrent_flat_map.hpp>
#include <iterator>
#include <nlohmann/json.hpp>

namespace nix {

std::optional<std::filesystem::path> FilteringSourceAccessor::getPhysicalPath(const CanonPath & path)
{
    auto owner = checkAllowed(path);
    auto innerPath = prefix / path;
    /* Admission of a directory says nothing about its descendants. A
       physical directory would bypass their filtering policy. */
    if (auto st = next->maybeLstat(innerPath); st && st->type != tDirectory)
        return next->getPhysicalPath(innerPath);
    return std::nullopt;
}

bool FilteringSourceAccessor::pathExists(const CanonPath & path)
{
    auto owner = weak_from_this().lock();
    if (!isAllowed(path))
        return false;
    return next->pathExists(prefix / path);
}

std::optional<SourceAccessor::Stat> FilteringSourceAccessor::maybeLstat(const CanonPath & path)
{
    auto owner = weak_from_this().lock();
    if (!isAllowed(path))
        return std::nullopt;
    return next->maybeLstat(prefix / path);
}

SourceAccessor::Stat FilteringSourceAccessor::lstat(const CanonPath & path)
{
    return withSource(path, &SourceAccessor::lstat);
}

SourceAccessor::DirEntries FilteringSourceAccessor::readDirectory(const CanonPath & path)
{
    auto owner = checkAllowed(path);
    auto entries = next->readDirectory(prefix / path);
    std::erase_if(entries, [&](const auto & entry) { return !isAllowed(path / entry.first); });
    return entries;
}

void FilteringSourceAccessor::readDirectoryScoped(const CanonPath & path, ReadDirectoryCallback callback)
{
    /* The scoped view borrows this policy throughout the callback. Stack
       scopes are already kept alive by their enclosing directory call. */
    auto owner = checkAllowed(path);
    next->readDirectoryWithEntries(prefix / path, [&](auto & inner, const auto & relative, auto && listing) {
        struct Scoped final : FilteringSourceAccessor
        {
            void anchor() override {}

            FilteringSourceAccessor * owner;
            CanonPath original;
            const DirEntries admitted;

            Scoped(
                SourceAccessor & inner,
                const CanonPath & relative,
                FilteringSourceAccessor & owner,
                const CanonPath & path,
                const Scoped * parent,
                DirectoryListing && listing)
                /* Borrowed only during readDirectoryWithEntries's scope.
                   No accessor or visitor may retain it after callback. */
                : FilteringSourceAccessor(
                      SourcePath(
                          ref<SourceAccessor>(
                              std::shared_ptr<SourceAccessor>(std::shared_ptr<SourceAccessor>{}, &inner)),
                          relative),
                      [this](const CanonPath & p) { return this->owner->makeNotAllowedError(this->original / p); })
                , owner(parent ? parent->owner : &owner)
                , original(parent ? parent->original / path : path)
                , admitted(std::move(listing).filter([&](const auto & entry) {
                    return this->owner->isAllowed(this->original / entry.first);
                }))
            {
            }

            bool isAllowed(const CanonPath & p) override
            {
                if (p.isRoot())
                    return true;
                auto relative = p.rel();
                if (relative.find('/') == std::string_view::npos)
                    return admitted.contains(relative);
                return owner->isAllowed(original / p);
            }

            bool allowsConcurrentTraversal() override
            {
                return owner->allowsConcurrentTraversal() && next->allowsConcurrentTraversal();
            }

            std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & p) override
            {
                return owner->getFingerprint(original / p);
            }

            void readDirectoryWithEntries(const CanonPath & p, ReadDirectoryCallback callback) override
            {
                readDirectoryScoped(p, std::move(callback));
            }
        };
        /* Non-root scopes flatten coordinates; root rereads retain the prior admission. */
        auto * parent = path.isRoot() ? nullptr : dynamic_cast<Scoped *>(this);
        Scoped scoped{inner, relative, *this, path, parent, std::move(listing)};
        callback(scoped, CanonPath::root, DirectoryListing(std::cref(scoped.admitted)));
    });
}

std::string FilteringSourceAccessor::showPath(const CanonPath & path)
{
    auto owner = weak_from_this().lock();
    return displayPrefix + next->showPath(prefix / path) + displaySuffix;
}

void FilteringSourceAccessor::checkAccess(const CanonPath & path)
{
    auto owner = checkAllowed(path);
}

std::shared_ptr<SourceAccessor> FilteringSourceAccessor::checkAllowed(const CanonPath & path)
{
    auto owner = weak_from_this().lock();
    if (!isAllowed(path))
        throw makeNotAllowedError(path);
    return owner;
}

struct AllowListSourceAccessorImpl final : AllowListSourceAccessor
{
    bool allowsConcurrentTraversal() override
    {
        return next.get_ptr()->allowsConcurrentTraversal();
    }

    void readDirectoryWithEntries(const CanonPath & path, ReadDirectoryCallback callback) override
    {
        readDirectoryScoped(path, std::move(callback));
    }
private:
    void anchor() override {};
public:
    SharedSync<std::set<CanonPath>> allowedPrefixes;
    const std::unordered_set<CanonPath> allowedPaths;

    AllowListSourceAccessorImpl(
        ref<SourceAccessor> next,
        const std::set<CanonPath> & allowedPrefixes,
        const std::unordered_set<CanonPath> & allowedPaths,
        MakeNotAllowedError && makeNotAllowedError)
        : AllowListSourceAccessor(SourcePath(next), std::move(makeNotAllowedError))
        , allowedPrefixes(allowedPrefixes.begin(), allowedPrefixes.end())
        , allowedPaths(allowedPaths)
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
        if (allowedPaths.contains(path))
            return true;
        /* Prefixes are minimal and each subtree is contiguous. The next
           prefix can be at or below path; only the previous can contain it. */
        auto prefixes = allowedPrefixes.readLock();
        auto next = prefixes->lower_bound(path);
        return (next != prefixes->end() && next->isWithin(path))
               || (next != prefixes->begin() && path.isWithin(*std::prev(next)));
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

    bool allowsSubtree(const CanonPath & path)
    {
        auto prefixes(allowedPrefixes.readLock());
        auto after = prefixes->upper_bound(path);
        return after != prefixes->begin() && path.isWithin(*std::prev(after));
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        if (!allowsSubtree(path))
            return FilteringSourceAccessor::getPhysicalPath(path);
        /* Admission only grows; release the policy lock before calling
           the child, which may reenter or release this wrapper. */
        return next.get_ptr()->getPhysicalPath(prefix / path);
    }

    std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & path) override
    {
        if (fingerprint)
            return std::pair{path, *fingerprint};
        /* At or under an allowed prefix everything is admitted, so the
           subtree is the inner one and carries its name.  Elsewhere, at an
           ancestor of a prefix for instance, entries are hidden, and the
           subtree has no name. */
        if (!allowsSubtree(path))
            return std::nullopt;
        /* Admission only grows, so this subtree remains fully admitted
           after unlocking. The child may reenter allowPrefix, or release
           the last owner of this wrapper, while providing its name. */
        return next.get_ptr()->getFingerprint(prefix / path);
    }
};

struct FixedSetFilteringSourceAccessor : FilteringSourceAccessor
{
    void readDirectoryWithEntries(const CanonPath & path, ReadDirectoryCallback callback) override
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
        , accepted(std::move(accepted))
    {
    }

    bool isAllowed(const CanonPath & path) override
    {
        return accepted.contains(path);
    }

    std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & path) override
    {
        if (fingerprint)
            return std::pair{path, *fingerprint};
        auto owner = weak_from_this().lock();
        auto inner = next->getFingerprint(prefix / path);
        if (!inner)
            return std::nullopt;
        const auto & [innerPath, innerName] = *inner;
        /* The admitted set below `path`, relative to it, in canonical form:
           the NAR of the filtered subtree depends on nothing else beyond
           the inner subtree.  The name is for this subtree exactly, hence
           the root path. */
        std::string below;
        for (auto i = accepted.lower_bound(path); i != accepted.end() && i->isWithin(path); ++i) {
            below += i->removePrefix(path).abs();
            below += '\0';
        }
        return std::pair{
            CanonPath::root,
            nlohmann::json::array({"filter",
                                   innerName,
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
    auto owner = weak_from_this().lock();
    auto res = isAllowedUncached(path);
    cache->emplace(path, res);
    return res;
}

void CachingFilteringSourceAccessor::invalidateCache()
{
    cache->clear();
    FilteringSourceAccessor::invalidateCache();
}

} // namespace nix
