#pragma once
///@file

#include "nix/fetchers/filtering-source-accessor.hh"

namespace nix {

/**
 * `Restrict(S)`: the Prism / commutative idempotent meet-semilattice
 * morphism on `(sets, ∩)`. Admits paths in S plus their ancestors;
 * descendants of S are NEVER admitted (L8 — symlink containment is
 * non-negotiable). Identity is `Restrict(universe)`; the factory
 * convention `S = ∅` is the universe sentinel and short-circuits
 * to no wrap.
 *
 * Layer 1 read semantics (per L4 — pure filter, post-Phase-1):
 *   - x ∈ S → forward to base.
 *   - x ∈ ancestors-of-S → forward to base; if base lacks the path,
 *     callers see nullopt rather than synthesised directory stats.
 *     Ancestor walkability for filter-derived views is provided by
 *     `DirectorySynthesizerSourceAccessor` (see
 *     `directory-synthesizer-source-accessor.hh`), wrapped outermost
 *     by the source-view factories.
 *   - else → throws via the err callback (`FileNotFound` for
 *     filtered-out).
 *
 * Layer 2 fingerprint semantics (per L7d / L10): bypass via
 * `computeOwnSuffix → nullopt` for paths in `closure(S)`. Inherited
 * from `PathSetOp::computeOwnSuffix`.
 *
 * Use `MakeNotFoundError` (defined in filtering-source-accessor.hh) when
 * "filtered out" should mean "doesn't exist in this view"; use
 * `MakeNotAllowedError` for security-flavoured uses.
 *
 * `lstat`/`maybeLstat`/`readDirectory` inherit from
 * `FilteringSourceAccessor`'s defaults: `isAllowed`-gate then forward
 * to `next`. No method overrides — the synthesis branches that used
 * to live here are now in `DirectorySynthesizerSourceAccessor`.
 */
struct RestrictSourceAccessor final : PathSetOp<RestrictSourceAccessor>
{
    using PathSetOp::PathSetOp;

    /* Algebraic shape: Prism on `(sets, ∩)`.
       Layer-1 admit-closure (S ∪ ancestors-of-S ∪ {root});
       Layer-2 bypass on closure(S);
       Meet-semilattice morphism for factory fusion (universe = ∅
       sentinel). */
    static constexpr PathSetAdmission kAdmission = PathSetAdmission::AdmitClosure;
    static constexpr PathSetFingerprint kFingerprint = PathSetFingerprint::BypassInClosure;
    static constexpr PathSetSemilattice kSemilattice = PathSetSemilattice::Meet;
};

/**
 * Construct `Restrict(paths)(base)` with morphism fusion.
 *
 * Identity short-circuit (L2b): empty `paths` is the empty-S sentinel
 * (universe in the recipe convention) → returns `base` unchanged.
 *
 * Fusion (L2a): if `base` is itself a `RestrictSourceAccessor`, fuse to
 * `Restrict(paths ∩ inner->paths)(inner->next)`. If the intersection is
 * empty, the fused operator is `Restrict(∅) ≡ Identity` per the
 * sentinel convention, so the helper returns `inner->next` directly.
 *
 * Returns `ref<SourceAccessor>` rather than the concrete operator type
 * because the identity-short-circuit branches may return a different
 * concrete type (the inner base accessor).
 */
ref<SourceAccessor> makeRestrict(
    ref<SourceAccessor> base,
    std::shared_ptr<const std::set<CanonPath>> paths,
    MakeNotAllowedError makeNotAllowedError);

} // namespace nix
