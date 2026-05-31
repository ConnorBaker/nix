/* DirectorySynthesizer (Prism-synthesis sub-operator)
   — D1-D13 plus factory and cross-combinator tests per
   doc/tecnix-survey/PROPOSAL.md §3 (the "D" bullet) + §2.Q.

   D belongs to the admission morphism family alongside Restrict and
   Mask, but its laws differ: it's a join-semilattice morphism on
   path-sets (D3 fusion) at Layer-1, transparent at Layer-2 (D11). */

#include <exception> // IWYU pragma: keep
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "nix/fetchers/directory-synthesizer-source-accessor.hh"
#include "nix/fetchers/restrict-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-path.hh"
#include "nix/util/tests/source-accessor.hh"

#include "operator-test-helpers.hh"

namespace nix {

namespace {

/* Local file-prefixed names so unity-build doesn't collide with similarly-
   named helpers in other test .cc files. */
ref<DirectorySynthesizerSourceAccessor> dsMkD(ref<SourceAccessor> base, std::set<CanonPath> s)
{
    auto sptr = std::make_shared<const std::set<CanonPath>>(std::move(s));
    return make_ref<DirectorySynthesizerSourceAccessor>(base, sptr, testHelpers::makeOpErr());
}

ref<RestrictSourceAccessor> dsMkR(ref<SourceAccessor> base, std::set<CanonPath> s)
{
    auto sptr = std::make_shared<const std::set<CanonPath>>(std::move(s));
    return make_ref<RestrictSourceAccessor>(base, sptr, testHelpers::makeOpErr());
}

} // namespace

/* ---- Concrete-fixture UNIT tests (the named scenarios from §5) ---- */

TEST(DirectorySynthesizer, AncestorOfMemberSynthesisedAsDirectory)
{
    /* Concrete L4-equivalent (relocated from restrict-source-accessor.cc):
       D({/a/x.txt}) over a base that lacks /a → maybeLstat(/a) returns
       Some(tDirectory). With the standard fixture (which DOES have /a),
       D forwards to base; the synthesised case requires a base without
       the intermediate dir. */
    auto base = make_ref<MemorySourceAccessor>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    auto d = dsMkD(base, {CanonPath("/a/x.txt")});

    auto stA = d->maybeLstat(CanonPath("/a"));
    ASSERT_TRUE(stA.has_value());
    EXPECT_EQ(stA->type, SourceAccessor::tDirectory);

    /* Root forwards to base (which has it). */
    EXPECT_TRUE(d->maybeLstat(CanonPath::root).has_value());
}

/* Regression: a path that is BOTH a member of S and a strict ancestor
   of a deeper member must still be synthesised when `next` lacks it.
   With S = {/a, /a/a} and a base missing /a, maybeLstat(/a) must return
   Some(tDirectory) so the walk can reach the accepted leaf /a/a.

   This exercised a real bug in `pathSet::ancestorOfMember`: its
   `lower_bound(p)` landed on the member `p` (=/a) and the old code bailed
   on `*it == p`, never advancing to the descendant /a/a — so /a was NOT
   synthesised. (Masked in Restrict::isAllowed by a preceding
   `paths->contains(p)` check, but live here — D::maybeLstat has no such
   guard.) Fixed by advancing past `p` in `ancestorOfMember`; pinned at
   the predicate level by PathSet.AncestorOfMemberMatchesReference. */
TEST(DirectorySynthesizer, MemberThatIsAlsoAncestorIsSynthesised)
{
    auto base = make_ref<MemorySourceAccessor>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    auto d = dsMkD(base, {CanonPath("/a"), CanonPath("/a/a")});

    /* /a is a member of S AND the ancestor of the member /a/a; base
       lacks it, so D must synthesise it as a directory. */
    auto stA = d->maybeLstat(CanonPath("/a"));
    ASSERT_TRUE(stA.has_value());
    EXPECT_EQ(stA->type, SourceAccessor::tDirectory);
}

TEST(DirectorySynthesizer, ForwardsToBaseWhenBaseHasPath)
{
    /* When base has the path, D forwards (D8 first case) — even if
       the path is in S. */
    auto base = testHelpers::makeOpFs();
    auto d = dsMkD(base, {CanonPath("/a/x.txt")});

    auto st = d->maybeLstat(CanonPath("/a/x.txt"));
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->type, SourceAccessor::tRegular);

    auto stA = d->maybeLstat(CanonPath("/a"));
    ASSERT_TRUE(stA.has_value());
    EXPECT_EQ(stA->type, SourceAccessor::tDirectory);
}

TEST(DirectorySynthesizer, ReadDirectoryMergesAncestors)
{
    /* Concrete D9: real children + synthesised intermediate-dir
       children from S. base has no /a entirely; D synthesises it
       from S = {/a/x.txt}. */
    auto base = make_ref<MemorySourceAccessor>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    base->addFile(CanonPath("/b.txt"), "b");
    auto d = dsMkD(base, {CanonPath("/a/x.txt")});

    auto rootEntries = d->readDirectory(CanonPath::root);
    EXPECT_TRUE(rootEntries.contains("a"));
    EXPECT_TRUE(rootEntries.contains("b.txt"));
    /* Synthesised "a" should be tagged as Directory since it's
       intermediate (cur != e in the walk-up). */
    auto aTy = rootEntries.find("a");
    ASSERT_NE(aTy, rootEntries.end());
    ASSERT_TRUE(aTy->second.has_value());
    EXPECT_EQ(*aTy->second, SourceAccessor::tDirectory);
}

TEST(DirectorySynthesizer, IdentityShortCircuit)
{
    /* D1 (factory): empty paths returns base unchanged. */
    auto base = testHelpers::makeOpFs();
    auto sptr = std::make_shared<const std::set<CanonPath>>();
    auto out = makeDirectorySynthesizer(base, sptr, testHelpers::makeOpErr());
    EXPECT_EQ(out.get_ptr(), base.get_ptr());
}

TEST(DirectorySynthesizer, MorphismFusionAtFactory)
{
    /* D3 (factory): D(S1) ∘ D(S2) fuses to a single wrapper with
       paths S1 ∪ S2. Verifiable structurally via dynamic_pointer_cast. */
    auto base = testHelpers::makeOpFs();
    auto inner = makeDirectorySynthesizer(
        base,
        std::make_shared<const std::set<CanonPath>>(std::set<CanonPath>{CanonPath("/a/x.txt")}),
        testHelpers::makeOpErr());
    auto outer = makeDirectorySynthesizer(
        inner,
        std::make_shared<const std::set<CanonPath>>(std::set<CanonPath>{CanonPath("/b.txt")}),
        testHelpers::makeOpErr());

    auto fused = outer.dynamic_pointer_cast<DirectorySynthesizerSourceAccessor>();
    ASSERT_TRUE(fused);
    /* Fused wrapper holds the union of both sets. */
    EXPECT_EQ(fused->paths->size(), 2u);
    EXPECT_TRUE(fused->paths->contains(CanonPath("/a/x.txt")));
    EXPECT_TRUE(fused->paths->contains(CanonPath("/b.txt")));
    /* Inner wrapper is collapsed: outer's `next` is base, not inner. */
    EXPECT_NE(fused->next.get_ptr(), inner.get_ptr());
}

TEST(DirectorySynthesizer, NonCommutativeWithRestrict)
{
    /* D12 (concrete counter-example to D ∘ R commuting): with
       S_R = {/}, S_D = {/a/b}, base = {root only}, the two stack
       orders produce different readDirectory(/) outputs.

       - D({/a/b}) ∘ R({/})(base).readDirectory(/):
         R admits root; iterates `next.readDirectory(/) = base...(/)`
         which is empty; filters → still empty. D wraps and synthesises
         "a" as tDirectory from its walk-up of {/a/b}. Result: {a:tDir}.
       - R({/}) ∘ D({/a/b})(base).readDirectory(/):
         R admits root; iterates next = D's readDirectory; D returns
         {a:tDir}. R then filters by `isAllowed(/a)` against S_R={/}.
         /a ∉ S_R, /a is not ancestor of /, /a is not root → denied.
         Result: {}. */
    auto base = make_ref<MemorySourceAccessor>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});

    auto restrictRoot = dsMkR(base, {CanonPath::root});
    auto dOverR = dsMkD(restrictRoot.cast<SourceAccessor>(), {CanonPath("/a/b")});
    auto entries1 = dOverR->readDirectory(CanonPath::root);
    EXPECT_TRUE(entries1.contains("a"));

    auto dOverBase = dsMkD(base, {CanonPath("/a/b")});
    auto rOverD = dsMkR(dOverBase.cast<SourceAccessor>(), {CanonPath::root});
    auto entries2 = rOverD->readDirectory(CanonPath::root);
    EXPECT_FALSE(entries2.contains("a"));
}

/* ---- Relocated Stage-0 pin tests (originally in restrict-source-accessor.cc)
   These verify that D ∘ R produces the same ancestor-walkability
   semantics the old combined Restrict did. Phase 1 extracted
   synthesis from Restrict into D; these pins ensure the composed
   stack (which is what sourceViewSubset builds) still satisfies
   the user-visible contracts. ---- */

/* AncestorReturnsSynthesisedDirectoryStat (formerly in restrict-source-accessor.cc):
   D ∘ R over a base that DOES have the ancestor forwards to base.
   Post-Phase-1 this is the observable contract: pure-filter Restrict
   admits /a as ancestor of /a/x.txt, D forwards to base since base
   has it. The synthesise-when-base-lacks case is tested separately by
   AncestorOfMemberSynthesisedAsDirectory above (direct D, no R). */
TEST(DirectorySynthesizer, AncestorReturnsSynthesisedDirectoryStat)
{
    /* Standard fixture: base has /a as a real directory.
       D ∘ R({/a/x.txt}) admits /a as ancestor; D forwards to base
       and returns the real stat (tDirectory from base). */
    auto base = testHelpers::makeOpFs();
    auto r = dsMkR(base, {CanonPath("/a/x.txt")});
    auto d = dsMkD(r.cast<SourceAccessor>(), {CanonPath("/a/x.txt")});
    auto st = d->maybeLstat(CanonPath("/a"));
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->type, SourceAccessor::tDirectory);
}

/* AncestorWalkability: D ∘ R({S})(base).pathExists(ancestor) is true
   for arbitrary S and ancestor-of-S, even when base would not expose
   the ancestor via Restrict alone. Tests D7 in composition. */
RC_GTEST_PROP(DirectorySynthesizer, AncestorWalkability, (const std::set<CanonPath> & s))
{
    RC_PRE(!s.empty());

    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, s);

    /* Build D ∘ R, the composition sourceViewSubset uses. */
    auto r = dsMkR(base, s);
    auto d = dsMkD(r.cast<SourceAccessor>(), s);

    for (auto & m : s) {
        if (m.isRoot())
            continue;
        if (!base->maybeLstat(m))
            continue;
        /* Walk up m's strict ancestors: each should be accessible. */
        auto cur = m;
        while (auto parent = cur.parent()) {
            cur = *parent;
            if (s.contains(cur))
                break;
            /* D ∘ R must expose every ancestor of S. */
            RC_ASSERT(d->maybeLstat(cur).has_value());
            if (cur.isRoot())
                break;
        }
    }
}

/* ReadDirectoryListsAllowedChildrenAndSynthesisedAncestors:
   D ∘ R({/a/sub/y.txt})(base).readDirectory(/) lists 'a' (synthesised
   ancestor) but NOT 'b.txt' (outside S). readDirectory(/a) lists
   'sub' but NOT 'x.txt'. Tests D9 in composition. */
TEST(DirectorySynthesizer, ReadDirectoryListsAllowedChildrenAndSynthesisedAncestors)
{
    /* Standard fixture: base has /a/x.txt, /a/sub/y.txt, /b.txt. */
    auto base = testHelpers::makeOpFs();
    auto r = dsMkR(base, {CanonPath("/a/sub/y.txt")});
    auto d = dsMkD(r.cast<SourceAccessor>(), {CanonPath("/a/sub/y.txt")});

    /* Root listing: 'a' synthesised as ancestor, 'b.txt' filtered. */
    auto rootEntries = d->readDirectory(CanonPath::root);
    EXPECT_TRUE(rootEntries.contains("a"));
    EXPECT_FALSE(rootEntries.contains("b.txt"));

    /* /a listing: 'sub' synthesised as ancestor, 'x.txt' filtered. */
    auto aEntries = d->readDirectory(CanonPath("/a"));
    EXPECT_TRUE(aEntries.contains("sub"));
    EXPECT_FALSE(aEntries.contains("x.txt"));
}

TEST(DirectorySynthesizer, SymlinkInSPreservesType)
{
    /* D13: when S contains a symlink path on base, readDirectory's
       synthesis loop calls `next->maybeLstat` on the terminal and
       uses base's type — never the fallback tDirectory. D's job is
       type-preservation in synthesis; descendants-never-admitted is
       Restrict's L8 (orthogonal — L8 holds upstream of D, regardless
       of how D fills the listing). */
    auto base = make_ref<MemorySourceAccessor>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    base->open(CanonPath("/link"), MemorySourceAccessor::File{MemorySourceAccessor::File::Symlink{"target"}});

    auto d = dsMkD(base, {CanonPath("/link")});
    auto entries = d->readDirectory(CanonPath::root);
    auto linkEntry = entries.find("link");
    ASSERT_NE(linkEntry, entries.end());
    ASSERT_TRUE(linkEntry->second.has_value());
    EXPECT_EQ(*linkEntry->second, SourceAccessor::tSymlink);
}

#ifndef COVERAGE

/* ---- D-law inventory PROP tests ---- */

/* D1: D(∅) ≡ Identity — empty path-set behaves observably as no
   wrap. The factory short-circuit (UNIT IdentityShortCircuit) is
   the structural form; this is the observable form. */
RC_GTEST_PROP(DirectorySynthesizer, IdentityElement, (const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    if (!x.isRoot()) {
        try {
            base->addFile(x, "X");
        } catch (Error &) {
        }
    }
    auto d = dsMkD(base, {});
    /* Empty-set D admits everything via base; no synthesis. */
    RC_ASSERT(d->maybeLstat(x).has_value() == base->maybeLstat(x).has_value());
}

/* D8 neither-case (negative): paths that are neither in S, nor ancestors
   of S, nor descendants of anything in base should not be spuriously
   synthesised. D(S)(b).maybeLstat(p) must equal b.maybeLstat(p) when
   p is outside the admit-set. Pinning this prevents D from accidentally
   admitting paths it has no business admitting. */
RC_GTEST_PROP(DirectorySynthesizer, NoSpuriousSynthesis, (const std::set<CanonPath> & s, const CanonPath & x))
{
    /* Exclude root (always admitted) and any path that IS an ancestor
       of S (those are synthesised intentionally). */
    RC_PRE(!s.empty());
    RC_PRE(!x.isRoot());
    RC_PRE(!pathSet::ancestorOfMember(s, x));
    RC_PRE(!s.contains(x));

    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, s);
    /* Do NOT plant x in base so base.maybeLstat(x) = nullopt. */

    auto d = dsMkD(base, s);
    /* D must not synthesise x since it's neither a member nor an
       ancestor of any member. */
    RC_ASSERT(!d->maybeLstat(x).has_value());
}

/* D2: D(S)(D(S)(b)) ≡ D(S)(b) — idempotent. */
RC_GTEST_PROP(DirectorySynthesizer, Idempotent, (const std::set<CanonPath> & s, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, s);

    auto stacked = dsMkD(dsMkD(base, s).cast<SourceAccessor>(), s);
    auto single = dsMkD(base, s);
    RC_ASSERT(stacked->maybeLstat(x).has_value() == single->maybeLstat(x).has_value());
}

/* D3 observable form: D(S1) ∘ D(S2) agrees with D(S1 ∪ S2)
   on `maybeLstat`. The factory-level structural fusion is
   covered by the UNIT test above. */
RC_GTEST_PROP(
    DirectorySynthesizer,
    JoinSemilatticeMorphism,
    (const std::set<CanonPath> & s1, const std::set<CanonPath> & s2, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, s1);
    testHelpers::plantOpFiles(*base, s2);

    std::set<CanonPath> unioned = s1;
    unioned.insert(s2.begin(), s2.end());

    auto stacked = dsMkD(dsMkD(base, s1).cast<SourceAccessor>(), s2);
    auto fused = dsMkD(base, unioned);
    RC_ASSERT(stacked->maybeLstat(x).has_value() == fused->maybeLstat(x).has_value());
}

/* D4: commutative — D(S) ∘ D(T) ≡ D(T) ∘ D(S) on `maybeLstat`. */
RC_GTEST_PROP(
    DirectorySynthesizer,
    Commutative,
    (const std::set<CanonPath> & s, const std::set<CanonPath> & t, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, s);
    testHelpers::plantOpFiles(*base, t);

    auto st = dsMkD(dsMkD(base, s).cast<SourceAccessor>(), t);
    auto ts = dsMkD(dsMkD(base, t).cast<SourceAccessor>(), s);
    RC_ASSERT(st->maybeLstat(x).has_value() == ts->maybeLstat(x).has_value());
}

/* D5: D(S)(b).readFile(p) = b.readFile(p) for paths base has — read
   pass-through on readFile. We only test paths actually planted on
   base; symlink paths and missing paths throw on both sides with
   different exception types. */
RC_GTEST_PROP(DirectorySynthesizer, ReadPassthroughReadFile, (const std::set<CanonPath> & s))
{
    RC_PRE(!s.empty());
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, s);

    auto d = dsMkD(base, s);
    for (auto & p : s) {
        if (p.isRoot())
            continue;
        auto st = base->maybeLstat(p);
        if (!st || st->type != SourceAccessor::tRegular)
            continue;
        RC_ASSERT(d->readFile(p) == base->readFile(p));
    }
}

/* D6: D(S)(b).readLink(p) = b.readLink(p) for paths base has —
   read pass-through on readLink. We only test paths that base has
   as symlinks. */
RC_GTEST_PROP(DirectorySynthesizer, ReadPassthroughReadLink, (const std::set<CanonPath> & s))
{
    RC_PRE(!s.empty());
    auto base = make_ref<MemorySourceAccessor>();
    /* Plant some of S as symlinks so the readLink path is exercised. */
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    for (auto & p : s) {
        if (p.isRoot())
            continue;
        try {
            base->open(p, MemorySourceAccessor::File{MemorySourceAccessor::File::Symlink{"target"}});
        } catch (Error &) {
            /* parent-is-Regular collision — skip. */
        }
    }

    auto d = dsMkD(base, s);
    for (auto & p : s) {
        if (p.isRoot())
            continue;
        /* Skip paths whose parent component is itself a symlink —
           `MemorySourceAccessor::open` throws SymlinkNotAllowed in
           that case and both sides would propagate the same throw,
           but the test fixture is more readable if we don't trip
           the `RC_ASSERT` apparatus on equivalent throws. */
        std::optional<SourceAccessor::Stat> st;
        try {
            st = base->maybeLstat(p);
        } catch (Error &) {
            continue;
        }
        if (!st || st->type != SourceAccessor::tSymlink)
            continue;
        RC_ASSERT(d->readLink(p) == base->readLink(p));
    }
}

/* D7: pathExists covers ancestors of S even when base lacks them.
   Inherited via the default pathExists → maybeLstat.has_value(). */
RC_GTEST_PROP(DirectorySynthesizer, PathExistsCovers, (const std::set<CanonPath> & s, const CanonPath & p))
{
    RC_PRE(!s.empty());
    auto base = make_ref<MemorySourceAccessor>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    /* Don't plant s; we want to test the ancestor synthesis case. */

    auto d = dsMkD(base, s);
    bool baseHas = base->pathExists(p);
    bool dHas = d->pathExists(p);
    bool isAnc = pathSet::ancestorOfMember(s, p);

    /* D's pathExists ⇔ base has it OR p is ancestor of S. */
    RC_ASSERT(dHas == (baseHas || isAnc));
}

/* D8 (ancestor case): for p ∉ base ∧ ancestor-of-S, maybeLstat returns
   Some(tDirectory). */
RC_GTEST_PROP(DirectorySynthesizer, MaybeLstatSynthesisesAncestor, (const std::set<CanonPath> & s, const CanonPath & p))
{
    RC_PRE(!s.empty());
    auto base = make_ref<MemorySourceAccessor>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});

    auto d = dsMkD(base, s);
    /* Only test the ancestor case where base genuinely lacks p
       (the synthesis branch). */
    if (!base->maybeLstat(p) && pathSet::ancestorOfMember(s, p)) {
        auto st = d->maybeLstat(p);
        RC_ASSERT(st.has_value());
        RC_ASSERT(st->type == SourceAccessor::tDirectory);
    }
}

/* D8 (passthrough case): for p that base has, D forwards `next`'s answer. */
RC_GTEST_PROP(DirectorySynthesizer, MaybeLstatPassThroughTerminal, (const std::set<CanonPath> & s, const CanonPath & p))
{
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, s);
    if (!p.isRoot()) {
        try {
            base->addFile(p, "P");
        } catch (Error &) {
        }
    }

    auto d = dsMkD(base, s);
    auto baseSt = base->maybeLstat(p);
    if (baseSt) {
        auto dSt = d->maybeLstat(p);
        RC_ASSERT(dSt.has_value());
        RC_ASSERT(dSt->type == baseSt->type);
    }
}

/* D9: D(S)(b).readDirectory(p) merges base's children with
   synthesised intermediate-dir children from S. Property form: any
   member of S that has a strict descendant chain through `p` — i.e.
   whose direct-child-of-`p` ancestor is an INTERMEDIATE directory
   (not the member itself) — surfaces as a synthesised entry; any
   child base has remains in the listing.

   Note the intermediate-only restriction (SV-3): a leaf member of S
   that is itself a direct child of `p` and absent from base is NOT
   synthesised, because it would advertise a child that maybeLstat
   cannot stat (members are excluded from ancestorOfMember) — a
   readDirectory/lstat divergence. Only intermediate ancestors
   (cur != e), which maybeLstat reports as directories, are listed. */
RC_GTEST_PROP(
    DirectorySynthesizer, ReadDirectoryMergesAncestorsProp, (const std::set<CanonPath> & s, const CanonPath & p))
{
    auto base = make_ref<MemorySourceAccessor>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    /* Don't plant S — focus on the synthesis branch. */

    auto d = dsMkD(base, s);
    SourceAccessor::DirEntries entries;
    try {
        entries = d->readDirectory(p);
    } catch (Error &) {
        RC_DISCARD();
    }

    /* For each direct-child-of-p in S, the entry should be in
       the listing. Walk-up s to find which descend from p. */
    for (auto & e : s) {
        if (e == p)
            continue;
        if (!e.isWithin(p))
            continue;
        /* Find e's ancestor whose parent is p. */
        auto cur = e;
        std::optional<std::string> directChildName;
        while (auto parent = cur.parent()) {
            if (*parent == p) {
                /* Only intermediate ancestors are synthesised (SV-3);
                   a leaf member (cur == e) that is a direct child of p
                   is deliberately omitted, so don't assert it. */
                if (cur != e)
                    if (auto bn = cur.baseName())
                        directChildName = std::string(*bn);
                break;
            }
            if (parent->isRoot())
                break;
            cur = *parent;
        }
        if (directChildName)
            RC_ASSERT(entries.contains(*directChildName));
    }
}

/* D11: Layer-2 transparent. computeOwnSuffix is empty for all paths
   (D never bypasses fingerprint — synthesis is content-neutral). */
RC_GTEST_PROP(DirectorySynthesizer, FingerprintTransparent, (const std::set<CanonPath> & s, const CanonPath & p))
{
    auto base = make_ref<MemorySourceAccessor>();
    base->fingerprint = "tree:R";
    auto d = dsMkD(base, s);
    auto suffix = d->computeOwnSuffix(p);
    RC_ASSERT(suffix.has_value() && suffix->empty());
}

/* D10: D distributes over Layer (Union). */
RC_GTEST_PROP(DirectorySynthesizer, DistributesOverLayer, (const std::set<CanonPath> & s, const CanonPath & x))
{
    auto a = make_ref<MemorySourceAccessor>();
    auto b = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*a, s);
    testHelpers::plantOpFiles(*b, s);

    auto lhs = dsMkD(makeUnionSourceAccessor({a, b}), s);
    auto da = dsMkD(a, s);
    auto db = dsMkD(b, s);
    auto rhs = makeUnionSourceAccessor({da.cast<SourceAccessor>(), db.cast<SourceAccessor>()});
    RC_ASSERT(lhs->maybeLstat(x).has_value() == rhs->maybeLstat(x).has_value());
}

/* ---- Cross-combinator tests ---- */

/* The headline regression-gate of Phase 1: stacked Restricts have
   strict commutativity at maybeLstat for arbitrary x once synthesis
   is extracted from Restrict. */
RC_GTEST_PROP(
    DirectorySynthesizer,
    OuterMostRestoresCommutativity,
    (const std::set<CanonPath> & s, const std::set<CanonPath> & t, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    testHelpers::plantOpFiles(*base, s);
    testHelpers::plantOpFiles(*base, t);

    /* Stacked Restricts (no D) — Restrict is now a pure filter, so
       commutativity holds at maybeLstat for arbitrary x. */
    auto st = dsMkR(dsMkR(base, s).cast<SourceAccessor>(), t);
    auto ts = dsMkR(dsMkR(base, t).cast<SourceAccessor>(), s);
    RC_ASSERT(st->maybeLstat(x).has_value() == ts->maybeLstat(x).has_value());
}

#endif

} // namespace nix
