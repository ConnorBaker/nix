/* Layer (Alternative `<|>` / first-wins) — Layer-1 read algebra L6.
   Tests cover associativity (L6a), left/right identity (L6b/L6c),
   singleton (L6d), empty (L6e), first-wins read (L6f), readDirectory
   merge (L6g), and the readImpl algebra fall-through. */

#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/union-source-accessor.hh"
#include "nix/util/tests/source-accessor.hh"

namespace nix {

namespace {

ref<MemorySourceAccessor> withFiles(std::initializer_list<std::pair<std::string, std::string>> files)
{
    auto m = make_ref<MemorySourceAccessor>();
    for (auto & [p, content] : files)
        m->addFile(CanonPath(p), std::string(content));
    return m;
}

ref<SourceAccessor> layer(std::vector<ref<SourceAccessor>> children)
{
    return makeLayer(std::move(children));
}

} // namespace

/* L6f: first-wins read. */
TEST(LayerSourceAccessor, FirstWinsRead)
{
    auto a = withFiles({{"shared.txt", "from-a"}});
    auto b = withFiles({{"shared.txt", "from-b"}});
    auto l = layer({a, b});
    EXPECT_EQ(l->readFile(CanonPath("/shared.txt")), "from-a");
}

/* L6f tail: fall-through to second when first lacks the path. */
TEST(LayerSourceAccessor, FallthroughRead)
{
    auto a = withFiles({{"only-a.txt", "A"}});
    auto b = withFiles({{"only-b.txt", "B"}});
    auto l = layer({a, b});
    EXPECT_EQ(l->readFile(CanonPath("/only-b.txt")), "B");
}

/* L6g: readDirectory merges children's listings (first-wins on conflicts). */
TEST(LayerSourceAccessor, ReadDirectoryMerges)
{
    auto a = withFiles({{"shared.txt", "from-a"}, {"only-a.txt", "A"}});
    auto b = withFiles({{"shared.txt", "from-b"}, {"only-b.txt", "B"}});
    auto l = layer({a, b});
    auto entries = l->readDirectory(CanonPath::root);
    EXPECT_TRUE(entries.contains("shared.txt"));
    EXPECT_TRUE(entries.contains("only-a.txt"));
    EXPECT_TRUE(entries.contains("only-b.txt"));
}

/* L6d: singleton short-circuit (not really a Layer at all). */
TEST(LayerSourceAccessor, SingletonShortCircuit)
{
    auto a = withFiles({{"x.txt", "X"}});
    auto l = layer({a});
    /* Reads still work; the "Layer" with one child reads from that child. */
    EXPECT_EQ(l->readFile(CanonPath("/x.txt")), "X");
}

/* L6e: empty Layer is the empty accessor. */
TEST(LayerSourceAccessor, EmptyIsEmpty)
{
    auto l = layer({});
    /* Reading anything throws (empty accessor has no files); root
       readDirectory returns empty (or throws — depending on impl).
       We just check no readFile succeeds. */
    EXPECT_THROW(l->readFile(CanonPath("/anything")), std::exception);
}

/* L6b: left identity — Layer([empty, a]) ≡ a. */
TEST(LayerSourceAccessor, LeftIdentityRead)
{
    auto e = makeEmptySourceAccessor();
    auto a = withFiles({{"x.txt", "X"}});
    auto withEmpty = layer({e, a});
    auto plain = a;
    EXPECT_EQ(withEmpty->readFile(CanonPath("/x.txt")), plain->readFile(CanonPath("/x.txt")));
}

/* L6c: right identity — Layer([a, empty]) ≡ a. */
TEST(LayerSourceAccessor, RightIdentityRead)
{
    auto e = makeEmptySourceAccessor();
    auto a = withFiles({{"x.txt", "X"}});
    auto withEmpty = layer({a, e});
    auto plain = a;
    EXPECT_EQ(withEmpty->readFile(CanonPath("/x.txt")), plain->readFile(CanonPath("/x.txt")));
}

/* L6a: associativity (observable form) — three children flatten to
   the same effective listing regardless of nesting. */
TEST(LayerSourceAccessor, Associativity)
{
    auto a = withFiles({{"a.txt", "A"}});
    auto b = withFiles({{"b.txt", "B"}});
    auto c = withFiles({{"c.txt", "C"}});
    auto leftAssoc = layer({layer({a, b}), c});
    auto rightAssoc = layer({a, layer({b, c})});
    auto leftEntries = leftAssoc->readDirectory(CanonPath::root);
    auto rightEntries = rightAssoc->readDirectory(CanonPath::root);
    EXPECT_EQ(leftEntries.size(), rightEntries.size());
    for (auto & [name, _] : leftEntries)
        EXPECT_TRUE(rightEntries.contains(name)) << "missing: " << name;
    /* And the actual content for each name agrees. */
    for (auto name : {std::string("a.txt"), std::string("b.txt"), std::string("c.txt")}) {
        EXPECT_EQ(leftAssoc->readFile(CanonPath("/" + name)), rightAssoc->readFile(CanonPath("/" + name)));
    }
}

/* L6a (observable form): nested Layers flatten, so listing `/sub` at
   any nesting yields the same set of names. The structural dynamic_cast
   check that pins the actual flattening (one Union, not a Union-of-Union)
   lives in `MakeLayerProducesUnionWithChildren` below. */
TEST(LayerSourceAccessor, AssociativityFlattens)
{
    /* This test asserts associativity holds for readDirectory at a
       child level too — i.e. listing /sub at any nesting yields the
       same set. */
    auto a = withFiles({{"sub/x.txt", "AX"}});
    auto b = withFiles({{"sub/y.txt", "BY"}});
    auto c = withFiles({{"sub/z.txt", "CZ"}});
    auto leftAssoc = layer({layer({a, b}), c});
    auto rightAssoc = layer({a, layer({b, c})});
    auto leftSub = leftAssoc->readDirectory(CanonPath("/sub"));
    auto rightSub = rightAssoc->readDirectory(CanonPath("/sub"));
    EXPECT_EQ(leftSub.size(), rightSub.size());
    for (auto & [name, _] : leftSub)
        EXPECT_TRUE(rightSub.contains(name));
}

#ifndef COVERAGE

/* PROP form of associativity: for arbitrary memory-accessor triples,
   left- and right-associated layers produce the same reads at any
   queried path. */
RC_GTEST_PROP(
    LayerSourceAccessor,
    AssociativityProp,
    (const MemorySourceAccessor & a,
     const MemorySourceAccessor & b,
     const MemorySourceAccessor & c,
     const CanonPath & x))
{
    auto pa = make_ref<MemorySourceAccessor>(a);
    auto pb = make_ref<MemorySourceAccessor>(b);
    auto pc = make_ref<MemorySourceAccessor>(c);
    auto leftAssoc = layer({layer({pa, pb}), pc});
    auto rightAssoc = layer({pa, layer({pb, pc})});

    /* Compare: does each side return the same maybeLstat shape?
       Both accessors share the same underlying memory accessors `pa`,
       `pb`, `pc`, so if one side throws (e.g. SymlinkNotAllowed for a
       path with a symlink ancestor — possible under the symlink-aware
       Arbitrary<MemorySourceAccessor> generator), the other side must
       throw too. We assert the throw symmetry rather than discarding,
       since divergent throw behaviour would be a real associativity
       violation. */
    bool lThrew = false, rThrew = false;
    std::optional<SourceAccessor::Stat> lst, rst;
    try {
        lst = leftAssoc->maybeLstat(x);
    } catch (Error &) {
        lThrew = true;
    }
    try {
        rst = rightAssoc->maybeLstat(x);
    } catch (Error &) {
        rThrew = true;
    }
    RC_ASSERT(lThrew == rThrew);
    if (!lThrew) {
        RC_ASSERT(lst.has_value() == rst.has_value());
        /* Strengthening (gap #3): associativity must preserve the
           readDirectory MERGE, not just presence. Compare the merged
           name-sets at x when it's a directory on both sides — the
           earlier has_value-only assertion would pass even if the
           listing-merge order/content diverged at a non-root path. */
        if (lst.has_value() && lst->type == SourceAccessor::tDirectory) {
            std::set<std::string> lnames, rnames;
            bool lThrew2 = false, rThrew2 = false;
            try {
                for (auto & [n, _] : leftAssoc->readDirectory(x))
                    lnames.insert(n);
            } catch (Error &) {
                lThrew2 = true;
            }
            try {
                for (auto & [n, _] : rightAssoc->readDirectory(x))
                    rnames.insert(n);
            } catch (Error &) {
                rThrew2 = true;
            }
            RC_ASSERT(lThrew2 == rThrew2);
            if (!lThrew2)
                RC_ASSERT(lnames == rnames);
        }
    }
}

/* L6 idempotence: Layer([a, a]) ≡ a. Merging an accessor with itself
   changes nothing — readDirectory name-sets agree (first-wins dedupes
   the duplicate child) and a present file reads the same bytes. The
   readDirectory-merge monoid is thus idempotent on a repeated child
   (the OverlayFS "same lower twice" degenerate case). */
RC_GTEST_PROP(LayerSourceAccessor, MergeIdempotent, (const MemorySourceAccessor & a, const CanonPath & x))
{
    auto pa = make_ref<MemorySourceAccessor>(a);
    auto l = layer({pa, pa});

    /* maybeLstat agreement (throw-for-throw). */
    bool lThrew = false, aThrew = false;
    std::optional<SourceAccessor::Stat> lst, ast;
    try {
        lst = l->maybeLstat(x);
    } catch (Error &) {
        lThrew = true;
    }
    try {
        ast = pa->maybeLstat(x);
    } catch (Error &) {
        aThrew = true;
    }
    RC_ASSERT(lThrew == aThrew);
    if (lThrew)
        return;
    RC_ASSERT(lst.has_value() == ast.has_value());
    if (!lst.has_value())
        return;
    RC_ASSERT(lst->type == ast->type);

    if (lst->type == SourceAccessor::tDirectory) {
        std::set<std::string> lnames, anames;
        for (auto & [n, _] : l->readDirectory(x))
            lnames.insert(n);
        for (auto & [n, _] : pa->readDirectory(x))
            anames.insert(n);
        RC_ASSERT(lnames == anames);
    } else if (lst->type == SourceAccessor::tRegular) {
        RC_ASSERT(l->readFile(x) == pa->readFile(x));
    }
}

/* Negative PROP: Layer([a,b]).maybeLstat(x) is None when neither a
   nor b has x. Layer is Alternative — it doesn't invent paths. */
RC_GTEST_PROP(
    LayerSourceAccessor,
    NegativeMaybeLstatBothAbsent,
    (const MemorySourceAccessor & a, const MemorySourceAccessor & b, const CanonPath & x))
{
    auto pa = make_ref<MemorySourceAccessor>(a);
    auto pb = make_ref<MemorySourceAccessor>(b);

    /* Only proceed when neither side has x. */
    bool aHas = false, bHas = false;
    try {
        aHas = pa->maybeLstat(x).has_value();
    } catch (Error &) {
    }
    try {
        bHas = pb->maybeLstat(x).has_value();
    } catch (Error &) {
    }
    RC_PRE(!aHas && !bHas);

    auto l = layer({pa, pb});
    bool lHas = false;
    try {
        lHas = l->maybeLstat(x).has_value();
    } catch (Error &) {
    }
    RC_ASSERT(!lHas);
}

#endif

/* Structural: makeLayer produces a UnionSourceAccessor whose children
   are the (flattened) inputs. This is the reason UnionSourceAccessor was
   moved to a header — so verification tests can dynamic_cast and inspect
   the resulting shape. */
TEST(LayerSourceAccessor, MakeLayerProducesUnionWithChildren)
{
    auto a = withFiles({{"a.txt", "A"}});
    auto b = withFiles({{"b.txt", "B"}});
    auto c = withFiles({{"c.txt", "C"}});

    auto two = makeLayer({a, b});
    auto u = two.dynamic_pointer_cast<UnionSourceAccessor>();
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->accessors.size(), 2u);

    /* L6a: nested Layers flatten — Layer([a, Layer([b, c])]) has three
       direct children, not two with a nested union. */
    auto nested = makeLayer({a, makeLayer({b, c})});
    auto un = nested.dynamic_pointer_cast<UnionSourceAccessor>();
    ASSERT_NE(un, nullptr);
    EXPECT_EQ(un->accessors.size(), 3u);
    for (auto & child : un->accessors)
        EXPECT_EQ(child.dynamic_pointer_cast<UnionSourceAccessor>(), nullptr);
}

} // namespace nix
