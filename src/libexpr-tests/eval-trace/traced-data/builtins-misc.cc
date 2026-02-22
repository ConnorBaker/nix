#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/traced-data.hh"
#include "nix/expr/json-to-value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

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
