#pragma once
///@file

#include "nix/fetchers/filtering-source-accessor.hh"

namespace nix {

/**
 * `DirectorySynthesizer(S)`: synthesis sub-operator of the
 * **admission** morphism family (sibling of `Restrict`/`Mask`).
 * Surfaces ancestors of `S` that `next` doesn't materialise as
 * synthesised `tDirectory` entries, so directory walks can descend
 * toward leaves in S even on bases that don't carry the intermediate
 * dirs.
 *
 * - **Layer 1**: admit-all (`isAdmitted` ≡ true). `maybeLstat` and
 *   `readDirectory` are overridden to add ancestor synthesis on top
 *   of `next`'s real entries; other reads pass through unchanged.
 *   `pathExists` is inherited via `SourceAccessor::pathExists` (which
 *   routes through `maybeLstat`), so D7 holds without an override.
 * - **Layer 2**: transparent (`inClosure` ≡ false → empty
 *   `computeOwnSuffix`). Synthesis is content-identity-neutral (D11).
 *
 * Composes with Restrict and Mask but is **not** commutative with
 * them at `readDirectory` (D12). Factories place D outermost over
 * the filter chain so synthesis sees the composed admit set.
 *
 * Full law inventory (D1–D13) lives in `doc/tecnix-survey/PROPOSAL.md`
 * §3 (the "D (DirectorySynthesizer)" bullet); the recipe→stack
 * derivation is in §1.4 / §2.Q.
 */
struct DirectorySynthesizerSourceAccessor final : PathSetOp<DirectorySynthesizerSourceAccessor>
{
    using PathSetOp::PathSetOp;

    /* Algebraic shape: Layer-1-only synthesis sub-operator.
       Layer-1 identity admission (synthesis is *additive* on top of
         next, never gating — see maybeLstat/readDirectory overrides
         for the synthesis itself);
       Layer-2 identity (D11 — content-identity neutral);
       Join-semilattice morphism for factory fusion (identity = ∅). */
    static constexpr PathSetAdmission kAdmission = PathSetAdmission::AdmitAll;
    static constexpr PathSetFingerprint kFingerprint = PathSetFingerprint::Identity;
    static constexpr PathSetSemilattice kSemilattice = PathSetSemilattice::Join;

    std::optional<Stat> maybeLstat(const CanonPath & p) override;
    DirEntries readDirectory(const CanonPath & p) override;
};

/**
 * Construct `D(paths)(base)` with morphism fusion.
 *
 * - D1 (identity): empty `paths` → returns `base` unchanged.
 * - D3 (fusion): `D(S₁) ∘ D(S₂) ≡ D(S₁ ∪ S₂)` (join-semilattice
 *   morphism on path sets).
 */
/* `DirectorySynthesizer` is `AdmitAll`, so its error builder is dead —
   defaulted to `neverDenied()`. Callers needn't supply one. */
ref<SourceAccessor> makeDirectorySynthesizer(
    ref<SourceAccessor> base,
    std::shared_ptr<const std::set<CanonPath>> paths,
    MakeNotAllowedError makeNotAllowedError = neverDenied());

} // namespace nix
