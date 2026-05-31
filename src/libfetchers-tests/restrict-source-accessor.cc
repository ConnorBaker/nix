/* Restrict (Prism / commutative idempotent meet-semilattice morphism)
   — Layer-1 admit S∪anc(S) (pure filter, post-Phase-1; ancestor
   synthesis lives in DirectorySynthesizerSourceAccessor), Layer-2
   bypass closure(S). Tests cover L2 (semilattice morphism, idempotent,
   commutative — STRICT post-Phase-1 since synthesis is no longer in
   Restrict), L8 (symlink containment — non-negotiable), and Layer-2
   bypass. Synthesis-dependent tests (AncestorReturnsSynthesisedDirectoryStat,
   AncestorWalkability, ReadDirectoryListsAllowedChildrenAndSynthesisedAncestors)
   were relocated to directory-synthesizer-source-accessor.cc per the
   Phase 1 split. */

#include <exception> // IWYU pragma: keep
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "nix/fetchers/restrict-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-path.hh"
#include "nix/util/tests/source-accessor.hh"

#include "operator-test-helpers.hh"

namespace nix {

namespace {

ref<RestrictSourceAccessor> makeRestrict(ref<SourceAccessor> base, std::set<CanonPath> s)
{
    auto sptr = std::make_shared<const std::set<CanonPath>>(std::move(s));
    return make_ref<RestrictSourceAccessor>(base, sptr, [](const CanonPath & p) -> RestrictedPathError {
        return RestrictedPathError("path '%s' not allowed", p.abs());
    });
}

} // namespace

TEST(RestrictSourceAccessor, AcceptedPathReadable)
{
    auto base = testHelpers::makeOpFs();
    auto r = makeRestrict(base, {CanonPath("/a/x.txt")});
    EXPECT_EQ(r->readFile(CanonPath("/a/x.txt")), "ax");
}

TEST(RestrictSourceAccessor, NonAcceptedPathFiltered)
{
    auto base = testHelpers::makeOpFs();
    auto r = makeRestrict(base, {CanonPath("/a/x.txt")});
    EXPECT_FALSE(r->maybeLstat(CanonPath("/b.txt")).has_value());
}

TEST(RestrictSourceAccessor, AncestorOfMemberAdmittedForwardsToBase)
{
    /* Post-Phase-1: pure-filter Restrict admits ancestors-of-S, but
       does NOT synthesise tDirectory when base lacks the path. With
       the standard fixture, base has /a as a real directory, so
       maybeLstat(/a) forwards to base and returns Some. The
       synthesise-when-base-lacks behaviour now lives in
       DirectorySynthesizer (covered by its own test file). */
    auto base = testHelpers::makeOpFs();
    auto r = makeRestrict(base, {CanonPath("/a/x.txt")});
    auto st = r->maybeLstat(CanonPath("/a"));
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->type, SourceAccessor::tDirectory);
}

TEST(RestrictSourceAccessor, RootIsAlwaysAdmitted)
{
    auto base = testHelpers::makeOpFs();
    auto r = makeRestrict(base, {CanonPath("/a/x.txt")});
    auto st = r->maybeLstat(CanonPath::root);
    ASSERT_TRUE(st.has_value());
}

TEST(RestrictSourceAccessor, EmptySetPassesThrough)
{
    /* Empty-set sentinel: no wrap behaviour at the operator level. */
    auto base = testHelpers::makeOpFs();
    auto r = makeRestrict(base, {});
    EXPECT_TRUE(r->maybeLstat(CanonPath("/b.txt")).has_value());
    EXPECT_EQ(r->readFile(CanonPath("/b.txt")), "b");
}

/* SKETCH §6b — `readLink` is Layer-1 (raw filesystem op) and remains
   transparent through Restrict for member paths even when the target
   is a non-existent path on base. The filter sees the link, not the
   target; this distinguishes Layer-1 reads from `resolveSymlinks`
   which would walk the target and throw. */
TEST(RestrictSourceAccessor, ReadLinkPassesThroughForBrokenSymlinks)
{
    auto base = make_ref<MemorySourceAccessor>();
    /* Root must exist before non-root paths can be planted (see
       `MemorySourceAccessor::open` line 21-22). */
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    /* Plant a broken symlink at /link → /nonexistent. */
    base->open(CanonPath("/link"), MemorySourceAccessor::File{MemorySourceAccessor::File::Symlink{"/nonexistent"}});

    auto r = makeRestrict(base, {CanonPath("/link")});

    /* Filter sees the link, not the target. readLink succeeds even
       though /nonexistent doesn't exist. */
    EXPECT_EQ(r->readLink(CanonPath("/link")), "/nonexistent");

    /* lstat returns the symlink's stat (lstat doesn't follow). */
    auto st = r->maybeLstat(CanonPath("/link"));
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->type, SourceAccessor::tSymlink);
}

#ifndef COVERAGE

/* L8 (non-negotiable): descendants of S MUST NOT be admitted.
   Empty-S is the universe sentinel (admit everything) — exclude it
   from this property since the assertion only models the non-empty
   case. */
RC_GTEST_PROP(RestrictSourceAccessor, SymlinkContainment, (const std::set<CanonPath> & s, const CanonPath & x))
{
    RC_PRE(!s.empty());
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, s);
    if (!x.isRoot()) {
        try {
            base->addFile(x, "Y");
        } catch (Error &) {
        }
    }

    auto r = makeRestrict(base, s);
    bool xInS = s.contains(x);
    bool xIsAncestor = pathSet::ancestorOfMember(s, x);
    if (!xInS && !xIsAncestor && !x.isRoot()) {
        RC_ASSERT(!r->maybeLstat(x).has_value());
    }
}

/* L2c: idempotent. */
RC_GTEST_PROP(RestrictSourceAccessor, Idempotent, (const std::set<CanonPath> & s, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, s);
    auto outer = makeRestrict(makeRestrict(base, s).cast<SourceAccessor>(), s);
    auto single = makeRestrict(base, s);
    RC_ASSERT(outer->maybeLstat(x).has_value() == single->maybeLstat(x).has_value());
}

/* L7d (Layer-2 bypass): paths in closure(S) yield nullopt suffix. */
RC_GTEST_PROP(
    RestrictSourceAccessor, FingerprintBypassesInClosure, (const std::set<CanonPath> & s, const CanonPath & x))
{
    RC_PRE(!s.empty());
    auto base = make_ref<MemorySourceAccessor>();
    auto r = makeRestrict(base, s);
    auto suffix = r->computeOwnSuffix(x);
    if (pathSet::inClosure(*r->paths, x)) {
        RC_ASSERT(!suffix.has_value());
    } else {
        RC_ASSERT(suffix.has_value() && suffix->empty());
    }
}

/* L2d (strict, post-Phase-1): commutative — Restrict(S) ∘ Restrict(T)
   ≡ Restrict(T) ∘ Restrict(S) at maybeLstat for arbitrary x.
   Pre-Phase-1 this property was weakened (`CommutativeOnRealMembers`)
   because Restrict's synthesis branch made local decisions based on
   the wrapper's own paths, not the composed admit set. With synthesis
   moved to DirectorySynthesizer, Restrict is a pure Prism filter and
   the strict law holds. The headline regression-gate of Phase 1. */
RC_GTEST_PROP(
    RestrictSourceAccessor,
    Commutative,
    (const std::set<CanonPath> & s, const std::set<CanonPath> & t, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, s);
    testHelpers::plantOpFiles(*base, t);
    auto st = makeRestrict(makeRestrict(base, s).cast<SourceAccessor>(), t);
    auto ts = makeRestrict(makeRestrict(base, t).cast<SourceAccessor>(), s);
    RC_ASSERT(st->maybeLstat(x).has_value() == ts->maybeLstat(x).has_value());
}

/* L4: Prism identity-match — for x ∈ S, reads pass through unchanged
   for paths that we successfully planted. */
RC_GTEST_PROP(RestrictSourceAccessor, PrismIdentityMatch, (const std::set<CanonPath> & s))
{
    RC_PRE(!s.empty());
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, s);

    auto r = makeRestrict(base, s);
    for (auto & p : s) {
        if (p.isRoot())
            continue;
        /* Skip paths that didn't get planted (parent-was-file collisions);
           those throw on both r->readFile and base->readFile, but the
           specific exception types differ, so just skip. */
        auto st = base->maybeLstat(p);
        if (!st)
            continue;
        /* Skip non-Regular entries: `MemorySourceAccessor::readFile` throws
           `SymlinkNotAllowed` on Symlinks and `NotARegularFile` on
           Directories — the byte-equality assertion below is meaningless
           for those; skip to avoid masking regressions as generator
           noise. */
        if (st->type != SourceAccessor::tRegular)
            continue;
        RC_ASSERT(r->readFile(p) == base->readFile(p));
    }
}

/* L2a: Restrict ∘ Restrict = Restrict(S ∩ T) at the admit-set level.
   This is the strict semilattice morphism law on closure-admit sets. */
RC_GTEST_PROP(
    RestrictSourceAccessor, MeetSemilatticeMorphism, (const std::set<CanonPath> & s, const std::set<CanonPath> & t))
{
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, s);
    testHelpers::plantOpFiles(*base, t);

    /* For paths that are in S ∩ T (and exist on base), both
       Restrict(S)∘Restrict(T) and Restrict(S∩T) admit them. */
    std::set<CanonPath> intersection;
    for (auto & p : s)
        if (t.contains(p))
            intersection.insert(p);

    auto stacked = makeRestrict(makeRestrict(base, s).cast<SourceAccessor>(), t);
    auto fused = makeRestrict(base, intersection);
    for (auto & p : intersection) {
        if (p.isRoot() || !base->maybeLstat(p))
            continue;
        RC_ASSERT(stacked->maybeLstat(p).has_value() == fused->maybeLstat(p).has_value());
    }
}

/* PROP version: root is always admitted under any non-empty Restrict. */
RC_GTEST_PROP(RestrictSourceAccessor, RootAlwaysAllowed, (const std::set<CanonPath> & s))
{
    auto base = make_ref<MemorySourceAccessor>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    auto r = makeRestrict(base, s);
    RC_ASSERT(r->maybeLstat(CanonPath::root).has_value());
}

/* L2e: Restrict distributes over Layer. */
RC_GTEST_PROP(RestrictSourceAccessor, DistributesOverLayer, (const std::set<CanonPath> & s, const CanonPath & x))
{
    auto a = make_ref<MemorySourceAccessor>();
    auto b = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*a, s);
    testHelpers::plantOpFiles(*b, s);

    auto lhs = makeRestrict(makeUnionSourceAccessor({a, b}).cast<SourceAccessor>(), s);
    auto ra = makeRestrict(a, s);
    auto rb = makeRestrict(b, s);
    auto rhs = makeUnionSourceAccessor({ra, rb});

    /* Both should agree on isAllowed observably. */
    RC_ASSERT(lhs->maybeLstat(x).has_value() == rhs->maybeLstat(x).has_value());
}

/* SKETCH §6a — L8 "symlink containment" in its named form: a member
   of S that is a *symlink* on base; the filter must deny strict
   descendants of that member.

   Uses the correlated `Arbitrary<TreeAndSet>` generator (every
   member of `s` exists on base). Rather than draw `x` independently
   and hope it's a descendant of a symlink-in-s, we **derive `x`
   from `s`**: find a symlink in s, append a single component, and
   use that as the descendant. The precondition holds by
   construction; rapidcheck variability remains via the choice of
   tree, s, and the descendant component. */
RC_GTEST_PROP(
    RestrictSourceAccessor, SymlinkContainmentStrict, (const rc::TreeAndSet & ts, const std::string & descName))
{
    /* descName must be a single canonical component. */
    RC_PRE(!descName.empty());
    RC_PRE(descName.find('/') == std::string::npos);
    RC_PRE(descName != "." && descName != "..");

    /* Find any symlink in s. The TreeAndSet generator is configured
       to produce ~50% symlinks, so this should succeed for most
       samples where s is non-empty. */
    auto base = make_ref<MemorySourceAccessor>(ts.tree);
    std::optional<CanonPath> linkInS;
    for (auto & m : ts.s) {
        std::optional<SourceAccessor::Stat> baseSt;
        try {
            baseSt = base->maybeLstat(m);
        } catch (Error &) {
            continue;
        }
        if (baseSt && baseSt->type == SourceAccessor::tSymlink) {
            linkInS = m;
            break;
        }
    }
    RC_PRE(linkInS.has_value());

    auto r = makeRestrict(base, ts.s);

    /* x is a strict descendant of a symlink-in-s by construction. */
    auto x = *linkInS / descName;
    RC_PRE(!ts.s.contains(x)); // (rare) descName might collide with a planted name

    /* L8: descendant of a symlink in S is denied. */
    RC_ASSERT(!r->maybeLstat(x).has_value());
}

/* SKETCH §6c — when `base->resolveSymlinks(p, Full)` lands at a path
   admitted by `S`, `r->resolveSymlinks(p, Full)` agrees byte-for-byte.

   Implemented as a UNIT rather than a PROP because the precondition
   ("Restrict admits every intermediate hop in the symlink walk")
   resists random sampling — under arbitrary S, the mid-walk
   admission gates fire too often to make the precondition fire
   without effectively pre-computing the resolution path. The UNIT
   pins the algebraic content directly: link to a Regular target,
   both admitted, both resolve identically. */
TEST(Algebra, SymlinkResolutionAgreesOnAcceptedTargets)
{
    auto base = make_ref<MemorySourceAccessor>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    base->open(CanonPath("/target"), MemorySourceAccessor::File{MemorySourceAccessor::File::Regular{}});
    base->open(CanonPath("/link"), MemorySourceAccessor::File{MemorySourceAccessor::File::Symlink{"/target"}});

    /* Admit both link and target — Restrict's resolveSymlinks needs
       to admit each intermediate hop. */
    auto r = makeRestrict(base, {CanonPath("/link"), CanonPath("/target")});

    auto baseResolved = base->resolveSymlinks(CanonPath("/link"), SymlinkResolution::Full);
    auto rResolved = r->resolveSymlinks(CanonPath("/link"), SymlinkResolution::Full);

    EXPECT_EQ(baseResolved, CanonPath("/target"));
    EXPECT_EQ(rResolved, baseResolved);
}

/* SKETCH §6d — for `p ∈ S` that is a symlink on base,
   `r->maybeLstat(p)` returns `tSymlink`, NOT a synthesised
   `tDirectory`. This distinguishes "member of S preserved as-is"
   (Prism identity) from "ancestor of S synthesised as tDirectory"
   (L4). Catches the regression where a Prism implementation
   accidentally synthesises directory stats over the top of every
   member. */
RC_GTEST_PROP(RestrictSourceAccessor, MaybeLstatPreservesSymlinkType, (const rc::TreeAndSet & ts))
{
    RC_PRE(!ts.s.empty());

    auto base = make_ref<MemorySourceAccessor>(ts.tree);
    auto r = makeRestrict(base, ts.s);
    auto & s = ts.s;

    for (auto & p : s) {
        /* Skip paths whose ancestor on base is itself a symlink —
           `base->maybeLstat(p)` throws `SymlinkNotAllowed` in that
           case, and `r->maybeLstat(p)` would propagate the same
           throw (Restrict is `isAllowed`-gate then forward to base).
           The PROP assertion is only meaningful for paths whose
           lstat-on-base actually returned a symlink. */
        std::optional<SourceAccessor::Stat> baseSt;
        try {
            baseSt = base->maybeLstat(p);
        } catch (Error &) {
            continue;
        }
        if (!baseSt || baseSt->type != SourceAccessor::tSymlink)
            continue;
        auto rSt = r->maybeLstat(p);
        RC_ASSERT(rSt.has_value());
        RC_ASSERT(rSt->type == SourceAccessor::tSymlink);
    }
}

#endif

} // namespace nix
