/* SoundnessGuard (Layer-2-only / commutative idempotent join-
   semilattice morphism on fingerprints). Tests cover the Layer-1
   pass-through laws (L7a–L7c), Layer-2 bypass on closure(R) (L7d),
   forwarding outside closure (L7d converse), join-semilattice morphism
   (L7f), idempotent (L7g), commutative (L7h). */

#include <exception> // IWYU pragma: keep
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "nix/fetchers/soundness-guard-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-path.hh"
#include "nix/util/tests/source-accessor.hh"

#include "operator-test-helpers.hh"

namespace nix {

namespace {

ref<SoundnessGuardSourceAccessor> makeGuard(ref<SourceAccessor> base, std::set<CanonPath> r)
{
    auto rptr = std::make_shared<const std::set<CanonPath>>(std::move(r));
    return make_ref<SoundnessGuardSourceAccessor>(base, rptr, [](const CanonPath & p) -> RestrictedPathError {
        return RestrictedPathError("guard '%s'", p.abs());
    });
}

} // namespace

TEST(SoundnessGuard, ReadPassthrough)
{
    auto base = testHelpers::makeOpFs();
    auto g = makeGuard(base, {CanonPath("/a/x.txt")});
    /* L7a: reads pass through unchanged regardless of R. */
    EXPECT_EQ(g->readFile(CanonPath("/a/x.txt")), "ax");
    EXPECT_EQ(g->readFile(CanonPath("/b.txt")), "b");
}

TEST(SoundnessGuard, MaybeLstatPassthrough)
{
    auto base = testHelpers::makeOpFs();
    auto g = makeGuard(base, {CanonPath("/a/x.txt")});
    EXPECT_TRUE(g->maybeLstat(CanonPath("/a/x.txt")).has_value());
    EXPECT_TRUE(g->maybeLstat(CanonPath("/b.txt")).has_value());
}

TEST(SoundnessGuard, ReadDirectoryPassthrough)
{
    auto base = testHelpers::makeOpFs();
    auto g = makeGuard(base, {CanonPath("/a/x.txt")});
    auto entries = g->readDirectory(CanonPath::root);
    EXPECT_TRUE(entries.contains("a"));
    EXPECT_TRUE(entries.contains("b.txt"));
}

TEST(SoundnessGuard, FingerprintBypassesInClosure)
{
    auto base = testHelpers::makeOpFs();
    base->fingerprint = "git:R";
    auto g = makeGuard(base, {CanonPath("/a/x.txt")});
    /* `/a/x.txt` is in closure; `/` ancestor too. */
    auto suffix = g->computeOwnSuffix(CanonPath("/a/x.txt"));
    EXPECT_FALSE(suffix.has_value()) << "should bypass on path in R";
    suffix = g->computeOwnSuffix(CanonPath::root);
    EXPECT_FALSE(suffix.has_value()) << "should bypass on ancestor of R";
}

TEST(SoundnessGuard, FingerprintForwardsOutsideClosure)
{
    auto base = testHelpers::makeOpFs();
    auto g = makeGuard(base, {CanonPath("/a/x.txt")});
    /* `/b.txt` is not in closure({/a/x.txt}). */
    auto suffix = g->computeOwnSuffix(CanonPath("/b.txt"));
    ASSERT_TRUE(suffix.has_value());
    EXPECT_TRUE(suffix->empty());
}

TEST(SoundnessGuard, EmptyRIsIdentity)
{
    /* L7e: SoundnessGuard(∅) ≡ Identity (transparent at Layer-2). */
    auto base = testHelpers::makeOpFs();
    auto g = makeGuard(base, {});
    auto suffix = g->computeOwnSuffix(CanonPath("/a/x.txt"));
    ASSERT_TRUE(suffix.has_value());
    EXPECT_TRUE(suffix->empty());
}

#ifndef COVERAGE

/* L7d & forward: behaviour matches `pathSet::inClosure` exactly. */
RC_GTEST_PROP(SoundnessGuard, FingerprintBypassMatchesClosure, (const std::set<CanonPath> & r, const CanonPath & x))
{
    RC_PRE(!r.empty());
    auto base = make_ref<MemorySourceAccessor>();
    auto g = makeGuard(base, r);
    auto suffix = g->computeOwnSuffix(x);
    if (pathSet::inClosure(*g->paths, x)) {
        RC_ASSERT(!suffix.has_value());
    } else {
        RC_ASSERT(suffix.has_value() && suffix->empty());
    }
}

/* L7g: idempotent. */
RC_GTEST_PROP(SoundnessGuard, Idempotent, (const std::set<CanonPath> & r, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    auto outer = makeGuard(makeGuard(base, r).cast<SourceAccessor>(), r);
    auto single = makeGuard(base, r);
    RC_ASSERT(outer->computeOwnSuffix(x) == single->computeOwnSuffix(x));
}

/* L7f: SoundnessGuard ∘ SoundnessGuard = SoundnessGuard(R1 ∪ R2)
   at the bypass level. The composition is observable through
   `getFingerprint` (which walks the stack via `composeFingerprint`),
   not through `computeOwnSuffix` (which queries only the outer
   wrapper's local contribution and is necessarily transparent for
   the outer when its R is empty). */
RC_GTEST_PROP(
    SoundnessGuard,
    JoinSemilatticeMorphism,
    (const std::set<CanonPath> & r1, const std::set<CanonPath> & r2, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    base->fingerprint = "tree:base";

    std::set<CanonPath> unioned = r1;
    unioned.insert(r2.begin(), r2.end());

    auto stacked = makeGuard(makeGuard(base, r1).cast<SourceAccessor>(), r2);
    auto fused = makeGuard(base, unioned);
    auto [_p1, fp1] = stacked->getFingerprint(x);
    auto [_p2, fp2] = fused->getFingerprint(x);
    RC_ASSERT(fp1 == fp2);
}

/* L7h: SoundnessGuard is commutative on bypass at the composed
   getFingerprint level. */
RC_GTEST_PROP(
    SoundnessGuard, Commutative, (const std::set<CanonPath> & r1, const std::set<CanonPath> & r2, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    base->fingerprint = "tree:base";
    auto g12 = makeGuard(makeGuard(base, r1).cast<SourceAccessor>(), r2);
    auto g21 = makeGuard(makeGuard(base, r2).cast<SourceAccessor>(), r1);
    auto [_p1, fp1] = g12->getFingerprint(x);
    auto [_p2, fp2] = g21->getFingerprint(x);
    RC_ASSERT(fp1 == fp2);
}

#endif

/* L7i: SoundnessGuard does NOT distribute over Layer in general.
   Specifically: distributing the outer R into per-child R₁/R₂ such
   that R₁ ∪ R₂ = R but R₁ ≠ R changes the bypass behaviour at
   paths in (R \ closure(Rᵢ)). The outer SG sees the union as one
   coherent boundary; the per-child SGs each see only their slice.
   This is the "cross-child information no individual child has"
   point — exactly what makes the SG-outer-Layer composition
   correct for Overlay's R = E ∪ W.

   Concrete witness:
     R = {/foo, /bar}, R₁ = {/foo}, R₂ = {/bar}.
     a has fingerprint "tree:A" (its own field) and that's all
     it needs to contribute via getFingerprint at any path.
     b is empty (no fingerprint).

   At /bar:
     LHS `SG(R)(Layer([a, b])).getFingerprint(/bar)`:
       Layer first-wins → a's "tree:A". SG: /bar ∈ R → bypass
       → nullopt.
     RHS `Layer([SG(R₁)(a), SG(R₂)(b)]).getFingerprint(/bar)`:
       SG(R₁)(a).getFingerprint(/bar): a returns "tree:A".
       SG suffix on /bar with R₁={/foo}: /bar not in
       closure({/foo}) → empty suffix → forward as "tree:A".
       Layer takes first non-null → returns "tree:A".

   LHS yields nullopt (cross-child boundary triggers bypass).
   RHS yields "tree:A" (per-child SG never sees /bar in its R).
   They differ — non-distributivity demonstrated.

   This test would also fail loudly if a future refactor "fixed"
   distributivity by widening per-child Rs (which would break the
   E-vs-W discrimination Overlay relies on). */
TEST(SoundnessGuard, NonDistributiveOverLayerWhenRSplits)
{
    auto a = make_ref<MemorySourceAccessor>();
    a->fingerprint = "tree:A";
    auto b = make_ref<MemorySourceAccessor>();

    std::set<CanonPath> r{CanonPath("/foo"), CanonPath("/bar")};
    std::set<CanonPath> r1{CanonPath("/foo")};
    std::set<CanonPath> r2{CanonPath("/bar")};

    /* LHS: outer SG sees the full R; bypass fires at /bar. */
    auto layered = makeUnionSourceAccessor({a, b});
    auto lhs = makeGuard(layered, r);
    auto [_p1, lhsFp] = lhs->getFingerprint(CanonPath("/bar"));
    EXPECT_FALSE(lhsFp.has_value()) << "outer SG with full R should bypass at /bar";

    /* RHS: per-child SGs split R; neither child sees /bar in its
       own R, so neither bypasses; Layer's first-wins forwards a's fp. */
    auto sga = makeGuard(a, r1);
    auto sgb = makeGuard(b, r2);
    auto rhs = makeUnionSourceAccessor({sga.cast<SourceAccessor>(), sgb.cast<SourceAccessor>()});
    auto [_p2, rhsFp] = rhs->getFingerprint(CanonPath("/bar"));
    ASSERT_TRUE(rhsFp.has_value()) << "per-child SGs split R; /bar outside both closures";
    EXPECT_EQ(*rhsFp, "tree:A");
}

} // namespace nix
