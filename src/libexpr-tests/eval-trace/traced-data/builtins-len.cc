#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/traced-data.hh"
#include "nix/expr/json-to-value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Category B — #len from iterating builtins ───────────────────────

TEST_F(TracedDataTest, TracedJSON_Map_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.map (x: x) j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,b,c"));
    }
}

TEST_F(TracedDataTest, TracedJSON_Map_ArrayShrinks)
{
    TempJsonFile file(R"({"items":["a","b","c"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.map (x: x) j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b,c"));
    }

    file.modify(R"({"items":["a","b"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }
}

TEST_F(TracedDataTest, TracedJSON_Filter_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.filter (x: true) j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,b,c"));
    }
}

TEST_F(TracedDataTest, TracedJSON_FoldlStrict_ArrayGrows)
{
    TempJsonFile file(R"({"nums":[1,2,3]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.foldl' (a: b: a + b) 0 j.nums)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(6));
    }

    file.modify(R"({"nums":[1,2,3,4]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(10));
    }
}

TEST_F(TracedDataTest, TracedJSON_Sort_ArrayGrows)
{
    TempJsonFile file(R"({"items":["b","a"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.sort (a: b: a < b) j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }

    file.modify(R"({"items":["b","a","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,b,c"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ConcatStringsSep_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,b,c"));
    }
}

TEST_F(TracedDataTest, TracedJSON_Any_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.any (x: x == "c") j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, TracedJSON_All_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.all (x: x != "c") j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, TracedJSON_Tail_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b","c"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.tail j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("b,c"));
    }

    file.modify(R"({"items":["a","b","c","d"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("b,c,d"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ConcatLists_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (j.items ++ ["extra"]))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b,extra"));
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,b,c,extra"));
    }
}

TEST_F(TracedDataTest, TracedJSON_Elem_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.elem "c" j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, TracedJSON_ConcatMap_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.concatMap (x: [x x]) j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,a,b,b"));
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,a,b,b,c,c"));
    }
}

TEST_F(TracedDataTest, TracedJSON_Partition_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","d"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); p = builtins.partition (x: x < "c") j.items; in builtins.concatStringsSep "," p.right)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        // partition (x: x < "c") ["a","d"] → right=["a"], wrong=["d"]
        EXPECT_THAT(v, IsStringEq("a"));
    }

    file.modify(R"({"items":["a","d","b"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        // partition (x: x < "c") ["a","d","b"] → right=["a","b"], wrong=["d"]
        EXPECT_THAT(v, IsStringEq("a,b"));
    }
}

TEST_F(TracedDataTest, TracedJSON_GroupBy_ArrayGrows)
{
    TempJsonFile file(R"({"tags":["a","a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); g = builtins.groupBy (x: x) j.tags; in builtins.toString (builtins.length g.a))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("2"));
    }

    file.modify(R"({"tags":["a","a","b","a"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("3"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ReplaceStrings_ArrayGrows)
{
    TempJsonFile file(R"({"from":["hello"],"to":["hi"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.replaceStrings j.from j.to "hello world")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hi world"));
    }

    file.modify(R"({"from":["hello","world"],"to":["hi","earth"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("hi earth"));
    }
}

TEST_F(TracedDataTest, TracedTOML_Map_ArrayGrows)
{
    TempTomlFile file("[[package]]\nname = \"foo\"\n\n[[package]]\nname = \"bar\"\n");
    auto expr = R"(let t = builtins.fromTOML (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.map (x: x.name) t.package))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("foo,bar"));
    }

    file.modify("[[package]]\nname = \"foo\"\n\n[[package]]\nname = \"bar\"\n\n[[package]]\nname = \"baz\"\n");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("foo,bar,baz"));
    }
}

} // namespace nix::eval_trace
