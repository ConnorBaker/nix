#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepPrecisionPipelinesTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// Multi-operation pipelines: dep verification + cache behavior
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionPipelinesTest, FilterMapLength_CacheMiss)
{
    // length (map f (filter p items)) -- multiple #len deps at each stage
    TempJsonFile file(R"({"items": [1, 10, 2, 20]})");
    auto expr = std::format(
        "let j = {}; in builtins.length (builtins.map (x: x) (builtins.filter (x: x > 5) j.items))",
        fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#len"))
            << "Pipeline must record SC #len\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2)); // [10, 20]
    }

    file.modify(R"({"items": [1, 10, 2, 20, 30]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(3)); // [10, 20, 30]
    }
}

TEST_F(DepPrecisionPipelinesTest, AttrNamesMapListToAttrs_CacheMiss)
{
    // attrNames (listToAttrs (map f items)) -- #len on items, #keys on result
    TempJsonFile file(R"({"items": [{"name": "a", "value": 1}, {"name": "b", "value": 2}]})");
    auto expr = std::format(
        "let j = {}; in builtins.attrNames (builtins.listToAttrs j.items)",
        fj(file.path));

    // -- Dep verification --
    // listToAttrs records SC #len on input list; the output attrset is Nix-created
    // (not TracedData), so attrNames does NOT record SC #keys on it
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#len"))
            << "listToAttrs records SC #len on input\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    file.modify(R"({"items": [{"name": "a", "value": 1}, {"name": "b", "value": 2}, {"name": "c", "value": 3}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 3);
    }
}

TEST_F(DepPrecisionPipelinesTest, UpdateMapAttrsAttrNames_CacheMiss)
{
    // attrNames (mapAttrs id (a // b)) -- SC #keys from attrNames, IS #keys from sources
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    auto expr = std::format(
        "builtins.attrNames (builtins.mapAttrs (n: v: v) ({} // {}))",
        fj(fileA.path), fj(fileB.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::ImplicitShape, "#keys"))
            << "IS #keys from both sources\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
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

TEST_F(DepPrecisionPipelinesTest, FilterSortLength_CacheMiss)
{
    // length (sort lt (filter p items)) -- #len at each stage
    TempJsonFile file(R"({"items": ["b", "a", "c"]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.length (builtins.sort (a: b: a < b) (builtins.filter (x: x != "c") j.items)))",
        fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2)); // ["a", "b"]
    }

    file.modify(R"({"items": ["b", "a", "c", "d"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(3)); // ["a", "b", "d"]
    }
}

TEST_F(DepPrecisionPipelinesTest, ConcatMapGroupByAttrNames_CacheMiss)
{
    // attrNames (groupBy id (concatMap f items))
    TempJsonFile file(R"({"items": ["a", "b"]})");
    auto expr = std::format(
        "let j = {}; in builtins.attrNames (builtins.groupBy (x: x) (builtins.concatMap (x: [x]) j.items))",
        fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2); // ["a", "b"]
    }

    file.modify(R"({"items": ["a", "b", "c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 3);
    }
}

TEST_F(DepPrecisionPipelinesTest, FoldlFilterPartition_CacheMiss)
{
    // length ((partition p items).right)
    TempJsonFile file(R"({"items": [1, 5, 2, 8]})");
    auto expr = std::format(
        "let j = {}; in builtins.length (builtins.partition (x: x < 4) j.items).right",
        fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2)); // [1, 2]
    }

    file.modify(R"({"items": [1, 5, 2, 8, 3]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(3)); // [1, 2, 3]
    }
}

TEST_F(DepPrecisionPipelinesTest, NestedUpdateScalarAccess_CacheHit)
{
    // (a // b // c).deep.nested.key -- only scalar SC dep for key, IS #keys from all 3
    TempJsonFile fileA(R"({"deep": {"nested": {"key": "val"}}})");
    TempJsonFile fileB(R"({"other": 1})");
    TempJsonFile fileC(R"({"extra": 2})");
    auto expr = std::format("({} // {} // {}).deep.nested.key",
        fj(fileA.path), fj(fileB.path), fj(fileC.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "key"))
            << "Nested scalar access must record SC dep\n" << dumpDeps(deps);
        EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
            << "No SC #keys from //\n" << dumpDeps(deps);
        EXPECT_GE(countDeps(deps, DepType::ImplicitShape, "#keys"), 3u)
            << "IS #keys from all 3 sources\n" << dumpDeps(deps);
    }

    // -- Cache behavior: change unrelated values --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("val"));
    }

    fileB.modify(R"({"other": 99})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0) << "IS fallback: b's keys unchanged";
        EXPECT_THAT(v, IsStringEq("val"));
    }
}

TEST_F(DepPrecisionPipelinesTest, MapAttrsFilterAttrNames_CacheMiss)
{
    // attrNames (mapAttrs f (fromJSON ...)) -- PosIdx preserved through mapAttrs
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format(
        "builtins.attrNames (builtins.mapAttrs (n: v: v + 1) ({}))", fj(file.path));

    // -- Dep verification --
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
            << "attrNames after mapAttrs records SC #keys\n" << dumpDeps(deps);
    }

    // -- Cache behavior --
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    file.modify(R"({"a": 1, "b": 2, "c": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 3);
    }
}

TEST_F(DepPrecisionPipelinesTest, RemoveAttrsIntersectAttrNames_CacheMiss)
{
    // attrNames (intersectAttrs (removeAttrs a [...]) b)
    TempJsonFile fileA(R"({"x": 1, "y": 2, "z": 3})");
    TempJsonFile fileB(R"({"x": 10, "y": 20})");
    auto expr = std::format(
        R"(builtins.attrNames (builtins.intersectAttrs (builtins.removeAttrs ({}) ["z"]) ({})))",
        fj(fileA.path), fj(fileB.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2); // x, y
    }

    fileA.modify(R"({"x": 1, "y": 2, "z": 3, "w": 4})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys dep on a detects change
    }
}

} // namespace nix::eval_trace
