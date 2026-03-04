#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepPrecisionAttrKeysMultiTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// Multi-source // shape observation (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionAttrKeysMultiTest, MultiProv_Update_AttrNames_KeyAdded_CacheMiss)
{
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    auto expr = std::format("builtins.attrNames ({} // {})", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2u);
    }

    fileB.modify(R"({"y": 2, "z": 3})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 3u);
    }
}

TEST_F(DepPrecisionAttrKeysMultiTest, MultiProv_Update_AttrNames_SameKeys_ValueChanged_CacheHit)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "y": 20})");
    auto expr = std::format("builtins.attrNames ({} // {})", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2u);
    }

    fileA.modify(R"({"x": 999, "y": 888})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_EQ(v.listSize(), 2u);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// intersectAttrs (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionAttrKeysMultiTest, IntersectAttrs_SameKeys_ValueChanged_CacheHit)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "y": 20})");
    auto expr = std::format("builtins.attrNames (builtins.intersectAttrs ({}) ({}))", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2u);
    }

    fileA.modify(R"({"x": 999, "y": 888})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_EQ(v.listSize(), 2u);
    }
}

TEST_F(DepPrecisionAttrKeysMultiTest, IntersectAttrs_DisjointKeys_KeyAdded_CacheMiss)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = std::format("builtins.attrNames (builtins.intersectAttrs ({}) ({}))", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 1u);
    }

    fileB.modify(R"({"x": 10, "y": 20, "z": 30})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 2u);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Chained // attrNames (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionAttrKeysMultiTest, ChainedUpdate_AttrNames_KeyAdded_CacheMiss)
{
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    TempJsonFile fileC(R"({"z": 3})");
    auto expr = std::format("builtins.attrNames ({} // {} // {})", fj(fileA.path), fj(fileB.path), fj(fileC.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3u);
    }

    fileC.modify(R"({"z": 3, "w": 4})");
    invalidateFileCache(fileC.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 4u);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Partial overlap (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionAttrKeysMultiTest, PartialOverlap_AttrNames_ValueChanged_CacheHit)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"y": 3, "z": 4})");
    auto expr = std::format("builtins.attrNames ({} // {})", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3u);
    }

    fileA.modify(R"({"x": 999, "y": 888})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_EQ(v.listSize(), 3u);
    }
}

TEST_F(DepPrecisionAttrKeysMultiTest, PartialOverlap_AttrNames_KeyAdded_ReEval)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"y": 3, "z": 4})");
    auto expr = std::format("builtins.attrNames ({} // {})", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3u);
    }

    fileA.modify(R"({"x": 1, "y": 2, "w": 99})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 4u);
    }
}

TEST_F(DepPrecisionAttrKeysMultiTest, PartialOverlap_AttrNames_KeyRemoved_ReEval)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"y": 3, "z": 4})");
    auto expr = std::format("builtins.attrNames ({} // {})", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3u);
    }

    fileA.modify(R"({"x": 1})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 3u);
    }
}

TEST_F(DepPrecisionAttrKeysMultiTest, PartialOverlap_AttrNames_BFileChanged_CacheHit)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"y": 3, "z": 4})");
    auto expr = std::format("builtins.attrNames ({} // {})", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3u);
    }

    fileB.modify(R"({"y": 999, "z": 888})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_EQ(v.listSize(), 3u);
    }
}

TEST_F(DepPrecisionAttrKeysMultiTest, PartialOverlap_SCKeys_NotRecordedForShadowed)
{
    // NEW: Dep verification of the precision fix.
    // When a // b with partial overlap, SC #keys is NOT recorded for the
    // shadowed source (a) because a's visible key count != a's total key count.
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"y": 3, "z": 4})");
    auto expr = std::format("builtins.attrNames ({} // {})", fj(fileA.path), fj(fileB.path));

    auto deps = evalAndCollectDeps(expr);

    // SC #keys should exist for b (2 visible = 2 total), but NOT for a (1 visible != 2 total)
    // Both should have IS #keys from creation
    EXPECT_TRUE(hasJsonDep(deps, DepType::ImplicitShape, shapePred("keys")))
        << "IS #keys must exist from creation\n" << dumpDeps(deps);

    // Count SC #keys — should be exactly 1 (from b only)
    size_t scKeysCount = countJsonDeps(deps, DepType::StructuredContent, shapePred("keys"));
    EXPECT_EQ(scKeysCount, 1u)
        << "SC #keys should be recorded for b only (not shadowed a)\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// intersectAttrs dep precision (from dep-precision-intersect.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionAttrKeysMultiTest, IntersectAttrs_RecordsHasKeyDeps)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = std::format("builtins.intersectAttrs ({}) ({})", fj(fileA.path), fj(fileB.path));

    auto deps = evalAndCollectDeps(expr);

    // intersectAttrs records #has:x on both operands (shared key)
    EXPECT_GE(countJsonDeps(deps, DepType::StructuredContent, hasKeyPred("x")), 2u)
        << "intersectAttrs must record #has:x on both operands\n" << dumpDeps(deps);
    // No #keys deps (replaced by per-key #has)
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "intersectAttrs must NOT record #keys\n" << dumpDeps(deps);
    // Non-shared keys get #has exists=false dep on the OTHER operand
    // (detects new keys entering the intersection)
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("y")))
        << "Left-only key y must get #has:y exists=false dep on right operand\n" << dumpDeps(deps);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("z")))
        << "Right-only key z must get #has:z exists=false dep on left operand\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionAttrKeysMultiTest, IntersectAttrs_SharedKeyChanged_CacheMiss)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = std::format("(builtins.intersectAttrs ({}) ({})).x", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(10));
    }

    fileB.modify(R"({"x": 99, "z": 30})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(99));
    }
}

TEST_F(DepPrecisionAttrKeysMultiTest, IntersectAttrs_DisjointKeyAdded_CacheHit)
{
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = std::format("(builtins.intersectAttrs ({}) ({})).x", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(10));
    }

    fileA.modify(R"({"x": 1, "y": 2})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "Adding disjoint key should not invalidate intersection";
        EXPECT_THAT(v, IsIntEq(10));
    }
}

TEST_F(DepPrecisionAttrKeysMultiTest, IntersectAttrs_DisjointKeyAdded_CacheHit_TraceCache)
{
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = std::format("(builtins.intersectAttrs ({}) ({})).x", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(10));
    }

    fileA.modify(R"({"x": 1, "y": 2})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "Adding disjoint key should not invalidate intersection";
        EXPECT_THAT(v, IsIntEq(10));
    }
}

TEST_F(DepPrecisionAttrKeysMultiTest, IntersectAttrs_ValueChangeUnrelatedKey_CacheHit)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = std::format("(builtins.intersectAttrs ({}) ({})).x", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(10));
    }

    fileA.modify(R"({"x": 1, "y": 99})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsIntEq(10));
    }
}

// Shared key "b" gets #has exists=true; disjoint keys get #has exists=false on other operand
TEST_F(DepPrecisionAttrKeysMultiTest, IntersectAttrs_HasKeyDepsForAllKeys)
{
    TempJsonFile fileA(R"({"a": 1, "b": 2})");
    TempJsonFile fileB(R"({"b": 10, "c": 30})");
    auto expr = std::format("builtins.intersectAttrs ({}) ({})", fj(fileA.path), fj(fileB.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("b")))
        << "Shared key b must get #has dep\n" << dumpDeps(deps);
    // Disjoint keys get exists=false on the other operand
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("a")))
        << "Left-only key a gets #has exists=false on right\n" << dumpDeps(deps);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("c")))
        << "Right-only key c gets #has exists=false on left\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "No #keys deps\n" << dumpDeps(deps);
}

// Empty intersection: all keys are non-shared → exists=false deps on each operand
TEST_F(DepPrecisionAttrKeysMultiTest, IntersectAttrs_EmptyIntersection_HasFalseDeps)
{
    TempJsonFile fileA(R"({"a": 1})");
    TempJsonFile fileB(R"({"b": 2})");
    auto expr = std::format("builtins.intersectAttrs ({}) ({})", fj(fileA.path), fj(fileB.path));
    auto deps = evalAndCollectDeps(expr);

    // "a" is left-only → #has:a exists=false on right (fileB)
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("a")))
        << "Left-only key a gets exists=false dep on right\n" << dumpDeps(deps);
    // "b" is right-only → #has:b exists=false on left (fileA)
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("b")))
        << "Right-only key b gets exists=false dep on left\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "No #keys deps\n" << dumpDeps(deps);
}

// Multiple shared keys: each gets #has dep; non-shared get exists=false
TEST_F(DepPrecisionAttrKeysMultiTest, IntersectAttrs_MultipleSharedKeys_AllGetHasDep)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2, "z": 3})");
    TempJsonFile fileB(R"({"x": 10, "y": 20, "w": 40})");
    auto expr = std::format("builtins.intersectAttrs ({}) ({})", fj(fileA.path), fj(fileB.path));
    auto deps = evalAndCollectDeps(expr);

    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
        << "Shared key x must get #has dep\n" << dumpDeps(deps);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("y")))
        << "Shared key y must get #has dep\n" << dumpDeps(deps);
    // Non-shared keys get exists=false on the other operand
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("z")))
        << "Left-only key z gets exists=false on right\n" << dumpDeps(deps);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("w")))
        << "Right-only key w gets exists=false on left\n" << dumpDeps(deps);
}

// Disjoint key removed from one operand — no dep for it → cache hit
TEST_F(DepPrecisionAttrKeysMultiTest, IntersectAttrs_DisjointKeyRemoved_CacheHit)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = std::format("(builtins.intersectAttrs ({}) ({})).x", fj(fileA.path), fj(fileB.path));
    { auto cache = makeCache(expr); auto v = forceRoot(*cache); EXPECT_THAT(v, IsIntEq(10)); }
    fileA.modify(R"({"x": 1})");  // Remove disjoint "y"
    invalidateFileCache(fileA.path);
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0) << "Removing disjoint key should not invalidate";
        EXPECT_THAT(v, IsIntEq(10));
    }
}

// Shared key removed — #has:x changes → cache miss
TEST_F(DepPrecisionAttrKeysMultiTest, IntersectAttrs_SharedKeyRemoved_CacheMiss)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = std::format("(builtins.intersectAttrs ({}) ({})).x", fj(fileA.path), fj(fileB.path));
    { auto cache = makeCache(expr); auto v = forceRoot(*cache); EXPECT_THAT(v, IsIntEq(10)); }
    fileA.modify(R"({"y": 2})");  // Remove shared "x" from left
    invalidateFileCache(fileA.path);
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Content changed → cache miss. Re-eval: intersection is now empty,
        // .x would fail (attribute missing).
        try { forceRoot(*cache); } catch (...) {}
        EXPECT_EQ(loaderCalls, 1) << "Removing shared key must invalidate";
    }
}

} // namespace nix::eval_trace
