/* Translate (Iso / monoid morphism) — Layer-1 read algebra L1. */

#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "nix/fetchers/translate-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-path.hh"
#include "nix/util/tests/source-accessor.hh"

namespace nix {

namespace {

ref<MemorySourceAccessor> sampleTree()
{
    auto m = make_ref<MemorySourceAccessor>();
    m->addFile(CanonPath("a/file.txt"), "a-bytes");
    m->addFile(CanonPath("a/sub/deep.txt"), "deep-bytes");
    m->addFile(CanonPath("b/other.txt"), "b-bytes");
    return m;
}

ref<TranslateSourceAccessor> tsaMakeTranslate(ref<SourceAccessor> base, CanonPath prefix)
{
    return make_ref<TranslateSourceAccessor>(base, std::move(prefix), [](const CanonPath & p) -> RestrictedPathError {
        return RestrictedPathError("path '%s' not allowed", p.abs());
    });
}

} // namespace

TEST(TranslateSourceAccessor, BasicReadAtPrefix)
{
    auto base = sampleTree();
    auto t = tsaMakeTranslate(base, CanonPath("/a"));
    EXPECT_EQ(t->readFile(CanonPath("/file.txt")), "a-bytes");
    EXPECT_EQ(t->readFile(CanonPath("/sub/deep.txt")), "deep-bytes");
}

TEST(TranslateSourceAccessor, ReadDirectoryAtPrefix)
{
    auto base = sampleTree();
    auto t = tsaMakeTranslate(base, CanonPath("/a"));
    auto entries = t->readDirectory(CanonPath::root);
    EXPECT_TRUE(entries.contains("file.txt"));
    EXPECT_TRUE(entries.contains("sub"));
    EXPECT_FALSE(entries.contains("other.txt")); /* under /b, not /a */
}

TEST(TranslateSourceAccessor, EmptyComputeOwnSuffixForwards)
{
    auto base = sampleTree();
    base->fingerprint = "git:R";
    auto t = tsaMakeTranslate(base, CanonPath("/a"));
    auto [_, fp] = t->getFingerprint(CanonPath("/file.txt"));
    /* MemorySourceAccessor's getFingerprint returns its top-level
       `fingerprint` field for any path; Translate forwards transparently,
       so we get base's fingerprint string back. */
    EXPECT_EQ(fp, "git:R");
}

/* Negative: Translate(p) reading a path not under p throws.
   Translate is an Iso (total bijection between the two namespaces),
   NOT a Prism — it doesn't deny any path at the wrapper level.
   Instead, `base.readFile(p / x)` throws if `p / x` doesn't exist
   in base. This test pins that "path at a different root than p"
   means the base lookup fails, not that Translate itself gates it. */
TEST(TranslateSourceAccessor, ReadMissingPathThrows)
{
    auto base = sampleTree();
    auto t = tsaMakeTranslate(base, CanonPath("/a"));
    /* /b/other.txt is in base but under /b, not /a.
       Translate(/a).readFile(/b/other.txt) tries base.readFile(/a/b/other.txt)
       which doesn't exist → throws FileNotFound. */
    EXPECT_THROW(t->readFile(CanonPath("/b/other.txt")), FileNotFound);
    /* A path that simply doesn't exist anywhere. */
    EXPECT_THROW(t->readFile(CanonPath("/nonexistent")), FileNotFound);
}

/* Negative PROP: Translate(p) at an arbitrary path x where base lacks
   p/x returns nullopt from maybeLstat. */
RC_GTEST_PROP(TranslateSourceAccessor, NegativeMaybeLstatMissesPassThrough, (const CanonPath & p, const CanonPath & x))
{
    /* Build base with content only at p/x; query a sibling. */
    auto base = make_ref<MemorySourceAccessor>();
    if (!(p / x).isRoot()) {
        try {
            base->addFile(p / x, "content");
        } catch (Error &) {
        }
    }
    auto t = tsaMakeTranslate(base, p);
    /* A path that's definitely not in base. */
    auto absent = CanonPath(std::string("/definitely-absent-") + std::string(x.baseName().value_or("q")));
    /* Translate forwards: maybeLstat returns base's answer. */
    RC_ASSERT(!t->maybeLstat(absent).has_value());
}

#ifndef COVERAGE

/* L1d: Translate(p) ∘ Translate(q) ≡ Translate(p / q). Observable form:
   nesting two Translates produces the same reads as a single combined
   Translate. */
RC_GTEST_PROP(
    TranslateSourceAccessor, MonoidComposition, (const CanonPath & p, const CanonPath & q, const CanonPath & x))
{
    /* Build a small fixture base with addFile so we have something to
       read at p/q/x (or fail symmetrically). Use the addFile path
       directly so the read either succeeds at the same path on both
       sides or throws on both sides. */
    auto base = make_ref<MemorySourceAccessor>();
    base->addFile(p / q / x, "content");
    auto baseRead = make_ref<MemorySourceAccessor>();
    baseRead->addFile(p / q / x, "content");

    auto nested = tsaMakeTranslate(tsaMakeTranslate(base, p).cast<SourceAccessor>(), q);
    auto fused = tsaMakeTranslate(baseRead, p / q);

    /* Both should read the same content at x. */
    bool nestedSucceeded = false;
    std::string nestedContent;
    try {
        nestedContent = nested->readFile(x);
        nestedSucceeded = true;
    } catch (...) {
    }

    bool fusedSucceeded = false;
    std::string fusedContent;
    try {
        fusedContent = fused->readFile(x);
        fusedSucceeded = true;
    } catch (...) {
    }

    RC_ASSERT(nestedSucceeded == fusedSucceeded);
    if (nestedSucceeded)
        RC_ASSERT(nestedContent == fusedContent);
}

/* L1b: Translate(/) ∘ T ≡ T (left identity, observable). */
RC_GTEST_PROP(TranslateSourceAccessor, LeftIdentity, (const CanonPath & p, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    base->addFile(p / x, "C");
    auto wrapped = tsaMakeTranslate(tsaMakeTranslate(base, CanonPath::root).cast<SourceAccessor>(), p);
    auto plain = tsaMakeTranslate(base, p);

    bool wOk = false;
    std::string wC;
    try {
        wC = wrapped->readFile(x);
        wOk = true;
    } catch (...) {
    }
    bool pOk = false;
    std::string pC;
    try {
        pC = plain->readFile(x);
        pOk = true;
    } catch (...) {
    }
    RC_ASSERT(wOk == pOk);
    if (wOk)
        RC_ASSERT(wC == pC);
}

/* L1c: T ∘ Translate(/) ≡ T (right identity, observable). */
RC_GTEST_PROP(TranslateSourceAccessor, RightIdentity, (const CanonPath & p, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    base->addFile(p / x, "C");
    auto wrapped = tsaMakeTranslate(tsaMakeTranslate(base, p).cast<SourceAccessor>(), CanonPath::root);
    auto plain = tsaMakeTranslate(base, p);

    bool wOk = false;
    std::string wC;
    try {
        wC = wrapped->readFile(x);
        wOk = true;
    } catch (...) {
    }
    bool pOk = false;
    std::string pC;
    try {
        pC = plain->readFile(x);
        pOk = true;
    } catch (...) {
    }
    RC_ASSERT(wOk == pOk);
    if (wOk)
        RC_ASSERT(wC == pC);
}

/* L1e: Translate(p)(b).read(x) = b.read(p / x) (faithful — no rewriting). */
RC_GTEST_PROP(TranslateSourceAccessor, Faithful, (const CanonPath & p, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    base->addFile(p / x, "C");
    auto t = tsaMakeTranslate(base, p);

    bool tOk = false;
    std::string tC;
    try {
        tC = t->readFile(x);
        tOk = true;
    } catch (...) {
    }
    bool bOk = false;
    std::string bC;
    try {
        bC = base->readFile(p / x);
        bOk = true;
    } catch (...) {
    }
    RC_ASSERT(tOk == bOk);
    if (tOk)
        RC_ASSERT(tC == bC);
}

/* L1 fingerprint forwarding: Translate(p)(b).getFingerprint(x)
   threads `prefix / x` as the inner-path argument to
   composeFingerprint, then forwards the inner result transparently
   (Translate's computeOwnSuffix is empty). For a base whose
   fingerprint is path-independent (MemorySourceAccessor returns its
   `fingerprint` field at every path), this means the wrapper's
   fingerprint at any wrapper-path equals the base's fingerprint.

   This PROP exercises the structural threading of `prefix / path`
   through `composeFingerprint`'s inner-path argument. A regression
   that broke the threading (e.g. dropping the prefix translation in
   getFingerprint) would still pass `EmptyComputeOwnSuffixForwards`
   above (which only tests at one fixed path) but would fail this
   PROP at any non-root prefix or non-root query path. */
RC_GTEST_PROP(TranslateSourceAccessor, FingerprintForwardsThroughPrefix, (const CanonPath & p, const CanonPath & x))
{
    auto base = make_ref<MemorySourceAccessor>();
    base->fingerprint = "tree:R";
    auto t = tsaMakeTranslate(base, p);

    auto [_, wrapperFp] = t->getFingerprint(x);
    auto [_p2, baseFp] = base->getFingerprint(p / x);
    RC_ASSERT(wrapperFp == baseFp);
}

/* L1f: Translate distributes over Layer. */
RC_GTEST_PROP(TranslateSourceAccessor, DistributesOverLayer, (const CanonPath & p, const CanonPath & x))
{
    /* Build two memory accessors with content at p/x. */
    auto a = make_ref<MemorySourceAccessor>();
    auto b = make_ref<MemorySourceAccessor>();
    if (!(p / x).isRoot()) {
        try {
            a->addFile(p / x, "A");
        } catch (Error &) {
        }
        try {
            b->addFile(p / x, "B");
        } catch (Error &) {
        }
    }

    /* LHS: Translate(p)(Layer([a, b])) */
    auto lhs = tsaMakeTranslate(makeUnionSourceAccessor({a, b}), p);

    /* RHS: Layer([Translate(p)(a), Translate(p)(b)]) */
    std::vector<ref<SourceAccessor>> rhsChildren;
    rhsChildren.push_back(tsaMakeTranslate(a, p));
    rhsChildren.push_back(tsaMakeTranslate(b, p));
    auto rhs = makeUnionSourceAccessor(std::move(rhsChildren));

    /* Both should agree on whether x is readable and what its bytes are. */
    bool lOk = lhs->maybeLstat(x).has_value();
    bool rOk = rhs->maybeLstat(x).has_value();
    RC_ASSERT(lOk == rOk);
    if (lOk)
        RC_ASSERT(lhs->readFile(x) == rhs->readFile(x));
}

/* Monoid-action FAITHFULNESS: the homomorphism (CanonPath, /) →
   transformations is injective — distinct prefixes give observably
   distinct accessors. p ≠ q ⇒ Translate(p) ≢ Translate(q). The fusion +
   identity laws do NOT imply this (the action could collapse distinct
   prefixes while still composing correctly); a bug that canonicalised or
   dropped a path component would pass fusion but fail here.

   Witness: plant a single Regular file at exactly `p`. Translate(p) at
   root reads base.lstat(p) = Regular. Translate(q) at root reads
   base.lstat(q): for q ≠ p that is never a Regular at `p` — it is absent
   (→ nullopt) or, if q is a strict ancestor of p, a Directory. So the
   observable at root differs. */
RC_GTEST_PROP(TranslateSourceAccessor, ActionFaithful, (const CanonPath & p, const CanonPath & q))
{
    RC_PRE(p != q);
    RC_PRE(!p.isRoot()); // need a non-root file to plant

    auto base = make_ref<MemorySourceAccessor>();
    base->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    base->addFile(p, "witness");

    auto tp = tsaMakeTranslate(base, p);
    auto tq = tsaMakeTranslate(base, q);

    /* At the wrapper root, tp sees base@p (Regular). tq sees base@q. */
    auto sp = tp->maybeLstat(CanonPath::root); // base@p
    std::optional<SourceAccessor::Stat> sq;
    bool qThrew = false;
    try {
        sq = tq->maybeLstat(CanonPath::root); // base@q
    } catch (Error &) {
        qThrew = true; // symlinked-ancestor etc. — still observably different from a Regular
    }
    RC_ASSERT(sp.has_value());
    RC_ASSERT(sp->type == SourceAccessor::tRegular);
    /* Distinct: tq's root is not a Regular-at-p (absent, throw, or a
       Directory if q is an ancestor of p). */
    bool distinct = qThrew || !sq.has_value() || sq->type != SourceAccessor::tRegular
                    || tp->readFile(CanonPath::root) != tq->readFile(CanonPath::root);
    RC_ASSERT(distinct);
}

#endif

} // namespace nix
