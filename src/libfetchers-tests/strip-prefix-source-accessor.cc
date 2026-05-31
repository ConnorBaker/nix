/* StripPrefix (Prism / partial inverse of Translate) — Layer-1 read
   algebra. Strip vs Translate's prepend. SP1–SP6. The Translate
   adjunction round-trips live in algebra-cross-cutting.cc.

   ORIENTATION (the dual of Translate, easy to get backwards):
   `StripPrefix(p)(b).read(p / y) = b.read(y)`. The inner accessor `b`
   is rooted at its OWN `/`; you query the wrapper at the PREFIXED path
   `p / y`, which strips back to `y`. (Translate is the mirror: content
   at the prefixed path, query at the stripped path.) So fixtures put
   content at the inner-rooted path and query through the prefix. */

#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "nix/fetchers/strip-prefix-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-path.hh"
#include "nix/util/tests/source-accessor.hh"

#include "operator-test-helpers.hh"

namespace nix {

namespace {

/* The INNER accessor, rooted at `/` (content at the stripped paths). */
ref<MemorySourceAccessor> innerTree()
{
    auto m = make_ref<MemorySourceAccessor>();
    m->addFile(CanonPath("file.txt"), "root-bytes");
    m->addFile(CanonPath("sub/deep.txt"), "deep-bytes");
    return m;
}

/* File-local factory. Prefixed `sp` so that under the libfetchers-tests
   unity build it does not collide with helpers in sibling op-test
   files. Mirrors `tsaMakeTranslate` in translate-source-accessor.cc. */
ref<StripPrefixSourceAccessor> spMakeStripPrefix(ref<SourceAccessor> base, CanonPath prefix)
{
    return make_ref<StripPrefixSourceAccessor>(base, std::move(prefix), [](const CanonPath & p) -> RestrictedPathError {
        return RestrictedPathError("path '%s' not allowed", p.abs());
    });
}

} // namespace

/* In-prefix read forwards at the stripped path: SP(/mnt).read(/mnt/file.txt)
   reads inner at /file.txt. */
TEST(StripPrefixSourceAccessor, BasicReadAtPrefix)
{
    auto base = innerTree();
    auto s = spMakeStripPrefix(base, CanonPath("/mnt"));
    EXPECT_EQ(s->readFile(CanonPath("/mnt/file.txt")), "root-bytes");
    EXPECT_EQ(s->readFile(CanonPath("/mnt/sub/deep.txt")), "deep-bytes");
}

/* Listing forwards at the stripped path. Listing the mount point itself
   strips to the inner root. */
TEST(StripPrefixSourceAccessor, ReadDirectoryAtPrefix)
{
    auto base = innerTree();
    auto s = spMakeStripPrefix(base, CanonPath("/mnt"));
    /* /mnt → inner root → {file.txt, sub}. */
    auto top = s->readDirectory(CanonPath("/mnt"));
    EXPECT_TRUE(top.contains("file.txt"));
    EXPECT_TRUE(top.contains("sub"));
    /* /mnt/sub → inner /sub → {deep.txt}. */
    auto sub = s->readDirectory(CanonPath("/mnt/sub"));
    EXPECT_TRUE(sub.contains("deep.txt"));
    EXPECT_EQ(sub.size(), 1u);
}

/* SP3 — Prism deny. A path NOT within the prefix is denied even when the
   inner accessor WOULD serve it — the gate fires before the strip. This
   is what distinguishes StripPrefix (gates at the wrapper) from Translate
   (never denies at the wrapper). */
TEST(StripPrefixSourceAccessor, OutOfPrefixDenied)
{
    auto base = innerTree();
    auto s = spMakeStripPrefix(base, CanonPath("/mnt"));
    /* Inner HAS /file.txt, but /file.txt is outside the /mnt prefix —
       so it is denied, not served. */
    EXPECT_FALSE(s->maybeLstat(CanonPath("/file.txt")).has_value());
    EXPECT_THROW(s->readFile(CanonPath("/file.txt")), RestrictedPathError);
    /* An ancestor of the prefix is denied too (can't read above the
       mount point through the strip). */
    EXPECT_FALSE(s->maybeLstat(CanonPath::root).has_value());
}

/* SP5 — at a fixed in-prefix path, the wrapper's fingerprint equals the
   base's fingerprint at the stripped path (empty own-suffix forwards). */
TEST(StripPrefixSourceAccessor, EmptyComputeOwnSuffixForwards)
{
    auto base = innerTree();
    base->fingerprint = "git:R";
    auto s = spMakeStripPrefix(base, CanonPath("/mnt"));
    auto [_, fp] = s->getFingerprint(CanonPath("/mnt/file.txt"));
    EXPECT_EQ(fp, "git:R");
}

/* SP5 off-domain — getFingerprint on a path outside the prefix must NOT
   call removePrefix (which would assert); it returns the wrapper's own
   (unset) fingerprint. */
TEST(StripPrefixSourceAccessor, FingerprintOffPrefixDoesNotAssert)
{
    auto base = innerTree();
    base->fingerprint = "git:R";
    auto s = spMakeStripPrefix(base, CanonPath("/mnt"));
    auto [_, fp] = s->getFingerprint(CanonPath("/file.txt")); // outside /mnt
    /* Wrapper's own fingerprint field is unset → nullopt; crucially, no
       assert/crash on the out-of-prefix removePrefix. */
    EXPECT_FALSE(fp.has_value());
}

/* ---------- Per-method override coverage (each strips before forwarding;
   each throwing method also denies off-prefix) ---------- */

TEST(StripPrefixSourceAccessor, LstatStripsAndDenies)
{
    auto base = innerTree();
    auto s = spMakeStripPrefix(base, CanonPath("/mnt"));
    /* In-prefix: /mnt/file.txt → inner /file.txt → regular. */
    EXPECT_EQ(s->lstat(CanonPath("/mnt/file.txt")).type, SourceAccessor::tRegular);
    /* Off-prefix: inner has /file.txt, but it's outside /mnt → denied. */
    EXPECT_THROW(s->lstat(CanonPath("/file.txt")), RestrictedPathError);
}

TEST(StripPrefixSourceAccessor, ReadLinkStripsAndDenies)
{
    auto base = make_ref<MemorySourceAccessor>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    base->open(CanonPath("/link"), MemorySourceAccessor::File{MemorySourceAccessor::File::Symlink{"the-target"}});
    auto s = spMakeStripPrefix(base, CanonPath("/mnt"));
    /* In-prefix: /mnt/link → inner /link → "the-target". */
    EXPECT_EQ(s->readLink(CanonPath("/mnt/link")), "the-target");
    /* Off-prefix denied. */
    EXPECT_THROW(s->readLink(CanonPath("/link")), RestrictedPathError);
}

TEST(StripPrefixSourceAccessor, ReadDirectoryDeniesOffPrefix)
{
    auto base = innerTree();
    auto s = spMakeStripPrefix(base, CanonPath("/mnt"));
    /* In-prefix listing works (covered in ReadDirectoryAtPrefix); here
       the negative: listing a path outside the prefix is denied. */
    EXPECT_THROW(s->readDirectory(CanonPath("/sub")), RestrictedPathError);
}

TEST(StripPrefixSourceAccessor, GetCheckoutPathStripsBeforeForwarding)
{
    /* A leaf reporting a physical path at the path it receives, so we can
       observe that StripPrefix forwards the STRIPPED path. */
    struct PhysLeaf : MemorySourceAccessor
    {
        std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
        {
            return std::filesystem::path("/phys") / path.rel();
        }
    };

    auto base = make_ref<PhysLeaf>();
    base->addFile(CanonPath("file.txt"), "v");
    auto s = spMakeStripPrefix(base, CanonPath("/mnt"));
    /* In-prefix: leaf sees /file.txt → /phys/file.txt. */
    auto pp = s->getPhysicalPath(CanonPath("/mnt/file.txt"));
    ASSERT_TRUE(pp.has_value());
    EXPECT_EQ(pp->generic_string(), "/phys/file.txt");
    /* Off-prefix denied (getPhysicalPath calls checkAccess). */
    EXPECT_THROW(s->getPhysicalPath(CanonPath("/file.txt")), RestrictedPathError);
}

TEST(StripPrefixSourceAccessor, PrefetchSubtreeStripsAndGatesSilently)
{
    /* Records the (stripped) subpath the leaf receives. */
    struct PrefetchLeaf : MemorySourceAccessor
    {
        std::vector<CanonPath> calls;

        void prefetchSubtree(const CanonPath & subpath, unsigned) override
        {
            calls.push_back(subpath);
        }
    };

    auto base = make_ref<PrefetchLeaf>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    auto s = spMakeStripPrefix(base, CanonPath("/mnt"));

    /* In-prefix: leaf receives the stripped subpath. */
    s->prefetchSubtree(CanonPath("/mnt/inner/path"), 2);
    ASSERT_EQ(base->calls.size(), 1u);
    EXPECT_EQ(base->calls[0], CanonPath("/inner/path"));

    /* Off-prefix: prefetch is a best-effort hint, so it is a SILENT
       no-op (not a throw) — the leaf is never called. */
    s->prefetchSubtree(CanonPath("/elsewhere"), 2);
    EXPECT_EQ(base->calls.size(), 1u);
}

#ifndef COVERAGE

/* SP3 PROP — any path provably not within the prefix returns nullopt. */
RC_GTEST_PROP(StripPrefixSourceAccessor, NegativeMaybeLstatOutOfPrefix, (const CanonPath & p, const CanonPath & y))
{
    auto base = make_ref<MemorySourceAccessor>();
    if (!y.isRoot()) {
        try {
            base->addFile(y, "content");
        } catch (Error &) {
        }
    }
    auto s = spMakeStripPrefix(base, p);
    /* A path under a sibling root that cannot be within a non-root p. */
    auto outside = CanonPath(std::string("/definitely-outside-prefix-") + std::string(y.baseName().value_or("q")));
    if (!outside.isWithin(p))
        RC_ASSERT(!s->maybeLstat(outside).has_value());
}

/* SP3 positive direction — for an IN-prefix path, the Prism is
   presence-faithful: maybeLstat at p/y mirrors base's maybeLstat at y
   (same has_value, same type). The dual of the negative PROP above; the
   gate admits exactly the in-prefix cone and rewrites nothing else. */
RC_GTEST_PROP(StripPrefixSourceAccessor, PositiveMaybeLstatMirrorsBase, (const CanonPath & p, const CanonPath & y))
{
    auto leaf = y.isRoot() ? CanonPath("only-file") : y;
    auto base = make_ref<MemorySourceAccessor>();
    base->addFile(leaf, "content");
    auto s = spMakeStripPrefix(base, p);

    auto query = p / leaf; // within p
    /* maybeLstat can throw SymlinkNotAllowed on a symlinked ancestor;
       compare outcomes including the throw. */
    std::optional<SourceAccessor::Stat> sSt, bSt;
    bool sThrew = false, bThrew = false;
    try {
        sSt = s->maybeLstat(query);
    } catch (Error &) {
        sThrew = true;
    }
    try {
        bSt = base->maybeLstat(query.removePrefix(p));
    } catch (Error &) {
        bThrew = true;
    }
    RC_ASSERT(sThrew == bThrew);
    if (!sThrew) {
        RC_ASSERT(sSt.has_value() == bSt.has_value());
        if (sSt.has_value())
            RC_ASSERT(sSt->type == bSt->type);
    }
}

/* StripPrefix is NOT idempotent — unlike the path-set Prisms (Restrict,
   Mask), whose S/W sets satisfy S ∩ S = S / W ∪ W = W. Here
   StripPrefix(p) ∘ StripPrefix(p) ≡ StripPrefix(p / p) ≠ StripPrefix(p)
   for non-root p (it strips twice as deep). This pins the algebraic
   distinction: StripPrefix is a free monoid morphism (CanonPath under
   `/`), with no idempotence. */
RC_GTEST_PROP(StripPrefixSourceAccessor, NotIdempotent, (const CanonPath & p, const CanonPath & y))
{
    RC_PRE(!p.isRoot());
    auto leaf = y.isRoot() ? CanonPath("only-file") : y;

    /* Both accessors hold content at the bare `leaf`. Query both at the
       DOUBLY-prefixed path (p/p)/leaf:
         - twice = SP(p)∘SP(p): strips p twice → base.maybeLstat(leaf) → HIT.
         - once  = SP(p):       strips p once  → base.maybeLstat(p/leaf) → MISS
                                                  (base has `leaf`, not `p/leaf`).
       Disagreement at the same query path ⇒ SP(p)∘SP(p) ≢ SP(p): the
       composite strips to depth p/p ≠ p. (Contrast Restrict/Mask, whose
       set Prisms ARE idempotent.) */
    auto base = make_ref<MemorySourceAccessor>();
    base->addFile(leaf, "content");
    auto onceBase = make_ref<MemorySourceAccessor>();
    onceBase->addFile(leaf, "content");

    auto once = spMakeStripPrefix(onceBase, p);
    auto twice = spMakeStripPrefix(spMakeStripPrefix(base, p).cast<SourceAccessor>(), p);

    auto query = (p / p) / leaf;
    RC_ASSERT(twice->maybeLstat(query).has_value());
    RC_ASSERT(!once->maybeLstat(query).has_value());
}

/* SP2 — StripPrefix(p) ∘ StripPrefix(q) ≡ StripPrefix(p / q) where p is
   the OUTER strip. Observable: nesting two strips reads the same as one
   combined strip. Content lives at the fully-stripped path `y`; query at
   the doubly-prefixed path `p / q / y`. (Raw make_ref, not the factory —
   this tests the observable law, not factory fusion.) */
RC_GTEST_PROP(
    StripPrefixSourceAccessor, MonoidComposition, (const CanonPath & p, const CanonPath & q, const CanonPath & y))
{
    auto leaf = y.isRoot() ? CanonPath("only-file") : y;
    auto base = make_ref<MemorySourceAccessor>();
    base->addFile(leaf, "content");
    auto baseRead = make_ref<MemorySourceAccessor>();
    baseRead->addFile(leaf, "content");

    /* Outer SP(p) wraps inner SP(q): the second make call is the outer. */
    auto nested = spMakeStripPrefix(spMakeStripPrefix(base, q).cast<SourceAccessor>(), p);
    auto fused = spMakeStripPrefix(baseRead, p / q);

    auto query = p / q / leaf;
    bool nestedOk = false, fusedOk = false;
    std::string nestedC, fusedC;
    try {
        nestedC = nested->readFile(query);
        nestedOk = true;
    } catch (...) {
    }
    try {
        fusedC = fused->readFile(query);
        fusedOk = true;
    } catch (...) {
    }
    RC_ASSERT(nestedOk == fusedOk);
    RC_ASSERT(nestedOk); // both must actually read content (non-vacuous)
    RC_ASSERT(nestedC == fusedC);
}

/* SP1 — StripPrefix(/) ∘ S ≡ S (left identity, observable). */
RC_GTEST_PROP(StripPrefixSourceAccessor, LeftIdentity, (const CanonPath & p, const CanonPath & y))
{
    auto leaf = y.isRoot() ? CanonPath("only-file") : y;
    auto base = make_ref<MemorySourceAccessor>();
    base->addFile(leaf, "C");
    auto wrapped = spMakeStripPrefix(spMakeStripPrefix(base, p).cast<SourceAccessor>(), CanonPath::root);
    auto plain = spMakeStripPrefix(base, p);

    auto query = p / leaf;
    RC_ASSERT(wrapped->readFile(query) == plain->readFile(query));
}

/* SP1 — S ∘ StripPrefix(/) ≡ S (right identity, observable). */
RC_GTEST_PROP(StripPrefixSourceAccessor, RightIdentity, (const CanonPath & p, const CanonPath & y))
{
    auto leaf = y.isRoot() ? CanonPath("only-file") : y;
    auto base = make_ref<MemorySourceAccessor>();
    base->addFile(leaf, "C");
    auto wrapped = spMakeStripPrefix(spMakeStripPrefix(base, CanonPath::root).cast<SourceAccessor>(), p);
    auto plain = spMakeStripPrefix(base, p);

    auto query = p / leaf;
    RC_ASSERT(wrapped->readFile(query) == plain->readFile(query));
}

/* SP4 — faithfulness: StripPrefix(p)(b).read(p / y) = b.read(y) (no
   rewriting beyond the strip). */
RC_GTEST_PROP(StripPrefixSourceAccessor, Faithful, (const CanonPath & p, const CanonPath & y))
{
    auto leaf = y.isRoot() ? CanonPath("only-file") : y;
    auto base = make_ref<MemorySourceAccessor>();
    base->addFile(leaf, "C");
    auto s = spMakeStripPrefix(base, p);

    auto query = p / leaf;
    /* Wrapper at p/leaf == base at (p/leaf).removePrefix(p) == base at leaf. */
    RC_ASSERT(s->readFile(query) == base->readFile(query.removePrefix(p)));
    RC_ASSERT(s->readFile(query) == base->readFile(leaf));
}

/* SP5 — fingerprint threading: the wrapper's fingerprint at an in-prefix
   path equals the base's fingerprint at the stripped path. A regression
   that dropped the strip in getFingerprint would fail at any non-root
   prefix. (MemorySourceAccessor returns its `fingerprint` field at any
   path, so this isolates the path-threading, not existence.) */
RC_GTEST_PROP(StripPrefixSourceAccessor, FingerprintForwardsThroughPrefix, (const CanonPath & p, const CanonPath & y))
{
    auto base = make_ref<MemorySourceAccessor>();
    base->fingerprint = "tree:R";
    auto s = spMakeStripPrefix(base, p);

    auto query = p / y; // within p
    auto [_, wrapperFp] = s->getFingerprint(query);
    auto [_p2, baseFp] = base->getFingerprint(query.removePrefix(p));
    RC_ASSERT(wrapperFp == baseFp);
}

/* SP6 — StripPrefix distributes over Layer:
   SP(p)(Layer([a,b])) ≡ Layer([SP(p)(a), SP(p)(b)]). Content at the
   stripped path on each leaf; query at the prefixed path. */
RC_GTEST_PROP(StripPrefixSourceAccessor, DistributesOverLayer, (const CanonPath & p, const CanonPath & y))
{
    auto leaf = y.isRoot() ? CanonPath("only-file") : y;
    auto a = make_ref<MemorySourceAccessor>();
    auto b = make_ref<MemorySourceAccessor>();
    a->addFile(leaf, "A");
    b->addFile(leaf, "B");

    auto lhs = spMakeStripPrefix(makeUnionSourceAccessor({a, b}), p);

    std::vector<ref<SourceAccessor>> rhsChildren;
    rhsChildren.push_back(spMakeStripPrefix(a, p));
    rhsChildren.push_back(spMakeStripPrefix(b, p));
    auto rhs = makeUnionSourceAccessor(std::move(rhsChildren));

    auto query = p / leaf;
    bool lOk = lhs->maybeLstat(query).has_value();
    bool rOk = rhs->maybeLstat(query).has_value();
    RC_ASSERT(lOk == rOk);
    RC_ASSERT(lOk); // non-vacuous: both must find it (first-wins → "A")
    RC_ASSERT(lhs->readFile(query) == rhs->readFile(query));
}

/* Monoid-action FAITHFULNESS (dual of Translate.ActionFaithful):
   p ≠ q ⇒ StripPrefix(p) ≢ StripPrefix(q). Distinct strip prefixes give
   observably distinct accessors; fusion + identity don't imply this.

   Witness: base = just a root directory. Query at z = p. StripPrefix(p)
   admits p (reflexive isWithin) and reads base@root = Some(Directory).
   StripPrefix(q) at p either DENIES (p ∉ q, since p ≠ q means p is not
   within q unless q is a strict ancestor of p) or, when q ⊏ p, reads
   base@(p.removePrefix(q)) which is a non-root path absent from the
   root-only base → nullopt. Either way distinct from Some(Directory). */
RC_GTEST_PROP(StripPrefixSourceAccessor, ActionFaithful, (const CanonPath & p, const CanonPath & q))
{
    RC_PRE(p != q);
    RC_PRE(!p.isRoot()); // query at p; need p non-root so q-at-p differs from root

    auto base = make_ref<MemorySourceAccessor>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});

    auto sp = spMakeStripPrefix(base, p);
    auto sq = spMakeStripPrefix(base, q);

    /* sp at p: admitted (p.isWithin(p)), strips to root → Directory. */
    auto rp = sp->maybeLstat(p);
    RC_ASSERT(rp.has_value());
    RC_ASSERT(rp->type == SourceAccessor::tDirectory);

    /* sq at p: denied (throws) if p ∉ q; else strips to a non-root path
       absent from the root-only base → nullopt. Never Some(Directory). */
    bool distinct = false;
    try {
        auto rq = sq->maybeLstat(p);
        distinct = !rq.has_value(); // absent ⇒ distinct from sp's Directory
    } catch (Error &) {
        distinct = true; // denied ⇒ distinct
    }
    RC_ASSERT(distinct);
}

/* L-ErrPreserve (StripPrefix arm) — for an IN-prefix path, StripPrefix's
   read forward constructs NO error of its own: `checkAccess` passes (the
   path is within the prefix) and the in-prefix branch forwards the
   stripped path straight to `next`. So the inner accessor's bespoke
   exception type AND message survive unchanged. We wrap a `ThrowingLeaf`
   (whose designated path is `p`) and read at `prefix / p`, which strips
   back to `p` and throws `CustomBespokeError` carrying the nonce; we
   assert BOTH the exact dynamic type (typed catch) AND that the message
   was derived-from (nonce still present), not replaced.

   This arm catches the §6.8.1 regression purely via the no-rewrite
   in-prefix forward: unlike Union, the in-prefix StripPrefix path has no
   `maybeLstat`-then-substitute step that could mask the inner error. The
   off-prefix deny (its own RestrictedPathError) is the SP3 tests above;
   here we pin that the IN-prefix channel is transparent. */
RC_GTEST_PROP(StripPrefixSourceAccessor, InPrefixErrorTypePreserved, (const CanonPath & prefix, const CanonPath & p))
{
    RC_PRE(!p.isRoot()); // p strict so prefix/p is a genuine in-prefix subpath

    auto leaf = make_ref<testHelpers::ThrowingLeaf>(p, "spErrPreserveNonce-2b8e");
    /* Raw make_ref (not the factory) so a root prefix still wraps — we
       want the transparent in-prefix forward, not the SP1 identity
       short-circuit (which would unwrap to the bare leaf, still valid but
       less direct a test of the wrapper's forward). */
    auto s = make_ref<StripPrefixSourceAccessor>(leaf.cast<SourceAccessor>(), prefix, testHelpers::makeOpErr());

    auto query = prefix / p; // within prefix → forwarded as `p` to the leaf
    RC_PRE(query.isWithin(prefix));

    bool threw = false, typedCatch = false, messageDerived = false;
    try {
        s->readFile(query);
    } catch (testHelpers::CustomBespokeError & e) {
        threw = typedCatch = true;
        messageDerived = e.msg().find("spErrPreserveNonce-2b8e") != std::string::npos;
    } catch (Error &) {
        /* A substituted error (e.g. RestrictedPathError / FileNotFound)
           leaves the typed flags false → the assertions below catch the
           mask. */
        threw = true;
    }
    RC_ASSERT(threw);
    RC_ASSERT(typedCatch);
    RC_ASSERT(messageDerived);
}

#endif

} // namespace nix
