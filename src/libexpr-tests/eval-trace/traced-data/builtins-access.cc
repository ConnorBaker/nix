#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/traced-data.hh"
#include "nix/expr/json-to-value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

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

TEST_F(TracedDataTest, TracedJSON_Update_KeyAdded_CacheHit)
{
    // The // operator does NOT record #keys for its operands (precision fix).
    // Only the consumer (.a access) records #has:a, which still passes when
    // an unrelated key "c" is added. Result: cache hit.
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
        EXPECT_EQ(loaderCalls, 0); // #has:a passes — key "c" is unrelated
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

} // namespace nix::eval_trace
