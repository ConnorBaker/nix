#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/sync.hh"
#include "nix/util/util.hh"

#include <boost/unordered/concurrent_flat_set.hpp>

namespace nix {

std::optional<std::filesystem::path> FilteringSourceAccessor::getPhysicalPath(const CanonPath & path)
{
    checkAccess(path);
    return next->getPhysicalPath(path);
}

void FilteringSourceAccessor::readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback)
{
    checkAccess(path);
    return next->readFile(path, sink, sizeCallback);
}

/* No `pathExists` override: inherit `SourceAccessor::pathExists` which
   is defined as `maybeLstat(path).has_value()`. Routing through our
   `maybeLstat` (rather than `next->pathExists`) lets derived classes
   that synthesise stats — like `DirectorySynthesizerSourceAccessor` —
   surface them through `pathExists` without an explicit override. */

std::optional<SourceAccessor::Stat> FilteringSourceAccessor::maybeLstat(const CanonPath & path)
{
    return isAllowed(path) ? next->maybeLstat(path) : std::nullopt;
}

SourceAccessor::Stat FilteringSourceAccessor::lstat(const CanonPath & path)
{
    checkAccess(path);
    return next->lstat(path);
}

SourceAccessor::DirEntries FilteringSourceAccessor::readDirectory(const CanonPath & path)
{
    checkAccess(path);
    DirEntries entries;
    for (auto & entry : next->readDirectory(path)) {
        if (isAllowed(path / entry.first))
            entries.insert(std::move(entry));
    }
    return entries;
}

std::string FilteringSourceAccessor::readLink(const CanonPath & path)
{
    checkAccess(path);
    return next->readLink(path);
}

std::string FilteringSourceAccessor::showPath(const CanonPath & path)
{
    return displayPrefix + next->showPath(path) + displaySuffix;
}

std::pair<CanonPath, std::optional<std::string>> FilteringSourceAccessor::getFingerprint(const CanonPath & path)
{
    return composeFingerprint(*this, *next, path, path);
}

void FilteringSourceAccessor::checkAccess(const CanonPath & path)
{
    if (!isAllowed(path))
        throw makeNotAllowedError(path);
}

struct AllowListSourceAccessorImpl : AllowListSourceAccessor
{
    SharedSync<std::set<CanonPath>> allowedPrefixes;
    boost::concurrent_flat_set<CanonPath> allowedPaths;

    AllowListSourceAccessorImpl(
        ref<SourceAccessor> next,
        const std::set<CanonPath> & allowedPrefixes,
        const std::unordered_set<CanonPath> & allowedPaths,
        MakeNotAllowedError && makeNotAllowedError)
        : AllowListSourceAccessor(std::move(next), std::move(makeNotAllowedError))
        , allowedPrefixes(allowedPrefixes.begin(), allowedPrefixes.end())
        , allowedPaths(allowedPaths.begin(), allowedPaths.end())
    {
    }

    bool isAllowed(const CanonPath & path) override
    {
        /* Read lock is held for the duration of the full expression if the || doesn't short-circuit. */
        return allowedPaths.contains(path) || path.isAllowed(*allowedPrefixes.readLock());
    }

    void allowPrefix(CanonPath prefix) override
    {
        allowedPrefixes.lock()->insert(std::move(prefix));
    }
};

ref<AllowListSourceAccessor> AllowListSourceAccessor::create(
    ref<SourceAccessor> next,
    const std::set<CanonPath> & allowedPrefixes,
    const std::unordered_set<CanonPath> & allowedPaths,
    MakeNotAllowedError && makeNotAllowedError)
{
    return make_ref<AllowListSourceAccessorImpl>(next, allowedPrefixes, allowedPaths, std::move(makeNotAllowedError));
}

bool CachingFilteringSourceAccessor::isAllowed(const CanonPath & path)
{
    if (auto cached = getConcurrent(cache, path))
        return *cached;
    auto res = isAllowedUncached(path);
    cache.emplace(path, res);
    return res;
}

namespace pathSet {

bool ancestorOfMember(const std::set<CanonPath> & s, const CanonPath & p)
{
    /* p is a strict ancestor of some s in S iff some member is `p`-or-
       below but not `p` itself. The CanonPath ordering maps '/' to a
       byte that sorts before any other character (see
       `CanonPath::operator<=>`), so a strict descendant of `p` sorts
       immediately after `p` and before any sibling whose name merely
       shares `p`'s last component as a prefix (e.g. `/a/a` sorts before
       `/a-b` relative to `/a`). Hence the first candidate is
       `lower_bound(p)`, and — crucially — if `p` itself is a member,
       `lower_bound` lands on `p` and we must advance PAST it to reach
       the first potential strict descendant. (The earlier version
       bailed on `*it == p`, missing descendants of a member `p`; that
       was masked in `Restrict::isAllowed` by a preceding
       `paths->contains(p)` check, but live in
       `DirectorySynthesizer::maybeLstat`, which has no such guard.) */
    auto it = s.lower_bound(p);
    if (it != s.end() && *it == p)
        ++it;
    if (it == s.end())
        return false;
    return it->isWithin(p);
}

bool memberOrDescendantOfMember(const std::set<CanonPath> & s, const CanonPath & p)
{
    /* p ∈ S — direct hit. */
    if (s.contains(p))
        return true;
    /* p is descendant of some w ∈ S — walk up p's ancestors, checking
       membership. Stop at root. */
    auto cur = p;
    while (!cur.isRoot()) {
        auto parent = cur.parent();
        if (!parent)
            break;
        cur = *parent;
        if (s.contains(cur))
            return true;
    }
    return false;
}

bool inClosure(const std::set<CanonPath> & s, const CanonPath & p)
{
    /* In closure(S) iff p ∈ S, p is an ancestor of some s ∈ S, or
       p is a descendant of some s ∈ S. */
    if (memberOrDescendantOfMember(s, p))
        return true;
    return ancestorOfMember(s, p);
}

} // namespace pathSet

} // namespace nix
