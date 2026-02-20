#include "helpers.hh"

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

// ── Category C — #keys from attrValues ──────────────────────────────

TEST_F(TracedDataTest, TracedJSON_AttrValues_KeyAdded)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.attrValues j))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x,y"));
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("x,y,z"));
    }
}

// ── Category E — #type container type change ────────────────────────

TEST_F(TracedDataTest, TracedJSON_TypeChange_ArrayToObject)
{
    TempJsonFile file(R"({"data":[1,2,3],"name":"foo"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.typeOf j.data + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }

    file.modify(R"({"data":{"a":1},"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("set-foo"));
    }
}

TEST_F(TracedDataTest, TracedJSON_TypeChange_ObjectToArray)
{
    TempJsonFile file(R"({"data":{"a":1},"name":"foo"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.typeOf j.data + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("set-foo"));
    }

    file.modify(R"({"data":[1,2,3],"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }
}

// ── Category A — Positional access (regression) ─────────────────────

TEST_F(TracedDataTest, TracedJSON_Head_Append_CacheHit)
{
    TempJsonFile file(R"({"items":["stable","other"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.head j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    file.modify(R"({"items":["stable","other","new"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

TEST_F(TracedDataTest, TracedJSON_Head_Prepend_CacheMiss)
{
    TempJsonFile file(R"({"items":["stable","other"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.head j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    file.modify(R"({"items":["new","stable","other"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("new"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ElemAt_Append_CacheHit)
{
    TempJsonFile file(R"({"items":["a","stable","c"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.elemAt j.items 1)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    file.modify(R"({"items":["a","stable","c","new"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

// ── Category D — Attrset output, name-based access (negative) ───────

TEST_F(TracedDataTest, TracedJSON_MapAttrs_KeyAdded_CacheHit)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"((builtins.mapAttrs (n: v: v + "!") (builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"())).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x!"));
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("x!"));
    }
}

TEST_F(TracedDataTest, TracedJSON_Update_KeyAdded_CacheMiss)
{
    // The // operator now records #keys for both operands. When a key is added
    // to the traced JSON, the #keys dep fails even though .a is unchanged.
    // This is the expected precision trade-off for the soundness fix.
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (j // { extra = "e"; }).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x"));
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys dep causes re-eval
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_IntersectAttrs_KeyAdded_CacheMiss)
{
    // intersectAttrs now records #keys for both operands. When a key is added
    // to the traced JSON, the #keys dep fails even though .a is unchanged.
    // This is the expected precision trade-off for the soundness fix.
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.intersectAttrs { a = true; } j).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x"));
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys dep causes re-eval
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_RemoveAttrs_KeyAdded_CacheHit)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.removeAttrs j ["unused"]).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x"));
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ListToAttrs_ElementAppended_CacheMiss)
{
    // listToAttrs now records #len for the input list. When an element is
    // appended, the #len dep fails even though .a is unchanged.
    // This is the expected precision trade-off for the soundness fix.
    TempJsonFile file(R"([{"name":"a","value":"x"},{"name":"b","value":"y"}])");
    auto expr = R"((builtins.listToAttrs (builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"())).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x"));
    }

    file.modify(R"([{"name":"a","value":"x"},{"name":"b","value":"y"},{"name":"c","value":"z"}])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len dep causes re-eval
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_NestedFieldAccess_SiblingAdded_CacheHit)
{
    TempJsonFile file(R"({"nodes":{"root":{"inputs":{"nixpkgs":"abc"}}}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in j.nodes.root.inputs.nixpkgs)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("abc"));
    }

    file.modify(R"({"nodes":{"root":{"inputs":{"nixpkgs":"abc","home-manager":"def"}}}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("abc"));
    }
}

// ── Scalar dep correctness (regression) ─────────────────────────────

TEST_F(TracedDataTest, TracedJSON_Map_SameLength_ValueChange_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.map (x: x) j.items))";

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

// ── #type persistence (negative — type unchanged) ───────────────────

TEST_F(TracedDataTest, TracedJSON_TypeUnchanged_CacheHit)
{
    // typeOf j.data records a #type dep on data (hash of "array").
    // j.name records a scalar dep. No #len dep is recorded because
    // no iterating builtin is called on data.
    TempJsonFile file(R"({"data":[1,2],"name":"foo"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.typeOf j.data + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }

    // Array grows but is still an array — #type dep passes. name unchanged.
    file.modify(R"({"data":[1,2,3],"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }
}

// ── Category F — toJSON serialization ───────────────────────────────

TEST_F(TracedDataTest, TracedJSON_ToJSON_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(builtins.toJSON (builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        // toJSON produces a string representation of the parsed JSON
        EXPECT_TRUE(v.type() == nString);
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

TEST_F(TracedDataTest, TracedJSON_ToJSON_KeyAdded)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(builtins.toJSON (builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nString);
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, TracedJSON_ToJSON_OutPath_CacheHit)
{
    // toJSON { outPath = j.name; extra = j; } short-circuits to just "\"foo\""
    // because outPath is the only thing serialized for sets with outPath.
    TempJsonFile file(R"({"name":"foo","other":"bar"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.toJSON { outPath = j.name; extra = j; })";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("\"foo\""));
    }

    file.modify(R"({"name":"foo","other":"baz"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("\"foo\""));
    }
}

// ── Category G — ? operator and or expression ───────────────────────

TEST_F(TracedDataTest, TracedJSON_HasOp_KeyAdded_CacheMiss)
{
    TempJsonFile file(R"({"a":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (if j ? b then j.b else "default") + "-" + j.a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("default-x"));
    }

    file.modify(R"({"a":"x","b":"y"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_HasOp_KeyRemoved_CacheMiss)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (if j ? b then j.b else "default") + "-" + j.a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }

    file.modify(R"({"a":"x"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("default-x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_HasOp_KeyUnchanged_CacheHit)
{
    // .b scalar dep fails (y→z), so re-eval happens due to scalar dep change.
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (if j ? a then j.a else "default") + "-" + j.b)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x-y"));
    }

    file.modify(R"({"a":"x","b":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // cache miss from scalar dep (.b changed y→z)
        EXPECT_THAT(v, IsStringEq("x-z"));
    }
}

TEST_F(TracedDataTest, TracedJSON_SelectOrDefault_KeyAdded_CacheMiss)
{
    TempJsonFile file(R"({"a":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (j.b or "default") + "-" + j.a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("default-x"));
    }

    file.modify(R"({"a":"x","b":"y"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_SelectOrDefault_KeyPresent_CacheHit)
{
    // or expression takes value path (j.b exists). .b and .a pass. .c not accessed.
    TempJsonFile file(R"({"a":"x","b":"y","c":"z"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (j.b or "default") + "-" + j.a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }

    file.modify(R"({"a":"x","b":"y","c":"w"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }
}

// ── Tricky area tests — additional coverage ─────────────────────────

TEST_F(TracedDataTest, TracedJSON_CoerceToString_ListGrows)
{
    // toString on traced list should record #len via coerceToString.
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in toString j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a b"));
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len dep fails (2→3)
        EXPECT_THAT(v, IsStringEq("a b c"));
    }
}

TEST_F(TracedDataTest, TracedJSON_IsAttrs_ObjectToArray)
{
    // isAttrs records #type dep. Type change object→array invalidates.
    TempJsonFile file(R"({"data":{"a":1},"name":"foo"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (if builtins.isAttrs j.data then "set" else "other") + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("set-foo"));
    }

    file.modify(R"({"data":[1,2,3],"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #type dep fails (object→array)
        EXPECT_THAT(v, IsStringEq("other-foo"));
    }
}

TEST_F(TracedDataTest, TracedJSON_IsList_ArrayToObject)
{
    // isList records #type dep. Type change array→object invalidates.
    TempJsonFile file(R"({"data":[1,2],"name":"foo"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (if builtins.isList j.data then "list" else "other") + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }

    file.modify(R"({"data":{"a":1},"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #type dep fails (array→object)
        EXPECT_THAT(v, IsStringEq("other-foo"));
    }
}

TEST_F(TracedDataTest, TracedJSON_CatAttrs_ListGrows)
{
    // catAttrs iterates the input list — should record #len.
    TempJsonFile file(R"({"items":[{"name":"a"},{"name":"b"}]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.catAttrs "name" j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }

    file.modify(R"({"items":[{"name":"a"},{"name":"b"},{"name":"c"}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len dep fails (2→3)
        EXPECT_THAT(v, IsStringEq("a,b,c"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ZipAttrsWith_ListGrows)
{
    // zipAttrsWith iterates the input list — should record #len.
    // zipAttrsWith returns attrset where each value is f(name, [vals...]).
    TempJsonFile file(R"({"sets":[{"a":"x"},{"a":"y"}]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.zipAttrsWith (name: vals: builtins.concatStringsSep "+" vals) j.sets).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x+y"));
    }

    file.modify(R"({"sets":[{"a":"x"},{"a":"y"},{"a":"z"}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len dep fails (2→3)
        EXPECT_THAT(v, IsStringEq("x+y+z"));
    }
}

TEST_F(TracedDataTest, TracedJSON_RemoveAttrs_NameListGrows)
{
    // removeAttrs name list records #len. Adding a name removes more keys.
    TempJsonFile file(R"({"names":["b"],"data":{"a":"x","b":"y","c":"z"}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.attrValues (builtins.removeAttrs j.data j.names)))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x,z"));
    }

    file.modify(R"({"names":["b","c"],"data":{"a":"x","b":"y","c":"z"}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len dep on names list fails (1→2)
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_NestedTypeChange)
{
    // Type change at non-root level. typeOf j.data.inner changes type.
    TempJsonFile file(R"({"data":{"inner":[1,2]},"name":"foo"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.typeOf j.data.inner + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }

    file.modify(R"({"data":{"inner":{"a":1}},"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #type dep fails at data.inner (array→object)
        EXPECT_THAT(v, IsStringEq("set-foo"));
    }
}

TEST_F(TracedDataTest, TracedJSON_EmptyArray_ThenGrows_NoFalseHit)
{
    // Empty array can't be tracked (no stable pointer). Verify that
    // when array becomes non-empty, Content dep failure causes re-eval.
    TempJsonFile file(R"({"items":[],"name":"foo"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in toString (builtins.length j.items) + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("0-foo"));
    }

    file.modify(R"({"items":["a","b"],"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        // No SC deps → two-level override cannot apply → Content failure → re-eval
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("2-foo"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ToXML_ArrayGrows)
{
    // toXML iterates list elements — should record #len.
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(builtins.toXML (builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nString);
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len dep fails from printValueAsXML
    }
}

TEST_F(TracedDataTest, TracedJSON_ToXML_KeyAdded)
{
    // toXML iterates attrset keys — should record #keys.
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(builtins.toXML (builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nString);
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys dep fails from printValueAsXML
    }
}

} // namespace nix::eval_trace
