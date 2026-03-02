#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepPrecisionListLenTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// Dep verification: length records SC #len
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, Length_RecordsSCLen)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = std::format("builtins.length ({}).items", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("len")))
        << "length must record SC #len\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "length must NOT record SC #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionListLenTest, Head_NoSCLen)
{
    TempJsonFile file(R"({"items": ["a", "b"]})");
    auto expr = std::format("builtins.head ({}).items", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("len")))
        << "head must NOT record SC #len\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionListLenTest, ElemAt_NoSCLen)
{
    TempJsonFile file(R"({"items": ["a", "b"]})");
    auto expr = std::format("builtins.elemAt ({}).items 0", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("len")))
        << "elemAt must NOT record SC #len\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Core bug reproduction (from shape-core.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, LengthPlusElemAt_ShapeChange_CacheMiss)
{
    TempJsonFile file(R"({"arr": ["alpha", "beta"]})");
    auto expr = std::format(
        "let j = {}; in (toString (builtins.length j.arr)) + \"-\" + (builtins.elemAt j.arr 0)",
        fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("2-alpha"));
    }

    file.modify(R"({"arr": ["alpha", "beta", "gamma"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len fails
        EXPECT_THAT(v, IsStringEq("3-alpha"));
    }
}

TEST_F(DepPrecisionListLenTest, LengthPlusElemAt_NoShapeChange_CacheHit)
{
    TempJsonFile file(R"({"arr": ["alpha", "beta"]})");
    auto expr = std::format(
        "let j = {}; in (toString (builtins.length j.arr)) + \"-\" + (builtins.elemAt j.arr 0)",
        fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("2-alpha"));
    }

    file.modify(R"({"arr": ["alpha", "CHANGED"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("2-alpha"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// List length shape deps (from shape-core.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, LengthOnly_ShapeChange_CacheMiss)
{
    TempJsonFile file(R"({"arr": ["a", "b"]})");
    auto expr = std::format("let j = {}; in builtins.length j.arr", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"arr": ["a", "b", "c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(DepPrecisionListLenTest, LengthOnly_ContentChange_CacheHit)
{
    TempJsonFile file(R"({"arr": ["a", "b"]})");
    auto expr = std::format("let j = {}; in builtins.length j.arr", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"arr": ["CHANGED!", "b"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsIntEq(2));
    }
}

TEST_F(DepPrecisionListLenTest, RootArrayLength_ShapeChange_CacheMiss)
{
    TempJsonFile file(R"(["x", "y", "z"])");
    auto expr = std::format("builtins.length ({})", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"(["x", "y", "z", "w"])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(4));
    }
}

TEST_F(DepPrecisionListLenTest, NestedListLength_ShapeChange_CacheMiss)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = std::format("let j = {}; in builtins.length j.items", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [1, 2, 3, 4]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(4));
    }
}

TEST_F(DepPrecisionListLenTest, ListElementAdded_PointAccess_CacheHit)
{
    TempJsonFile file(R"({"items": ["alpha", "beta"]})");
    auto expr = std::format("builtins.elemAt ({}).items 0", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("alpha"));
    }

    file.modify(R"({"items": ["alpha", "beta", "gamma"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("alpha"));
    }
}

TEST_F(DepPrecisionListLenTest, ListSizeChange_NoLeafAccess_CacheMiss)
{
    TempJsonFile file(R"(["a", "b", "c"])");
    auto expr = std::format("{}", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify(R"(["a", "b", "c", "d"])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified -- filter (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, Filter_Length_ElementRemoved_CacheMiss)
{
    TempJsonFile file(R"({"items": [1, 10, 2, 20]})");
    auto expr = std::format(
        "builtins.length (builtins.filter (x: x > 5) ({}).items)", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"items": [1, 10, 2, 20, 30]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len suppressed -> re-eval
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(DepPrecisionListLenTest, Filter_Length_AllKept_CacheMiss)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = std::format(
        "builtins.length (builtins.filter (x: true) ({}).items)", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [1, 2, 3, 4]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len NOT suppressed, detects change
        EXPECT_THAT(v, IsIntEq(4));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified -- tail (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, Tail_Length_ElementAdded_CacheMiss)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = std::format(
        "builtins.length (builtins.tail ({}).items)", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"items": [1, 2, 3, 4]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len suppressed -> re-eval
        EXPECT_THAT(v, IsIntEq(3));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified -- sort (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, Sort_Length_ElementAdded_CacheMiss)
{
    TempJsonFile file(R"({"items": [3, 1, 2]})");
    auto expr = std::format(
        "builtins.length (builtins.sort (a: b: a < b) ({}).items)", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [3, 1, 2, 0]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len NOT suppressed, detects change
        EXPECT_THAT(v, IsIntEq(4));
    }
}

TEST_F(DepPrecisionListLenTest, Sort_Length_ValueChanged_CacheMiss)
{
    TempJsonFile file(R"({"items": [3, 1, 2]})");
    auto expr = std::format(
        "builtins.length (builtins.sort (a: b: a < b) ({}).items)", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [9, 8, 7]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC deps from comparator fail -> re-eval
        EXPECT_THAT(v, IsIntEq(3));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified -- multi-source ++ (from shape-modified.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, Concat_EmptyPlusTracked_ValueChanged_CacheHit)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = std::format("builtins.length ([] ++ ({}).items)", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [9, 8, 7]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // #len passes -- same count
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(DepPrecisionListLenTest, Concat_BothTracked_ElementAdded_CacheMiss)
{
    TempJsonFile fileA(R"({"items": [1, 2]})");
    TempJsonFile fileB(R"({"items": [3, 4]})");
    auto expr = std::format(
        "builtins.length (({}).items ++ ({}).items)", fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(4));
    }

    fileB.modify(R"({"items": [3, 4, 5]})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len suppressed -> re-eval
        EXPECT_THAT(v, IsIntEq(5));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Scalar dep correctness (from builtins-access.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, Map_SameLength_ValueChange_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.concatStringsSep "," (builtins.map (x: x) j.items))",
        fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }

    file.modify(R"({"items":["x","y"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("x,y"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// TOML length (from shape-core.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenTest, TOML_LengthChange_CacheMiss)
{
    TempTomlFile file("items = [\"a\", \"b\"]\n");
    auto expr = std::format("let t = {}; in builtins.length t.items", ft(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify("items = [\"a\", \"b\", \"c\"]\n");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len fails
        EXPECT_THAT(v, IsIntEq(3));
    }
}

} // namespace nix::eval_trace
