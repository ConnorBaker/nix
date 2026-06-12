#pragma once
///@file

#include "nix/fetchers/filtering-source-accessor.hh"

namespace nix {

/**
 * `SoundnessGuard(R)`: a Layer-2-only operator that bypasses the
 * fingerprint cache for paths in `closure(R)` while leaving reads
 * unchanged. The point is to express "the modified region for
 * fingerprint purposes" when the region depends on multiple sets that
 * no individual operator sees in isolation — most notably the
 * `Overlay` recipe, where `R = E ∪ W` (entries + whiteouts) so both
 * Restrict's and Mask's individual views miss cross-set
 * descendant-of-the-other paths.
 *
 * Layer 1 (per L7a–L7c): pure pass-through. `isAllowed` returns true
 * unconditionally; reads/lstat/dirlistings forward to base.
 *
 * Layer 2 (per L7d, L10): bypass via `computeOwnSuffix → nullopt`
 * when path is in `closure(R)`; transparent forward otherwise.
 * Inherited from `PathSetOp::computeOwnSuffix`.
 *
 * SoundnessGuard does NOT distribute over Layer in general (L7i) —
 * that's the *point*: it captures cross-child information no
 * individual child has.
 */
struct SoundnessGuardSourceAccessor final : PathSetOp<SoundnessGuardSourceAccessor>
{
    using PathSetOp::PathSetOp;

    ~SoundnessGuardSourceAccessor() override;

    /* Algebraic shape: Layer-2-only Writer-style decoration.
       Layer-1 identity (admit everything; reads pass through);
       Layer-2 bypass on closure(R) — the cross-set fingerprint
       boundary that no individual child sees in isolation;
       Join-semilattice morphism for factory fusion (identity = ∅). */
    static constexpr PathSetAdmission kAdmission = PathSetAdmission::AdmitAll;
    static constexpr PathSetFingerprint kFingerprint = PathSetFingerprint::BypassInClosure;
    static constexpr PathSetSemilattice kSemilattice = PathSetSemilattice::Join;
};

/**
 * Construct `SoundnessGuard(paths)(base)` with morphism fusion.
 *
 * Identity short-circuit (L7e): empty `paths` is
 * `SoundnessGuard(∅) ≡ Identity` → returns `base` unchanged.
 *
 * Fusion (L7f): if `base` is itself a `SoundnessGuardSourceAccessor`,
 * fuse to `SoundnessGuard(paths ∪ inner->paths)(inner->next)`
 * (join-semilattice morphism on fingerprints).
 *
 * Returns `ref<SourceAccessor>` rather than the concrete operator type
 * because the identity-short-circuit branch may return `base` directly.
 */
/* `SoundnessGuard` is `AdmitAll` (Layer-1 read pass-through; the guard is a
   Layer-2 fingerprint concern), so its error builder is dead — defaulted to
   `neverDenied()`. Callers needn't supply one. */
ref<SourceAccessor> makeSoundnessGuard(
    ref<SourceAccessor> base,
    std::shared_ptr<const std::set<CanonPath>> paths,
    MakeNotAllowedError makeNotAllowedError = neverDenied());

} // namespace nix
