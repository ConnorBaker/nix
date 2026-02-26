#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"

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
        EXPECT_EQ(v.listSize(), 2);
    }

    fileB.modify(R"({"y": 2, "z": 3})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 3);
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
        EXPECT_EQ(v.listSize(), 2);
    }

    fileA.modify(R"({"x": 999, "y": 888})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_EQ(v.listSize(), 2);
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
        EXPECT_EQ(v.listSize(), 2);
    }

    fileA.modify(R"({"x": 999, "y": 888})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_EQ(v.listSize(), 2);
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
        EXPECT_EQ(v.listSize(), 1);
    }

    fileB.modify(R"({"x": 10, "y": 20, "z": 30})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 2);
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
        EXPECT_EQ(v.listSize(), 3);
    }

    fileC.modify(R"({"z": 3, "w": 4})");
    invalidateFileCache(fileC.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 4);
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
        EXPECT_EQ(v.listSize(), 3);
    }

    fileA.modify(R"({"x": 999, "y": 888})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_EQ(v.listSize(), 3);
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
        EXPECT_EQ(v.listSize(), 3);
    }

    fileA.modify(R"({"x": 1, "y": 2, "w": 99})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 4);
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
        EXPECT_EQ(v.listSize(), 3);
    }

    fileA.modify(R"({"x": 1})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 3);
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
        EXPECT_EQ(v.listSize(), 3);
    }

    fileB.modify(R"({"y": 999, "z": 888})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_EQ(v.listSize(), 3);
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
    EXPECT_TRUE(hasDep(deps, DepType::ImplicitShape, "#keys"))
        << "IS #keys must exist from creation\n" << dumpDeps(deps);

    // Count SC #keys — should be exactly 1 (from b only)
    size_t scKeysCount = countDeps(deps, DepType::StructuredContent, "#keys");
    EXPECT_EQ(scKeysCount, 1u)
        << "SC #keys should be recorded for b only (not shadowed a)\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// intersectAttrs dep precision (from dep-precision-intersect.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionAttrKeysMultiTest, IntersectAttrs_RecordsSCKeys_BothOperands)
{
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = std::format("builtins.intersectAttrs ({}) ({})", fj(fileA.path), fj(fileB.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_GE(countDeps(deps, DepType::StructuredContent, "#keys"), 2u)
        << "intersectAttrs currently records #keys for both operands\n" << dumpDeps(deps);
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

TEST_F(DepPrecisionAttrKeysMultiTest, DISABLED_IntersectAttrs_DisjointKeyAdded_CacheHit)
{
    // CURRENTLY FAILS: #keys on a is recorded, so key addition invalidates.
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

TEST_F(DepPrecisionAttrKeysMultiTest, IntersectAttrs_CurrentlyInvalidatesOnDisjointKeyAdd)
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
        EXPECT_EQ(loaderCalls, 1)
            << "KNOWN IMPRECISION: intersectAttrs records #keys on both operands";
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

} // namespace nix::eval_trace
