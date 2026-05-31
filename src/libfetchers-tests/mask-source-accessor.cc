/* Mask (Prism dual / commutative idempotent join-semilattice morphism)
   — L3, L5, Layer-2 bypass. Tests cover join-semilattice morphism,
   idempotent, commutative, deny-closure semantics, and Layer-2 bypass. */

#include <exception> // IWYU pragma: keep
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/fetchers/mask-source-accessor.hh"
#include "nix/fetchers/restrict-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-path.hh"
#include "nix/util/tests/source-accessor.hh"

#include "operator-test-helpers.hh"

namespace nix {

namespace {

ref<MaskSourceAccessor> makeMask(ref<SourceAccessor> base, std::set<CanonPath> w)
{
    auto wptr = std::make_shared<const std::set<CanonPath>>(std::move(w));
    return make_ref<MaskSourceAccessor>(base, wptr, [](const CanonPath & p) -> RestrictedPathError {
        return RestrictedPathError("path '%s' masked", p.abs());
    });
}

/* File-local Restrict factory. Prefixed `mx` so that under the
   libfetchers-tests unity build it does not collide with the
   `makeRestrict` defined in restrict-source-accessor.cc's anonymous
   namespace. Used only by the conditional-commutativity tests below. */
ref<RestrictSourceAccessor> mxMkRestrict(ref<SourceAccessor> base, std::set<CanonPath> s)
{
    auto sptr = std::make_shared<const std::set<CanonPath>>(std::move(s));
    return make_ref<RestrictSourceAccessor>(base, sptr, [](const CanonPath & p) -> RestrictedPathError {
        return RestrictedPathError("path '%s' not allowed", p.abs());
    });
}

} // namespace

TEST(MaskSourceAccessor, EmptyWPassesThrough)
{
    auto base = testHelpers::makeOpFs();
    auto m = makeMask(base, {});
    EXPECT_EQ(m->readFile(CanonPath("/a/x.txt")), "ax");
}

TEST(MaskSourceAccessor, MaskedPathDenied)
{
    auto base = testHelpers::makeOpFs();
    auto m = makeMask(base, {CanonPath("/a/x.txt")});
    EXPECT_FALSE(m->maybeLstat(CanonPath("/a/x.txt")).has_value());
}

TEST(MaskSourceAccessor, DescendantOfMaskedPathDenied)
{
    auto base = testHelpers::makeOpFs();
    /* L5: descendants of W are also denied. */
    auto m = makeMask(base, {CanonPath("/a")});
    EXPECT_FALSE(m->maybeLstat(CanonPath("/a/x.txt")).has_value());
    EXPECT_FALSE(m->maybeLstat(CanonPath("/a/sub/y.txt")).has_value());
    EXPECT_TRUE(m->maybeLstat(CanonPath("/b.txt")).has_value());
}

TEST(MaskSourceAccessor, ParentOfMaskedPathStillReadable)
{
    /* Listing the parent of a whiteout still succeeds; it just omits
       the whiteout from results. */
    auto base = testHelpers::makeOpFs();
    auto m = makeMask(base, {CanonPath("/a/x.txt")});
    auto entries = m->readDirectory(CanonPath("/a"));
    EXPECT_FALSE(entries.contains("x.txt"));
    EXPECT_TRUE(entries.contains("sub"));
}

/* Concrete witness for the closure-overlap regime where the conditional
   commutativity law's precondition fails.

   Configuration: S = {/a/b}, W = {/a}. Their closures both contain /a
   (closure(S) via ancestor-of-/a/b; closure(W) via /a itself), so the
   closure-disjoint precondition `closure(S) ∩ closure(W) = ∅` is
   violated and the conditional law of the property test below does NOT
   apply.

   Trace at maybeLstat(/a) with /a/b planted on base (so /a exists as a
   real directory in base):

   Stack mr = Mask(W) ∘ Restrict(S)  (Restrict wraps base; Mask outermost)
       outer Mask({/a}): isAdmitted(/a) = !memberOrDescendantOfMember
       ({/a}, /a) = !true = FALSE → outer denies → maybeLstat(/a)
       returns nullopt without ever calling the inner Restrict.
       → nullopt.

   Stack rm = Restrict(S) ∘ Mask(W)  (Mask wraps base; Restrict outermost)
       outer Restrict({/a/b}): isAdmitted(/a) = (paths.contains(/a) ||
       p.isRoot() || ancestorOfMember({/a/b}, /a)) = (false || false ||
       true) = TRUE → forwards to inner Mask. inner Mask({/a}):
       isAdmitted(/a) = FALSE → inner denies → nullopt.
       → nullopt.

   Both stacks return nullopt at /a. The path-set-level preconditions
   intersect, but at the maybeLstat-observation layer both orderings
   converge on nullopt because Mask's deny propagates upward through
   Restrict's ancestor-admit forward (Restrict, post-Phase-1, is a pure
   filter — it does not synthesise stat for ancestors; that role moved
   to DirectorySynthesizerSourceAccessor).

   This test exists primarily to:
     1. Document the configuration that violates the closure-disjoint
        precondition (a future refactor that re-adds Restrict-side
        synthesis, e.g. ancestor stat fabrication, would make the two
        stacks disagree at /a — outer Mask would return nullopt as
        before, but outer Restrict would synthesise a directory stat,
        bypassing the inner Mask's denial). The unit test would fail
        loudly in that case.
     2. Anchor the property-test (below) to the disjoint-closures
        regime: the conditional law requires the precondition because
        the closure-overlap regime is where the algebraic invariants
        of the operator pair stop generalising. */
TEST(MaskSourceAccessor, NonCommutativeWithRestrictWhenClosuresOverlap)
{
    auto base = make_ref<MemorySourceAccessor>();
    base->addFile(CanonPath("a/b"), "value");

    std::set<CanonPath> s{CanonPath("/a/b")};
    std::set<CanonPath> w{CanonPath("/a")};

    /* Sanity-check the precondition violation: /a is in both closures. */
    EXPECT_TRUE(pathSet::inClosure(s, CanonPath("/a")));
    EXPECT_TRUE(pathSet::inClosure(w, CanonPath("/a")));

    /* Both stack orderings, against the same base. */
    auto mr = makeMask(mxMkRestrict(base, s).cast<SourceAccessor>(), w);
    auto rm = mxMkRestrict(makeMask(base, w).cast<SourceAccessor>(), s);

    /* At /a both stacks bottom out in Mask's denial and return nullopt.
       The Restrict-outermost stack forwards through to the inner Mask;
       the Mask-outermost stack denies before reaching the inner
       Restrict. They agree at maybeLstat because Restrict (post-Phase-1)
       does not synthesise — see the long comment above for details and
       the regression scenario this test would catch. */
    auto a = CanonPath("/a");
    EXPECT_FALSE(mr->maybeLstat(a).has_value());
    EXPECT_FALSE(rm->maybeLstat(a).has_value());

    /* At /a/b (member of S, descendant of W), both stacks deny — Mask
       always wins because /a/b is in W's deny closure (descendant of
       /a) and Mask's denial cannot be overridden by Restrict's admit. */
    auto ab = CanonPath("/a/b");
    EXPECT_FALSE(mr->maybeLstat(ab).has_value());
    EXPECT_FALSE(rm->maybeLstat(ab).has_value());
}

#ifndef COVERAGE

/* L5: deny closure includes W and descendants. */
RC_GTEST_PROP(MaskSourceAccessor, DenyClosure, (const std::set<CanonPath> & w, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, w);
    if (!x.isRoot()) {
        try {
            base->addFile(x, "X");
        } catch (Error &) {
        }
    }

    auto m = makeMask(base, w);
    if (!w.empty() && pathSet::memberOrDescendantOfMember(*m->paths, x)) {
        RC_ASSERT(!m->maybeLstat(x).has_value());
    }
}

/* L3c: idempotent. */
RC_GTEST_PROP(MaskSourceAccessor, Idempotent, (const std::set<CanonPath> & w, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, w);
    if (!x.isRoot()) {
        try {
            base->addFile(x, "X");
        } catch (Error &) {
        }
    }
    auto outer = makeMask(makeMask(base, w).cast<SourceAccessor>(), w);
    auto single = makeMask(base, w);
    RC_ASSERT(outer->maybeLstat(x).has_value() == single->maybeLstat(x).has_value());
}

/* L7d analog: bypass on closure(W). */
RC_GTEST_PROP(MaskSourceAccessor, FingerprintBypassesInClosure, (const std::set<CanonPath> & w, const CanonPath & x))
{
    RC_PRE(!w.empty());
    auto base = make_ref<MemorySourceAccessor>();
    auto m = makeMask(base, w);
    auto suffix = m->computeOwnSuffix(x);
    if (pathSet::inClosure(*m->paths, x)) {
        RC_ASSERT(!suffix.has_value());
    } else {
        RC_ASSERT(suffix.has_value() && suffix->empty());
    }
}

/* L3a: Mask ∘ Mask = Mask(W1 ∪ W2) at the deny-set level. */
RC_GTEST_PROP(
    MaskSourceAccessor,
    JoinSemilatticeMorphism,
    (const std::set<CanonPath> & w1, const std::set<CanonPath> & w2, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, w1);
    testHelpers::plantOpFiles(*base, w2);
    if (!x.isRoot()) {
        try {
            base->addFile(x, "X");
        } catch (Error &) {
        }
    }

    std::set<CanonPath> unioned = w1;
    unioned.insert(w2.begin(), w2.end());

    auto stacked = makeMask(makeMask(base, w1).cast<SourceAccessor>(), w2);
    auto fused = makeMask(base, unioned);
    RC_ASSERT(stacked->maybeLstat(x).has_value() == fused->maybeLstat(x).has_value());
}

/* L3d: Mask is commutative. */
RC_GTEST_PROP(
    MaskSourceAccessor,
    Commutative,
    (const std::set<CanonPath> & w1, const std::set<CanonPath> & w2, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, w1);
    testHelpers::plantOpFiles(*base, w2);
    if (!x.isRoot()) {
        try {
            base->addFile(x, "X");
        } catch (Error &) {
        }
    }
    auto m12 = makeMask(makeMask(base, w1).cast<SourceAccessor>(), w2);
    auto m21 = makeMask(makeMask(base, w2).cast<SourceAccessor>(), w1);
    RC_ASSERT(m12->maybeLstat(x).has_value() == m21->maybeLstat(x).has_value());
}

/* L5 converse: paths outside closure(W) pass through unchanged. */
RC_GTEST_PROP(MaskSourceAccessor, PassOutsideClosure, (const std::set<CanonPath> & w, const CanonPath & x))
{
    RC_PRE(!w.empty());
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, w);
    if (!x.isRoot()) {
        try {
            base->addFile(x, "X");
        } catch (Error &) {
        }
    }

    auto m = makeMask(base, w);
    if (!pathSet::memberOrDescendantOfMember(*m->paths, x)) {
        /* Outside deny closure: maybeLstat agrees with base. */
        RC_ASSERT(m->maybeLstat(x).has_value() == base->maybeLstat(x).has_value());
    }
}

/* L3e: Mask distributes over Layer. */
RC_GTEST_PROP(MaskSourceAccessor, DistributesOverLayer, (const std::set<CanonPath> & w, const CanonPath & x))
{
    auto a = make_ref<MemorySourceAccessor>();
    auto b = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*a, w);
    testHelpers::plantOpFiles(*b, w);
    if (!x.isRoot()) {
        try {
            a->addFile(x, "A");
        } catch (Error &) {
        }
    }

    auto lhs = makeMask(makeUnionSourceAccessor({a, b}), w);
    auto ma = makeMask(a, w);
    auto mb = makeMask(b, w);
    auto rhs = makeUnionSourceAccessor({ma, mb});
    RC_ASSERT(lhs->maybeLstat(x).has_value() == rhs->maybeLstat(x).has_value());
}

/* Conditional commutativity of Mask and Restrict (sibling admission
   morphisms): Mask(W) ∘ Restrict(S) ≡ Restrict(S) ∘ Mask(W) at maybeLstat
   when their path-set closures are disjoint, i.e. neither set contains
   a path that lies in the other's closure.

   We use a precondition stronger than (and sufficient for) full
   closure-disjointness: every member of S sits outside closure(W) AND
   every member of W sits outside closure(S). This implies
   closure(S) ∩ closure(W) = ∅ on members; the broader closures (which
   add ancestors and descendants) also stay disjoint because closure is
   monotone under set membership and our quantifier covers the
   generators of each set's closure.

   The precondition rules out the closure-overlap regime exhibited by
   the unit counterexample above. RC_DISCARD on violation rather than
   RC_PRE because the discard rate is high for arbitrary set pairs and
   we want rapidcheck to keep generating until we accumulate enough
   in-regime samples.

   The property documents that under the closure-disjoint regime, Mask
   and Restrict commute observationally — the same kind of equivalence
   asserted by the existing `RestrictSourceAccessor.Commutative` and
   `MaskSourceAccessor.Commutative` PROPs but for the cross-operator
   pair. This is exactly the algebraic shape that justifies treating
   `entries` and `whiteouts` as independent in `sourceViewOverlay`
   (see `src/libfetchers/source-view-factories.cc`): the factory builds
   `Layer([Restrict(E)(o), Mask(W)(b)])` and relies on the two
   operators commuting on disjoint regions. */
RC_GTEST_PROP(
    MaskSourceAccessor,
    CommutesWithRestrictWhenClosuresDisjoint,
    (const std::set<CanonPath> & s, const std::set<CanonPath> & w, const CanonPath & x))
{
    /* Closure-disjoint precondition: no s-member lies in closure(W),
       and no w-member lies in closure(S). Equivalently: each member
       of one set is strictly outside the other set's full closure. */
    for (auto & sp : s) {
        if (pathSet::inClosure(w, sp))
            RC_DISCARD();
    }
    for (auto & wp : w) {
        if (pathSet::inClosure(s, wp))
            RC_DISCARD();
    }

    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, s);
    testHelpers::plantOpFiles(*base, w);
    if (!x.isRoot()) {
        try {
            base->addFile(x, "X");
        } catch (Error &) {
        }
    }

    /* Mask(W) ∘ Restrict(S): Restrict wraps base, Mask is outermost. */
    auto mr = makeMask(mxMkRestrict(base, s).cast<SourceAccessor>(), w);
    /* Restrict(S) ∘ Mask(W): Mask wraps base, Restrict is outermost. */
    auto rm = mxMkRestrict(makeMask(base, w).cast<SourceAccessor>(), s);

    /* Observational equivalence at maybeLstat: both stacks agree on
       admission. We check `.has_value()` rather than full stat-equality
       because the question is whether the path is admitted, not what
       its inode-level metadata is. */
    RC_ASSERT(mr->maybeLstat(x).has_value() == rm->maybeLstat(x).has_value());
}

#endif

} // namespace nix
