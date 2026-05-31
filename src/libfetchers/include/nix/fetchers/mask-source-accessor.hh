#pragma once
///@file

#include "nix/fetchers/filtering-source-accessor.hh"

namespace nix {

/**
 * `Mask(W)`: the Prism dual / commutative idempotent join-semilattice
 * morphism on `(sets, ∪)`. Denies `W ∪ desc(W)` (i.e. paths in W and
 * paths beneath any element of W). Outside that closure, reads pass
 * through. Identity is `Mask(∅)`.
 *
 * Layer 1 read semantics (per L5):
 *   - x ∈ W or descendant of any w ∈ W → denied.
 *   - else → forward to base. Listings: `b.readDirectory(p)` minus
 *     children whose path is in W (the parent listing succeeds even
 *     if `p` is the parent of a whiteout).
 *
 * Layer 2 fingerprint semantics (per L7d analogue): bypass via
 * `computeOwnSuffix → nullopt` for paths in `closure(W)`. Inherited
 * from `PathSetOp::computeOwnSuffix`.
 *
 * `readDirectory` filtering is automatic via the inherited
 * `FilteringSourceAccessor::readDirectory` (which calls our
 * `isAllowed` per child).
 */
struct MaskSourceAccessor final : PathSetOp<MaskSourceAccessor>
{
    using PathSetOp::PathSetOp;

    /* Algebraic shape: Prism dual on `(sets, ∪)`.
       Layer-1 deny-closure (W ∪ descendants-of-W);
       Layer-2 bypass on closure(W);
       Join-semilattice morphism for factory fusion (identity = ∅). */
    static constexpr PathSetAdmission kAdmission = PathSetAdmission::DenyClosure;
    static constexpr PathSetFingerprint kFingerprint = PathSetFingerprint::BypassInClosure;
    static constexpr PathSetSemilattice kSemilattice = PathSetSemilattice::Join;
};

/**
 * Construct `Mask(paths)(base)` with morphism fusion.
 *
 * Identity short-circuit (L3b): empty `paths` is `Mask(∅) ≡ Identity`
 * → returns `base` unchanged.
 *
 * Fusion (L3a): if `base` is itself a `MaskSourceAccessor`, fuse to
 * `Mask(paths ∪ inner->paths)(inner->next)`.
 *
 * Returns `ref<SourceAccessor>` rather than the concrete operator type
 * because the identity-short-circuit branch may return `base` directly.
 */
ref<SourceAccessor> makeMask(
    ref<SourceAccessor> base,
    std::shared_ptr<const std::set<CanonPath>> paths,
    MakeNotAllowedError makeNotAllowedError);

} // namespace nix
