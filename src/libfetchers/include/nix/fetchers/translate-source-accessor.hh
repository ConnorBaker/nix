#pragma once
///@file

#include "nix/fetchers/filtering-source-accessor.hh"

namespace nix {

/**
 * `Translate(p)`: the Iso / monoid morphism on paths. For all `x`,
 * `Translate(p)(b).read(x) = b.read(p / x)`. Identity is `p = /`,
 * which the factory short-circuits to no wrap.
 *
 * Layer 1: pure prefix-translate via the operator's own `prefix`
 * field. `isAllowed` always returns true, so reads pass through —
 * the override read methods rewrite each query path as `prefix / path`
 * before forwarding to `next`.
 *
 * Layer 2: empty `computeOwnSuffix` — transparent forward through
 * `composeFingerprint`. No own contribution to the fingerprint;
 * the `getFingerprint` override threads `prefix / path` as the
 * inner-path argument so the wrapper observes the same fingerprint
 * `next` would produce at the translated path.
 */
struct TranslateSourceAccessor final : FilteringSourceAccessor
{
    CanonPath prefix;

    TranslateSourceAccessor(ref<SourceAccessor> next, CanonPath prefix, MakeNotAllowedError && makeNotAllowedError)
        : FilteringSourceAccessor(std::move(next), std::move(makeNotAllowedError))
        , prefix(std::move(prefix))
    {
    }

    bool isAllowed(const CanonPath &) override
    {
        return true;
    }

    /* default empty `computeOwnSuffix` from `SourceAccessor` is fine —
       transparent forward. */

    /* Read-method overrides: apply the prefix translation before
       forwarding to `next`. The base `FilteringSourceAccessor` no
       longer carries a prefix — Translate is the only descendant
       that needs path translation, so the translation lives here. */

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override;

    using FilteringSourceAccessor::readFile;
    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override;

    /* No `pathExists` override: inherits `SourceAccessor::pathExists`
       (defined as `maybeLstat(path).has_value()`), which routes through
       our `maybeLstat` override and so picks up the prefix translation
       for free. Mirrors the base `FilteringSourceAccessor` decision —
       see the comment there for rationale. */

    Stat lstat(const CanonPath & path) override;

    std::optional<Stat> maybeLstat(const CanonPath & path) override;

    DirEntries readDirectory(const CanonPath & path) override;

    std::string readLink(const CanonPath & path) override;

    std::string showPath(const CanonPath & path) override;

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override;

    void prefetchSubtree(const CanonPath & subpath, unsigned depth) override;
};

/**
 * Construct `Translate(prefix)(base)` with morphism fusion.
 *
 * Identity short-circuit (L1b/L1c): `prefix.isRoot()` returns `base`
 * unchanged.
 *
 * Fusion (L1d): if `base` is itself a `TranslateSourceAccessor` with
 * inner prefix `q`, fuse to `Translate(q / prefix)(base->next)`. The
 * resulting wrapper depth never exceeds 1 — `Translate ∘ Translate ∘ …`
 * collapses to a single Translate.
 *
 * Returns `ref<SourceAccessor>` rather than the concrete operator type
 * because the identity-short-circuit branch may return `base` directly
 * (a different concrete type).
 */
/* `Translate` always admits (`isAllowed` ≡ true), so its error builder is
   dead — defaulted to `neverDenied()` (see filtering-source-accessor.hh).
   Callers needn't supply one. */
ref<SourceAccessor>
makeTranslate(ref<SourceAccessor> base, CanonPath prefix, MakeNotAllowedError makeNotAllowedError = neverDenied());

} // namespace nix
