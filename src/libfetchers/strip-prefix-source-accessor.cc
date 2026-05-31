#include "nix/fetchers/strip-prefix-source-accessor.hh"

namespace nix {

std::optional<std::filesystem::path> StripPrefixSourceAccessor::getPhysicalPath(const CanonPath & path)
{
    checkAccess(path);
    return next->getPhysicalPath(path.removePrefix(prefix));
}

void StripPrefixSourceAccessor::readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback)
{
    checkAccess(path);
    return next->readFile(path.removePrefix(prefix), sink, sizeCallback);
}

SourceAccessor::Stat StripPrefixSourceAccessor::lstat(const CanonPath & path)
{
    checkAccess(path);
    return next->lstat(path.removePrefix(prefix));
}

std::optional<SourceAccessor::Stat> StripPrefixSourceAccessor::maybeLstat(const CanonPath & path)
{
    /* Non-throwing: the Prism deny is the empty answer, not an
       exception. Guard with `isAllowed` so `removePrefix` only runs on
       in-prefix paths. */
    return isAllowed(path) ? next->maybeLstat(path.removePrefix(prefix)) : std::nullopt;
}

SourceAccessor::DirEntries StripPrefixSourceAccessor::readDirectory(const CanonPath & path)
{
    /* `isAllowed` for in-prefix paths admits the whole subtree, so no
       per-child filtering is needed — forward the stripped listing
       as-is. (Off-prefix paths are rejected by `checkAccess`.) */
    checkAccess(path);
    return next->readDirectory(path.removePrefix(prefix));
}

std::string StripPrefixSourceAccessor::readLink(const CanonPath & path)
{
    checkAccess(path);
    return next->readLink(path.removePrefix(prefix));
}

std::string StripPrefixSourceAccessor::showPath(const CanonPath & path)
{
    /* Display only; must not throw on an off-prefix path (it may be
       formatted into an error message *about* that denied path). Fall
       back to the base rendering when we can't strip. */
    if (!path.isWithin(prefix))
        return SourceAccessor::showPath(path);
    return displayPrefix + next->showPath(path.removePrefix(prefix)) + displaySuffix;
}

std::pair<CanonPath, std::optional<std::string>> StripPrefixSourceAccessor::getFingerprint(const CanonPath & path)
{
    /* Off-domain: fall back to the wrapper's own fingerprint without
       calling `next` (and without `removePrefix`, which would assert).
       In-domain: forward as `(*this, *next, outerPath=path,
       innerPath=path.removePrefix(prefix))` — `composeFingerprint` gets
       the outer-rooted path for the wrapper's own input-level fallback,
       and the stripped inner-rooted path for `next->getFingerprint`.
       Mirror of `Translate`, which threads `prefix / path`. */
    if (!path.isWithin(prefix))
        return {path, fingerprint};
    return composeFingerprint(*this, *next, path, path.removePrefix(prefix));
}

void StripPrefixSourceAccessor::prefetchSubtree(const CanonPath & subpath, unsigned depth)
{
    /* Gate on `isAllowed` (as the base does) but strip before
       forwarding. Off-prefix subpaths are a silent no-op — prefetch is
       a best-effort hint, not an observation. */
    if (isAllowed(subpath))
        next->prefetchSubtree(subpath.removePrefix(prefix), depth);
}

ref<SourceAccessor> makeStripPrefix(ref<SourceAccessor> base, CanonPath prefix, MakeNotAllowedError makeNotAllowedError)
{
    /* SP1: identity short-circuit. `StripPrefix(/) ≡ Identity`. */
    if (prefix.isRoot())
        return base;

    /* SP2: fuse `StripPrefix(p) ∘ StripPrefix(q) ≡ StripPrefix(p / q)`.
       Outer reads at `x` strip `p` (→ `x.removePrefix(p)`), then inner
       strips `q` (→ `x.removePrefix(p).removePrefix(q)` =
       `x.removePrefix(p / q)`) — so the fused prefix is
       `prefix / inner->prefix` (OUTER / INNER). This is the opposite
       order from `makeTranslate`; see the header note. */
    if (auto inner = base.dynamic_pointer_cast<StripPrefixSourceAccessor>())
        return make_ref<StripPrefixSourceAccessor>(inner->next, prefix / inner->prefix, std::move(makeNotAllowedError));

    return make_ref<StripPrefixSourceAccessor>(std::move(base), std::move(prefix), std::move(makeNotAllowedError));
}

} // namespace nix
