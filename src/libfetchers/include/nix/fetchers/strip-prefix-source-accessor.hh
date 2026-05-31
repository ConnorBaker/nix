#pragma once
///@file

#include "nix/fetchers/filtering-source-accessor.hh"

namespace nix {

/**
 * `StripPrefix(p)`: the Prism on paths that is the partial inverse of
 * `Translate(p)` — strip vs prepend. For all `x` within `p`,
 * `StripPrefix(p)(b).read(x) = b.read(x.removePrefix(p))`. Paths NOT
 * within `p` are denied (the Prism's no-match case). Identity is
 * `p = /`, which the factory short-circuits to no wrap.
 *
 * `Translate` and `StripPrefix` are the two adjoint halves of path
 * re-rooting (a section/retraction pair):
 *
 *   - `Translate(p)(StripPrefix(p)(a)) ≡ a` — **total** identity:
 *     prepend-after-strip recovers every path (the retraction).
 *   - `StripPrefix(p)(Translate(p)(a)) ≡ a` on `{x : x.isWithin(p)}` —
 *     identity **restricted to the in-prefix cone**: the outer strip
 *     gates everything outside `p` (the section).
 *
 * Layer 1 read semantics:
 *   - x within p → forward to base at `x.removePrefix(p)`.
 *   - x not within p → denied (`maybeLstat`/`prefetchSubtree` return
 *     the empty answer; the throwing read methods raise via the err
 *     callback — `FileNotFound` for the domain-restriction reading,
 *     `RestrictedPathError` for security-flavoured uses).
 *
 * Layer 2 fingerprint semantics: empty `computeOwnSuffix` — transparent
 * forward through `composeFingerprint`. No own contribution to the
 * fingerprint; the `getFingerprint` override threads `x.removePrefix(p)`
 * as the inner-path argument so the wrapper observes the same
 * fingerprint `next` would produce at the stripped path. (Mirror of
 * `Translate`, which threads `p / x` instead.) Off-domain (`x` not
 * within `p`) returns the wrapper's own `fingerprint` fallback without
 * touching `next` — never asserts on `removePrefix`.
 *
 * Contrast with `Translate`: Translate's `isAllowed` is unconditionally
 * true (an Iso denies nothing); StripPrefix's `isAllowed` is the real
 * Prism gate `x.isWithin(prefix)`. And where Translate prepends in its
 * read overrides, StripPrefix strips.
 */
struct StripPrefixSourceAccessor final : FilteringSourceAccessor
{
    CanonPath prefix;

    StripPrefixSourceAccessor(ref<SourceAccessor> next, CanonPath prefix, MakeNotAllowedError && makeNotAllowedError)
        : FilteringSourceAccessor(std::move(next), std::move(makeNotAllowedError))
        , prefix(std::move(prefix))
    {
    }

    /* The Prism gate: admit `x` iff it is `prefix` or below it. Denies
       ancestors of `prefix` too — you cannot read above the mount
       point through the strip. Unlike `Translate` (always true). */
    bool isAllowed(const CanonPath & path) override
    {
        return path.isWithin(prefix);
    }

    /* default empty `computeOwnSuffix` from `SourceAccessor` is fine —
       transparent forward, same as Translate. */

    /* Read-method overrides: gate via `isAllowed`/`checkAccess`, then
       strip the prefix before forwarding to `next`. The base
       `FilteringSourceAccessor` forwards the path unchanged, so — like
       `Translate` — every read method must be overridden to apply the
       path translation (here: `removePrefix`, not `prefix /`).

       The throwing methods call `checkAccess(path)` first: that both
       delivers the Prism deny for off-prefix paths AND guards
       `removePrefix`'s `isWithin` precondition (so it never asserts).
       The non-throwing methods (`maybeLstat`, `prefetchSubtree`,
       `showPath`, `getFingerprint`) guard with an explicit `isAllowed`
       / `isWithin` check instead. */

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override;

    using FilteringSourceAccessor::readFile;
    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override;

    /* No `pathExists` override: inherits `SourceAccessor::pathExists`
       (defined as `maybeLstat(path).has_value()`), which routes through
       our `maybeLstat` override and so picks up the prefix strip and the
       Prism gate for free. Mirrors the base `FilteringSourceAccessor`
       decision — see the comment there for rationale. */

    Stat lstat(const CanonPath & path) override;

    std::optional<Stat> maybeLstat(const CanonPath & path) override;

    DirEntries readDirectory(const CanonPath & path) override;

    std::string readLink(const CanonPath & path) override;

    std::string showPath(const CanonPath & path) override;

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override;

    void prefetchSubtree(const CanonPath & subpath, unsigned depth) override;
};

/**
 * Construct `StripPrefix(prefix)(base)` with morphism fusion.
 *
 * Identity short-circuit (SP1): `prefix.isRoot()` returns `base`
 * unchanged (`removePrefix` on a root prefix is already the no-op, so
 * the wrapper would be transparent; the factory makes it a true
 * no-wrap).
 *
 * Fusion (SP2): `StripPrefix(p) ∘ StripPrefix(q) ≡ StripPrefix(p / q)`.
 * If `base` is itself a `StripPrefixSourceAccessor` with inner prefix
 * `q`, fuse to `StripPrefix(prefix / q)(base->next)`. The resulting
 * wrapper depth never exceeds 1.
 *
 * NOTE the fused field order is `prefix / inner->prefix` (OUTER / INNER)
 * — the *opposite* of `makeTranslate`, which fuses to
 * `inner->prefix / prefix`. The asymmetry is real: for `Translate` the
 * inner prefix is applied closest to base (reads prepend outer→inner),
 * whereas for `StripPrefix` the outer strip is applied first (closest to
 * the query). Getting this order wrong is an undetectable-by-types
 * field swap; the `factory-fusion.cc` structural test pins it.
 *
 * Returns `ref<SourceAccessor>` rather than the concrete operator type
 * because the identity-short-circuit branch may return `base` directly
 * (a different concrete type).
 */
ref<SourceAccessor>
makeStripPrefix(ref<SourceAccessor> base, CanonPath prefix, MakeNotAllowedError makeNotAllowedError);

} // namespace nix
