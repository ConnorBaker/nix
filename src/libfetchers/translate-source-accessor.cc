#include "nix/fetchers/translate-source-accessor.hh"

namespace nix {

std::optional<std::filesystem::path> TranslateSourceAccessor::getPhysicalPath(const CanonPath & path)
{
    return next->getPhysicalPath(prefix / path);
}

void TranslateSourceAccessor::readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback)
{
    return next->readFile(prefix / path, sink, sizeCallback);
}

SourceAccessor::Stat TranslateSourceAccessor::lstat(const CanonPath & path)
{
    return next->lstat(prefix / path);
}

std::optional<SourceAccessor::Stat> TranslateSourceAccessor::maybeLstat(const CanonPath & path)
{
    return next->maybeLstat(prefix / path);
}

SourceAccessor::DirEntries TranslateSourceAccessor::readDirectory(const CanonPath & path)
{
    /* `isAllowed` is unconditionally true for Translate, so no per-child
       filtering is needed — forward the base listing as-is at the
       translated path. */
    return next->readDirectory(prefix / path);
}

std::string TranslateSourceAccessor::readLink(const CanonPath & path)
{
    return next->readLink(prefix / path);
}

std::string TranslateSourceAccessor::showPath(const CanonPath & path)
{
    return displayPrefix + next->showPath(prefix / path) + displaySuffix;
}

std::pair<CanonPath, std::optional<std::string>> TranslateSourceAccessor::getFingerprint(const CanonPath & path)
{
    /* Forward as `(*this, *next, outerPath=path, innerPath=prefix / path)`:
       `composeFingerprint` gets the outer-rooted path for the wrapper's
       own input-level fallback, and the translated inner-rooted path
       for `next->getFingerprint`. */
    return composeFingerprint(*this, *next, path, prefix / path);
}

void TranslateSourceAccessor::prefetchSubtree(const CanonPath & subpath, unsigned depth)
{
    /* `isAllowed` is unconditionally true; just translate the subpath
       and forward. The base's prefetchSubtree gates on `isAllowed`,
       which is true here, so the only Translate-specific behaviour is
       the prefix translation. */
    next->prefetchSubtree(prefix / subpath, depth);
}

ref<SourceAccessor> makeTranslate(ref<SourceAccessor> base, CanonPath prefix, MakeNotAllowedError makeNotAllowedError)
{
    /* L1b/L1c: identity short-circuit. `Translate(/) ≡ Identity`. */
    if (prefix.isRoot())
        return base;

    /* L1d: fuse `Translate(p) ∘ Translate(q) ≡ Translate(q / p)`.
       Outer reads at `x` go to inner at `p / x`, then to base at
       `q / (p / x) = (q / p) / x` — so the fused prefix is
       `inner->prefix / prefix`. */
    if (auto inner = base.dynamic_pointer_cast<TranslateSourceAccessor>())
        return make_ref<TranslateSourceAccessor>(inner->next, inner->prefix / prefix, std::move(makeNotAllowedError));

    return make_ref<TranslateSourceAccessor>(std::move(base), std::move(prefix), std::move(makeNotAllowedError));
}

} // namespace nix
