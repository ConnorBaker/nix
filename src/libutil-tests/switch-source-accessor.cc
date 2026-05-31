/* Switch (longest-prefix-match, authoritative-commit) — the categorical
   dual of Layer (Alternative/fall-through). Plus the mutable face,
   MountedSourceAccessor.

   The headline law is the distinction from Layer: a Switch commits to
   the nearest-prefix mount's answer (a miss is final), whereas a Layer
   would fall through to a shorter-prefix child. This is the correct
   mount semantics — and the reason MountedSourceAccessor is NOT a
   Layer([StripPrefix...]). See doc/tecnix-survey/PROPOSAL.md §6.7.

   Property tests (the category theory tells us what to check):
   - Sw1 (resolution law): ∀ mounts,x — read == nearest-prefix owner's
     read at the stripped path (the operational spec of LPM-commit).
   - Sw2 (authoritative ≠ fall-through): ∀ shadowing configs — Switch
     commits to the owner's miss; the Layer of the same data does not.
   - Sw3 (per-branch ≡ StripPrefix-shaped strip): ∀ mountPoint,tree,y —
     reads + fingerprint at p/y equal the inner read/fingerprint at y.
     (The libfetchers StripPrefix-operator equivalence proper is in
     algebra-cross-cutting.cc::SwitchBranchEqualsStripPrefix.) */

#include "nix/util/switch-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/union-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/tests/source-accessor.hh"

#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

namespace nix {

namespace {

/* ---------- ERROR-CHANNEL test double (L-ErrPreserve / L-Dual) ----------

   A leaf whose throwing read methods raise a *bespoke* exception type
   carrying a unique nonce in its message. This lets the error-channel
   laws assert not merely that "something threw" (the Maybe-equivalence
   that every existing `catch (Error&); compare bool threw` property
   collapses to) but that the EXACT dynamic type and the inner accessor's
   own message survived the combinator unchanged (the Either-equivalence
   that the §6.8.1 Item-5 regression violated: a Union masked the git
   workdir's RestrictedPathError as a generic FileNotFound).

   `CustomBespokeError` sits under `SourceAccessorError` (a sibling of
   `FileNotFound`) so it is distinguishable from the `FileNotFound` that
   Union/Layer substitutes on a miss, yet still a `SourceAccessorError`
   (proving the law isn't trivially satisfied by an unrelated hierarchy). */
MakeError(CustomBespokeError, SourceAccessorError);

/* For a single designated path `p`:
     - maybeLstat(p) → nullopt   (the non-throwing probe finds nothing,
       so a maybeLstat-then-read combinator like Union would mask);
     - readFile/readLink/readDirectory/lstat(p) → throw CustomBespokeError
       whose message is DERIVED FROM the captured nonce.
   The root is a well-formed directory so the accessor is valid to mount
   and so resolveMount's ancestor walk terminates. Any path other than
   `p` simply misses (nullopt / throws the generic base FileNotFound),
   which is irrelevant to these laws. */
struct ThrowingLeaf : SourceAccessor
{
    CanonPath p;
    std::string nonce;

    ThrowingLeaf(CanonPath p, std::string nonce)
        : p(std::move(p))
        , nonce(std::move(nonce))
    {
        displayPrefix.clear();
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        if (path.isRoot())
            return Stat{.type = tDirectory};
        return std::nullopt; // including at `p`: the silent-probe miss
    }

    void readFile(const CanonPath & path, Sink &, fun<void(uint64_t)>) override
    {
        if (path == p)
            throw CustomBespokeError("bespoke readFile nonce %s", nonce);
        throw FileNotFound("no such path '%s'", showPath(path));
    }

    Stat lstat(const CanonPath & path) override
    {
        if (path == p)
            throw CustomBespokeError("bespoke lstat nonce %s", nonce);
        return SourceAccessor::lstat(path); // throws generic FileNotFound
    }

    std::string readLink(const CanonPath & path) override
    {
        if (path == p)
            throw CustomBespokeError("bespoke readLink nonce %s", nonce);
        throw FileNotFound("no such path '%s'", showPath(path));
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        if (path == p)
            throw CustomBespokeError("bespoke readDirectory nonce %s", nonce);
        throw FileNotFound("no such path '%s'", showPath(path));
    }
};

ref<MemorySourceAccessor> withFiles(std::initializer_list<std::pair<std::string, std::string>> files)
{
    auto m = make_ref<MemorySourceAccessor>();
    for (auto & [p, content] : files)
        m->addFile(CanonPath(p), std::string(content));
    return m;
}

ref<MemorySourceAccessor> emptyDir()
{
    auto m = make_ref<MemorySourceAccessor>();
    m->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    return m;
}

/* A leaf whose getPhysicalPath returns a recognisable value at the
   stripped path, so we can verify Switch strips before forwarding. */
struct CheckoutPathLeaf : MemorySourceAccessor
{
    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        return std::filesystem::path("/phys") / path.rel();
    }
};

/* Records prefetchSubtree calls (path it receives, after any strip). */
struct PrefetchRecorder : MemorySourceAccessor
{
    std::vector<std::pair<CanonPath, unsigned>> calls;

    void prefetchSubtree(const CanonPath & subpath, unsigned depth) override
    {
        calls.emplace_back(subpath, depth);
    }
};

} // namespace

/* ---------- The headline: Switch commits; Layer falls through ---------- */

/* mounts = {/ → A (has /a/b/c), /a/b → B (empty)}.
   Switch.read(/a/b/c): nearest mount is /a/b → B, stripped to /c → B has
   no /c → NOT FOUND (authoritative, commits). It must NOT fall through
   to the root A even though A has /a/b/c. */
TEST(SwitchSourceAccessor, AuthoritativeCommitNoFallthrough)
{
    auto a = withFiles({{"a/b/c", "from-root"}});
    auto b = emptyDir();

    auto sw = makeSwitch({
        {CanonPath::root, a},
        {CanonPath("/a/b"), b},
    });

    EXPECT_FALSE(sw->maybeLstat(CanonPath("/a/b/c")).has_value());
    EXPECT_THROW(sw->readFile(CanonPath("/a/b/c")), FileNotFound);
}

/* Contrast: a Layer over the same data DOES fall through. Pins the
   behavioural difference that the §6.7 counterexample rests on. */
TEST(SwitchSourceAccessor, LayerWouldFallThroughUnlikeSwitch)
{
    auto a = withFiles({{"a/b/c", "from-root"}});
    auto bEmpty = emptyDir();

    /* Union = Alternative: first child (empty) misses, falls through to
       A, which has it. Switch must NOT do this. */
    auto layer = makeUnionSourceAccessor({bEmpty, a});
    EXPECT_EQ(layer->readFile(CanonPath("/a/b/c")), "from-root");
}

/* ---------- Longest-prefix-match selection ---------- */

TEST(SwitchSourceAccessor, LongestPrefixWins)
{
    auto shallow = withFiles({{"only-shallow.txt", "shallow"}, {"a/shadowed.txt", "root-sees"}});
    auto deep = withFiles({{"x.txt", "deep"}});

    auto sw = makeSwitch({
        {CanonPath::root, shallow},
        {CanonPath("/a"), deep},
    });

    /* /a/x.txt: nearest mount is /a → deep, stripped to /x.txt. */
    EXPECT_EQ(sw->readFile(CanonPath("/a/x.txt")), "deep");
    /* /only-shallow.txt: only root matches → shallow, full path. */
    EXPECT_EQ(sw->readFile(CanonPath("/only-shallow.txt")), "shallow");
    /* /a/shadowed.txt: the /a mount OWNS /a and has no shadowed.txt, so
       Switch commits to not-found — it does NOT serve root's
       /a/shadowed.txt. (Authoritative-commit at a non-root owner.) */
    EXPECT_FALSE(sw->maybeLstat(CanonPath("/a/shadowed.txt")).has_value());
}

/* ---------- Per-mount strip equivalence (the StripPrefix identity) ----------
   We can't import the libfetchers StripPrefix here (DAG), so assert
   against the explicit removePrefix forwarding. The proper
   StripPrefix-operator equivalence is in
   libfetchers-tests/algebra-cross-cutting.cc::SwitchBranchEqualsStripPrefix. */
TEST(SwitchSourceAccessor, PerMountReadEqualsStrippedInnerRead)
{
    auto inner = withFiles({{"file.txt", "v"}, {"d/deep.txt", "w"}});
    auto sw = makeSwitch({
        {CanonPath::root, makeEmptySourceAccessor()},
        {CanonPath("/sub"), inner},
    });

    EXPECT_EQ(sw->readFile(CanonPath("/sub/file.txt")), inner->readFile(CanonPath("/file.txt")));
    EXPECT_EQ(sw->readFile(CanonPath("/sub/d/deep.txt")), inner->readFile(CanonPath("/d/deep.txt")));
}

/* ---------- getFingerprint matches the inner fingerprint at stripped path ----------
   For (i) a content-keyed inner (git-submodule shape) and (ii) a null-fp
   inner (storeFS shape). */
TEST(SwitchSourceAccessor, FingerprintForwardsStripped)
{
    auto inner = withFiles({{"file.txt", "v"}});
    inner->fingerprint = "tree:SUB";
    auto sw = makeSwitch({
        {CanonPath::root, makeEmptySourceAccessor()},
        {CanonPath("/sub"), inner},
    });
    auto [_s, swFp] = sw->getFingerprint(CanonPath("/sub/file.txt"));
    auto [_i, innerFp] = inner->getFingerprint(CanonPath("/file.txt"));
    EXPECT_EQ(swFp, innerFp);
    EXPECT_EQ(swFp, "tree:SUB");

    auto plainInner = withFiles({{"f", "x"}});
    auto sw2 = makeSwitch({
        {CanonPath::root, makeEmptySourceAccessor()},
        {CanonPath("/m"), plainInner},
    });
    auto [_s2, sw2Fp] = sw2->getFingerprint(CanonPath("/m/f"));
    EXPECT_FALSE(sw2Fp.has_value());
}

/* ---------- Method-override coverage: readLink / lstat / readDirectory /
   getPhysicalPath / prefetchSubtree all strip before dispatch ---------- */

TEST(SwitchSourceAccessor, ReadLinkStripsAndDispatches)
{
    auto inner = make_ref<MemorySourceAccessor>();
    inner->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    inner->open(CanonPath("/link"), MemorySourceAccessor::File{MemorySourceAccessor::File::Symlink{"the-target"}});
    auto sw = makeSwitch({
        {CanonPath::root, emptyDir()},
        {CanonPath("/sub"), inner},
    });
    /* /sub/link → inner /link → "the-target". */
    EXPECT_EQ(sw->readLink(CanonPath("/sub/link")), "the-target");
    auto st = sw->maybeLstat(CanonPath("/sub/link"));
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->type, SourceAccessor::tSymlink);
}

TEST(SwitchSourceAccessor, LstatStripsAndDispatches)
{
    auto inner = withFiles({{"f.txt", "data"}});
    auto sw = makeSwitch({
        {CanonPath::root, emptyDir()},
        {CanonPath("/sub"), inner},
    });
    auto st = sw->lstat(CanonPath("/sub/f.txt"));
    EXPECT_EQ(st.type, SourceAccessor::tRegular);
}

TEST(SwitchSourceAccessor, ReadDirectoryStripsAndDispatches)
{
    auto inner = withFiles({{"d/one.txt", "1"}, {"d/two.txt", "2"}});
    auto sw = makeSwitch({
        {CanonPath::root, emptyDir()},
        {CanonPath("/sub"), inner},
    });
    auto entries = sw->readDirectory(CanonPath("/sub/d"));
    EXPECT_EQ(entries.size(), 2u);
    EXPECT_TRUE(entries.contains("one.txt"));
    EXPECT_TRUE(entries.contains("two.txt"));
}

/* Opacity is RECURSIVE (gap #7): the no-merge property holds not just AT
   the mount point but at every SUBDIRECTORY under it. Listing /sub/d
   yields only the mount's entries; the root's same-named /sub/d/* is not
   merged in — the OverlayFS "opaque directory" semantics, recursive. */
TEST(SwitchSourceAccessor, ReadDirectoryNoMergeBelowMountPoint)
{
    /* root has content at /sub/d/root-only.txt; the /sub mount has its
       own d/mount-only.txt. */
    auto root = withFiles({{"sub/d/root-only.txt", "R"}});
    auto inner = withFiles({{"d/mount-only.txt", "M"}});
    auto sw = makeSwitch({
        {CanonPath::root, root},
        {CanonPath("/sub"), inner},
    });

    /* Listing /sub/d resolves into the mount (stripped to /d) → only the
       mount's child; root's /sub/d/root-only.txt must NOT appear. */
    auto entries = sw->readDirectory(CanonPath("/sub/d"));
    EXPECT_TRUE(entries.contains("mount-only.txt"));
    EXPECT_FALSE(entries.contains("root-only.txt"));
}

TEST(SwitchSourceAccessor, GetCheckoutPathStripsBeforeForwarding)
{
    auto inner = make_ref<CheckoutPathLeaf>();
    inner->addFile(CanonPath("f.txt"), "v");
    auto sw = makeSwitch({
        {CanonPath::root, emptyDir()},
        {CanonPath("/sub"), inner},
    });
    /* The leaf sees the STRIPPED path /f.txt, so physical path is
       /phys/f.txt — proving Switch strips the /sub prefix first. */
    auto pp = sw->getPhysicalPath(CanonPath("/sub/f.txt"));
    ASSERT_TRUE(pp.has_value());
    EXPECT_EQ(pp->generic_string(), "/phys/f.txt");
}

TEST(SwitchSourceAccessor, PrefetchSubtreeStripsAndDispatches)
{
    auto inner = make_ref<PrefetchRecorder>();
    inner->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    auto sw = makeSwitch({
        {CanonPath::root, emptyDir()},
        {CanonPath("/sub"), inner},
    });
    sw->prefetchSubtree(CanonPath("/sub/inner/path"), 3);
    ASSERT_EQ(inner->calls.size(), 1u);
    /* Inner receives the stripped path /inner/path, not /sub/inner/path. */
    EXPECT_EQ(inner->calls[0].first, CanonPath("/inner/path"));
    EXPECT_EQ(inner->calls[0].second, 3u);
}

/* CR-3: an EXHAUSTIVE prefetch (subpath = an ancestor of sub-mounts)
   must fan out to every strictly-descendant mount — otherwise a Git
   submodule (mounted at /sub) is never bulk-primed and its blobs fall
   back to the slow per-blob path. */
TEST(SwitchSourceAccessor, PrefetchSubtreeFansOutToDescendantMounts)
{
    auto root = make_ref<PrefetchRecorder>();
    root->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    auto subA = make_ref<PrefetchRecorder>();
    subA->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    auto subB = make_ref<PrefetchRecorder>();
    subB->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});

    auto sw = makeSwitch({
        {CanonPath::root, root},
        {CanonPath("/modules/a"), subA},
        {CanonPath("/modules/b"), subB},
    });

    /* Exhaustive prefetch from the root. */
    sw->prefetchSubtree(CanonPath::root, 7);

    /* The root mount owns root → one call at its own root. */
    ASSERT_EQ(root->calls.size(), 1u);
    EXPECT_EQ(root->calls[0].first, CanonPath::root);

    /* BOTH sub-mounts must be primed from their own roots — the fan-out. */
    ASSERT_EQ(subA->calls.size(), 1u);
    EXPECT_EQ(subA->calls[0].first, CanonPath::root);
    EXPECT_EQ(subA->calls[0].second, 7u);
    ASSERT_EQ(subB->calls.size(), 1u);
    EXPECT_EQ(subB->calls[0].first, CanonPath::root);
}

/* A NARROW prefetch under one mount must NOT fan out to sibling/unrelated
   mounts (only the owner is touched). */
TEST(SwitchSourceAccessor, PrefetchSubtreeNarrowDoesNotFanOut)
{
    auto root = make_ref<PrefetchRecorder>();
    root->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    auto subA = make_ref<PrefetchRecorder>();
    subA->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});

    auto sw = makeSwitch({
        {CanonPath::root, root},
        {CanonPath("/modules/a"), subA},
    });

    /* Prefetch a path served entirely by the root mount, disjoint from
       /modules/a. Only the root mount is touched; subA is untouched. */
    sw->prefetchSubtree(CanonPath("/other"), 2);
    ASSERT_EQ(root->calls.size(), 1u);
    EXPECT_EQ(root->calls[0].first, CanonPath("/other"));
    EXPECT_EQ(subA->calls.size(), 0u);
}

/* ---------- Edge cases ---------- */

TEST(SwitchSourceAccessor, RootMountServesUnmatched)
{
    auto root = withFiles({{"anywhere/at/all.txt", "R"}});
    auto sw = makeSwitch({{CanonPath::root, root}});
    /* Single root mount: everything routes there with the full path
       (removePrefix(root) is the no-op). */
    EXPECT_EQ(sw->readFile(CanonPath("/anywhere/at/all.txt")), "R");
}

TEST(SwitchSourceAccessor, ReadAtMountPointItself)
{
    /* Querying exactly the mount point: removePrefix(==) → root, so the
       inner accessor is read at its own root. */
    auto inner = withFiles({{"f.txt", "v"}});
    auto sw = makeSwitch({
        {CanonPath::root, makeEmptySourceAccessor()},
        {CanonPath("/sub"), inner},
    });
    auto st = sw->maybeLstat(CanonPath("/sub"));
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->type, SourceAccessor::tDirectory);
    EXPECT_EQ(sw->readFile(CanonPath("/sub/f.txt")), "v");
}

TEST(SwitchSourceAccessor, NestedMountsLongestPrefix)
{
    auto outer = withFiles({{"a/marker.txt", "outer"}});
    auto innerA = withFiles({{"marker.txt", "innerA"}});
    auto innerAB = withFiles({{"marker.txt", "innerAB"}});

    auto sw = makeSwitch({
        {CanonPath::root, outer},
        {CanonPath("/a"), innerA},
        {CanonPath("/a/b"), innerAB},
    });

    EXPECT_EQ(sw->readFile(CanonPath("/a/b/marker.txt")), "innerAB");
    EXPECT_EQ(sw->readFile(CanonPath("/a/marker.txt")), "innerA");
}

/* ---------- Property tests (the category theory dictates these) ---------- */

#ifndef COVERAGE

namespace {

/* Build a Switch with a root mount (required) plus an optional mount at
   `mountPoint → inner`. Returns the Switch and the resolved owner of any
   query, so the test can compute the expected answer independently. */
std::map<CanonPath, ref<SourceAccessor>> rootPlus(ref<SourceAccessor> root, CanonPath mp, ref<SourceAccessor> inner)
{
    std::map<CanonPath, ref<SourceAccessor>> m{{CanonPath::root, root}};
    if (!mp.isRoot())
        m.insert_or_assign(mp, inner);
    return m;
}

} // namespace

/* Sw3 — per-branch resolution law (the identity that justifies Switch's
   inline strip): for a mount at `mp` and any path under it, the Switch's
   read/maybeLstat/getFingerprint equal the inner accessor's at the
   stripped path. This is `∀` over (mountPoint, inner tree, subpath). */
RC_GTEST_PROP(SwitchSourceAccessor, PerBranchEqualsStrippedInner, (const CanonPath & mp, const CanonPath & y))
{
    RC_PRE(!mp.isRoot());

    /* inner tree, rooted at /, with content at `y` (and a sibling so the
       tree is non-trivial). */
    auto leaf = y.isRoot() ? CanonPath("only-file") : y;
    auto inner = make_ref<MemorySourceAccessor>();
    inner->addFile(leaf, "content");
    inner->fingerprint = "tree:R";

    auto sw = makeSwitch(rootPlus(makeEmptySourceAccessor(), mp, inner));

    auto query = mp / leaf;                 // within mp → owned by the inner mount
    auto stripped = query.removePrefix(mp); // == leaf

    /* read agrees */
    RC_ASSERT(sw->readFile(query) == inner->readFile(stripped));
    /* maybeLstat agreement (presence + type) */
    auto swSt = sw->maybeLstat(query);
    auto inSt = inner->maybeLstat(stripped);
    RC_ASSERT(swSt.has_value() == inSt.has_value());
    /* getFingerprint agrees (the load-bearing cache-row-sharing law) */
    auto [_s, swFp] = sw->getFingerprint(query);
    auto [_i, inFp] = inner->getFingerprint(stripped);
    RC_ASSERT(swFp == inFp);
}

/* Sw1 — resolution law over a random two-mount Switch: for ANY query
   path, the Switch's maybeLstat equals "route to the nearest-prefix
   owner, read at the stripped path" computed independently. Exercises
   both the owned-by-inner and owned-by-root cases as the random path
   falls inside or outside `mp`. */
RC_GTEST_PROP(
    SwitchSourceAccessor,
    ResolutionMatchesNearestOwner,
    (const CanonPath & mp,
     const MemorySourceAccessor & rootTree,
     const MemorySourceAccessor & innerTree,
     const CanonPath & x))
{
    RC_PRE(!mp.isRoot());

    auto root = make_ref<MemorySourceAccessor>(rootTree);
    auto inner = make_ref<MemorySourceAccessor>(innerTree);
    auto sw = makeSwitch(rootPlus(root, mp, inner));

    /* Independently compute the expected owner + stripped path. */
    ref<SourceAccessor> owner = x.isWithin(mp) ? inner.cast<SourceAccessor>() : root.cast<SourceAccessor>();
    CanonPath stripped = x.isWithin(mp) ? x.removePrefix(mp) : x;

    /* maybeLstat can throw SymlinkNotAllowed when an ancestor of the
       queried path is a symlink (a leaf-accessor property, not Switch's).
       Switch forwards to the owner at the stripped path, so it must throw
       iff the owner does — compare the full outcome (throw / value /
       type), not just the success case. */
    std::optional<SourceAccessor::Stat> swSt, ownerSt;
    bool swThrew = false, ownerThrew = false;
    try {
        swSt = sw->maybeLstat(x);
    } catch (Error &) {
        swThrew = true;
    }
    try {
        ownerSt = owner->maybeLstat(stripped);
    } catch (Error &) {
        ownerThrew = true;
    }
    RC_ASSERT(swThrew == ownerThrew);
    if (!swThrew) {
        RC_ASSERT(swSt.has_value() == ownerSt.has_value());
        if (swSt.has_value())
            RC_ASSERT(swSt->type == ownerSt->type);
    }
}

/* Sw2 — authoritative-commit ≠ fall-through, as a property. Whenever the
   inner mount OWNS a path but lacks it, while the root WOULD serve it,
   the Switch commits to not-found (≠ what a fall-through Layer gives).
   We construct exactly that shadowing shape and assert the Switch misses. */
RC_GTEST_PROP(SwitchSourceAccessor, AuthoritativeCommitProperty, (const CanonPath & mp, const CanonPath & tail))
{
    RC_PRE(!mp.isRoot());
    RC_PRE(!tail.isRoot());

    /* root HAS mp/tail; inner (mounted at mp) is empty. `addFile` can
       throw if a path component collides with an existing regular file
       (the random alphabet makes this possible) — discard those samples
       rather than fail. */
    auto root = make_ref<MemorySourceAccessor>();
    try {
        root->addFile(mp / tail, "shadowed-by-mount");
    } catch (Error &) {
        RC_DISCARD("unplantable path (component conflict)");
    }
    auto inner = emptyDir();

    auto sw = makeSwitch(rootPlus(root.cast<SourceAccessor>(), mp, inner));

    /* The mount at mp owns mp/tail and lacks it → Switch commits to a
       miss. A Layer would have fallen through to root and found it. */
    RC_ASSERT(!sw->maybeLstat(mp / tail).has_value());
    /* And the contrasting fall-through Layer DOES find it (non-vacuous). */
    auto layer = makeUnionSourceAccessor({inner.cast<SourceAccessor>(), root.cast<SourceAccessor>()});
    RC_ASSERT(layer->maybeLstat(mp / tail).has_value());
}

/* Sw4 — mount-order independence. Longest-prefix-match resolution is a
   function of the mount SET, not the insertion order: the two mounts are
   keyed in a map by path, so building the Switch with the entries added
   in either order yields the same accessor. This is the defining
   structural property of an LPM Switch (an ordered Layer, by contrast,
   is order-DEPENDENT — first-wins). We assert agreement at the disputed
   path that each mount would serve differently. */
RC_GTEST_PROP(SwitchSourceAccessor, MountOrderIndependent, (const CanonPath & y))
{
    auto leaf = y.isRoot() ? CanonPath("only-file") : y;

    /* Two non-nested mounts /p and /q, each serving `leaf` with distinct
       bytes; a root that also serves it. */
    auto mkLeaf = [&](const char * tag) {
        auto m = make_ref<MemorySourceAccessor>();
        m->addFile(leaf, tag);
        return m;
    };
    auto root = mkLeaf("ROOT");
    auto pAcc = mkLeaf("P");
    auto qAcc = mkLeaf("Q");

    /* Build with the two non-root entries in both orders. std::map keys
       by path, so insertion order cannot matter — assert it. */
    std::map<CanonPath, ref<SourceAccessor>> m1{
        {CanonPath::root, root}, {CanonPath("/p"), pAcc}, {CanonPath("/q"), qAcc}};
    std::map<CanonPath, ref<SourceAccessor>> m2{
        {CanonPath("/q"), qAcc}, {CanonPath("/p"), pAcc}, {CanonPath::root, root}};
    auto sw1 = makeSwitch(m1);
    auto sw2 = makeSwitch(m2);

    for (auto & q : {CanonPath("/p") / leaf, CanonPath("/q") / leaf, CanonPath("/elsewhere") / leaf}) {
        auto a = sw1->maybeLstat(q);
        auto b = sw2->maybeLstat(q);
        RC_ASSERT(a.has_value() == b.has_value());
        if (a.has_value())
            RC_ASSERT(sw1->readFile(q) == sw2->readFile(q));
    }
}

/* Sw5 — no listing merge (the readDirectory dual of authoritative-commit,
   and the sharp contrast with Layer). The owning mount's directory
   listing is returned ALONE; the root's entries at the same logical path
   are NOT merged in. (A Layer at the same path WOULD union the two
   listings.) */
RC_GTEST_PROP(SwitchSourceAccessor, ReadDirectoryDoesNotMerge, (const CanonPath & mp))
{
    RC_PRE(!mp.isRoot());

    /* root has an entry UNDER mp (so a merge would surface it); the mount
       at mp has a different entry at its own root. */
    auto root = make_ref<MemorySourceAccessor>();
    try {
        root->addFile(mp / CanonPath("root-only.txt"), "R");
    } catch (Error &) {
        RC_DISCARD("unplantable mp");
    }
    auto inner = make_ref<MemorySourceAccessor>();
    inner->addFile(CanonPath("mount-only.txt"), "M");

    auto sw = makeSwitch(rootPlus(root.cast<SourceAccessor>(), mp, inner.cast<SourceAccessor>()));

    /* Listing mp resolves to the mount, stripped to its root → only the
       mount's entries. root-only.txt (which a Layer merge would include)
       must be ABSENT. */
    auto entries = sw->readDirectory(mp);
    RC_ASSERT(entries.contains("mount-only.txt"));
    RC_ASSERT(!entries.contains("root-only.txt"));
}

/* Sw6 — PREFIX-MONOTONICITY (the LPM "longer match always wins" law as a
   property over nested mount pairs): if m1 ⊏ m2 (m1 a strict prefix of
   m2), then every path within m2 is owned by m2, never m1 — the deeper
   mount shadows m1 on its ENTIRE subtree. (LPM / radix-tree:
   https://en.wikipedia.org/wiki/Longest_prefix_match.) We give m1 and m2
   distinct content at the same logical leaf and assert the read resolves
   to m2, stripped at m2. */
RC_GTEST_PROP(
    SwitchSourceAccessor,
    PrefixMonotonicityDeeperShadowsSubtree,
    (const CanonPath & m1, const CanonPath & rest, const CanonPath & y))
{
    RC_PRE(!m1.isRoot());
    RC_PRE(!rest.isRoot()); // m2 = m1/rest is a STRICT descendant of m1
    auto m2 = m1 / rest;

    auto leaf = y.isRoot() ? CanonPath("leaf") : y;
    auto accM1 = make_ref<MemorySourceAccessor>();
    accM1->addFile(leaf, "M1");
    auto accM2 = make_ref<MemorySourceAccessor>();
    accM2->addFile(leaf, "M2");

    std::map<CanonPath, ref<SourceAccessor>> mounts{
        {CanonPath::root, makeEmptySourceAccessor()},
        {m1, accM1},
        {m2, accM2},
    };
    auto sw = makeSwitch(mounts);

    /* Query within m2's cone: must resolve to accM2 at the m2-stripped
       path, NOT accM1 (even though m2/leaf is also within m1). */
    auto query = m2 / leaf;
    RC_ASSERT(sw->readFile(query) == "M2");
    RC_ASSERT(sw->readFile(query) == accM2->readFile(query.removePrefix(m2)));
}

/* Sw7 — DISJOINT-OWNERSHIP / sibling-cone independence: for non-nested
   mounts m1, m2 (neither a prefix of the other), a read in m1's cone is
   unaffected by m2's content. We mount two siblings under distinct
   roots and assert each cone reads its own mount, and that swapping the
   OTHER mount's content changes nothing in this cone. */
RC_GTEST_PROP(SwitchSourceAccessor, SiblingConesIndependent, (const CanonPath & y))
{
    auto leaf = y.isRoot() ? CanonPath("leaf") : y;
    auto p = make_ref<MemorySourceAccessor>();
    p->addFile(leaf, "P");
    auto q1 = make_ref<MemorySourceAccessor>();
    q1->addFile(leaf, "Q1");
    auto q2 = make_ref<MemorySourceAccessor>();
    q2->addFile(leaf, "Q2-different");

    /* /p and /q are siblings (neither within the other). */
    auto sw1 = makeSwitch({{CanonPath::root, makeEmptySourceAccessor()}, {CanonPath("/p"), p}, {CanonPath("/q"), q1}});
    auto sw2 = makeSwitch({{CanonPath::root, makeEmptySourceAccessor()}, {CanonPath("/p"), p}, {CanonPath("/q"), q2}});

    /* The /p-cone read is identical regardless of what's mounted at /q. */
    auto query = CanonPath("/p") / leaf;
    RC_ASSERT(sw1->readFile(query) == "P");
    RC_ASSERT(sw1->readFile(query) == sw2->readFile(query));
}

/* ---------- ERROR-CHANNEL laws (PROPOSAL §6.8.1, the Item-5 regression) ---------- */

/* L-ErrPreserve (Switch arm) — Switch DIRECT-FORWARDS reads, so the inner
   accessor's bespoke exception type AND its message survive the
   combinator unchanged. We mount a `ThrowingLeaf` at a generated non-root
   `mp`; reading `mp / p` resolves into the leaf at the stripped `p`, which
   throws `CustomBespokeError` carrying the captured nonce. We assert BOTH
   that the dynamic type is exactly `CustomBespokeError` (a typed catch
   succeeds) AND that the leaf's own message was derived from — not
   replaced by — the wrapper (the nonce is still present).

   NON-VACUITY: this is the property that the §6.8.1 regression shape — a
   Switch that did maybeLstat-then-read and rethrew the miss as its own
   generic `FileNotFound`, the way Union does — fails, while passing all
   seven existing Sw1–Sw7 props (those only compare a BOOLEAN `threw` /
   `has_value`, collapsing Either-equivalence to Maybe-equivalence). Pin
   the dynamic type, and the masking is caught. */
RC_GTEST_PROP(SwitchSourceAccessor, ErrorTypePreservedAcrossForward, (const CanonPath & mp, const CanonPath & p))
{
    RC_PRE(!mp.isRoot());
    RC_PRE(!p.isRoot()); // p must be a strict subpath so the leaf, not the root, owns it

    auto leaf = make_ref<ThrowingLeaf>(p, "swErrPreserveNonce-7f3a");
    auto sw = makeSwitch(rootPlus(makeEmptySourceAccessor(), mp, leaf.cast<SourceAccessor>()));

    auto query = mp / p; // owned by the leaf mount; strips back to `p`

    bool threw = false, typedCatch = false, messageDerived = false;
    try {
        sw->readFile(query);
    } catch (CustomBespokeError & e) {
        threw = typedCatch = true;
        messageDerived = e.msg().find("swErrPreserveNonce-7f3a") != std::string::npos;
    } catch (Error &) {
        /* Any other Error (e.g. a substituted FileNotFound) leaves the
           typed flags false → the assertions below fail, catching the
           mask. */
        threw = true;
    }
    RC_ASSERT(threw);          // the leaf must raise at its designated path
    RC_ASSERT(typedCatch);     // exact dynamic type preserved
    RC_ASSERT(messageDerived); // inner message derived-from, not replaced
}

/* L-Dual — the missing Union-mirror of Sw2 (AuthoritativeCommitProperty),
   stated on the ERROR channel and over a SHADOWING shape. Build a root
   accessor that DOES serve `mp / p`, plus a more-specific mount at `mp`
   that does NOT (a `ThrowingLeaf` that misses on probe and throws on
   read). Then:

     - Switch commits to the specific mount: it resolves `mp / p` into the
       leaf and forwards, so `maybeLstat` misses and `readFile`/`lstat`
       throw the leaf's bespoke type — it NEVER falls through to the root,
       even though the root has the path.
     - Layer (= Union) falls through: the leaf's `maybeLstat` miss makes
       Union try the next accessor (the root), which serves it — so
       `maybeLstat` succeeds and `readFile` returns the root's bytes.

   This pins the Layer-vs-Switch asymmetry of PROPOSAL §6.7 on the error
   channel (Sw2 pinned it only on presence). `lstat` is included in the
   surface to show the throwing-method dual as well as the silent probe. */
RC_GTEST_PROP(SwitchSourceAccessor, DualSwitchCommitsLayerFallsThrough, (const CanonPath & mp, const CanonPath & p))
{
    RC_PRE(!mp.isRoot());
    RC_PRE(!p.isRoot());

    /* Root serves mp/p; the more-specific mount (leaf) does not. `addFile`
       can throw on a random component collision — discard those samples. */
    auto root = make_ref<MemorySourceAccessor>();
    try {
        root->addFile(mp / p, "served-by-root");
    } catch (Error &) {
        RC_DISCARD("unplantable mp/p (component conflict)");
    }
    auto leaf = make_ref<ThrowingLeaf>(p, "dualNonce-91c4");

    auto query = mp / p;

    /* Switch: commits to the leaf (the nearest mount owning mp/p). */
    auto sw = makeSwitch(rootPlus(root.cast<SourceAccessor>(), mp, leaf.cast<SourceAccessor>()));
    RC_ASSERT(!sw->maybeLstat(query).has_value()); // committed miss, NOT root's hit
    RC_ASSERT_THROWS_AS(sw->readFile(query), CustomBespokeError);
    RC_ASSERT_THROWS_AS(sw->lstat(query), CustomBespokeError);

    /* Layer: leaf misses on probe → falls through to root, which serves. */
    auto layer = makeUnionSourceAccessor({leaf.cast<SourceAccessor>(), root.cast<SourceAccessor>()});
    RC_ASSERT(layer->maybeLstat(query).has_value()); // fell through to root
    RC_ASSERT(layer->readFile(query) == "served-by-root");
}

#endif

/* ---------- Mutable face: MountedSourceAccessor ---------- */

TEST(MountedSourceAccessor, MountAtRuntimeResolves)
{
    auto mounted = makeMountedSourceAccessor({{CanonPath::root, makeEmptySourceAccessor()}});

    EXPECT_FALSE(mounted->maybeLstat(CanonPath("/lazy/f.txt")).has_value());

    auto inner = withFiles({{"f.txt", "late"}});
    mounted->mount(CanonPath("/lazy"), inner);
    EXPECT_EQ(mounted->readFile(CanonPath("/lazy/f.txt")), "late");
}

TEST(MountedSourceAccessor, GetMountIsExactKey)
{
    auto inner = withFiles({{"f", "v"}});
    auto mounted = makeMountedSourceAccessor({{CanonPath::root, makeEmptySourceAccessor()}});
    mounted->mount(CanonPath("/nix/store/x"), inner);

    EXPECT_NE(mounted->getMount(CanonPath("/nix/store/x")), nullptr);
    /* A child of the mount point is NOT a mount key. */
    EXPECT_EQ(mounted->getMount(CanonPath("/nix/store/x/sub")), nullptr);
    /* An ancestor is not the key either. */
    EXPECT_EQ(mounted->getMount(CanonPath("/nix/store")), nullptr);
}

} // namespace nix
