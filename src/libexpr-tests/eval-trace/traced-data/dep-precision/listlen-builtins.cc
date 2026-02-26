#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepPrecisionListLenBuiltinsTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// #len from iterating builtins (from builtins-len.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenBuiltinsTest, Map_ArrayGrows_CacheMiss)
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

TEST_F(DepPrecisionListLenBuiltinsTest, Map_ArrayShrinks_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b","c"]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.concatStringsSep "," (builtins.map (x: x) j.items))",
        fj(file.path));

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

TEST_F(DepPrecisionListLenBuiltinsTest, Filter_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.concatStringsSep "," (builtins.filter (x: true) j.items))",
        fj(file.path));

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

TEST_F(DepPrecisionListLenBuiltinsTest, FoldlStrict_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"nums":[1,2,3]})");
    auto expr = std::format(
        "let j = {}; in builtins.foldl' (a: b: a + b) 0 j.nums", fj(file.path));

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

TEST_F(DepPrecisionListLenBuiltinsTest, Sort_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":["b","a"]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.concatStringsSep "," (builtins.sort (a: b: a < b) j.items))",
        fj(file.path));

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

TEST_F(DepPrecisionListLenBuiltinsTest, ConcatStringsSep_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.concatStringsSep "," j.items)", fj(file.path));

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

TEST_F(DepPrecisionListLenBuiltinsTest, Any_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.any (x: x == "c") j.items)", fj(file.path));

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

TEST_F(DepPrecisionListLenBuiltinsTest, All_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.all (x: x != "c") j.items)", fj(file.path));

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

TEST_F(DepPrecisionListLenBuiltinsTest, Tail_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b","c"]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.concatStringsSep "," (builtins.tail j.items))",
        fj(file.path));

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

TEST_F(DepPrecisionListLenBuiltinsTest, ConcatLists_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.concatStringsSep "," (j.items ++ ["extra"]))",
        fj(file.path));

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

TEST_F(DepPrecisionListLenBuiltinsTest, Elem_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.elem "c" j.items)", fj(file.path));

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

TEST_F(DepPrecisionListLenBuiltinsTest, ConcatMap_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.concatStringsSep "," (builtins.concatMap (x: [x x]) j.items))",
        fj(file.path));

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

TEST_F(DepPrecisionListLenBuiltinsTest, Partition_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","d"]})");
    auto expr = std::format(
        R"(let j = {}; p = builtins.partition (x: x < "c") j.items; in builtins.concatStringsSep "," p.right)",
        fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a"));
    }

    file.modify(R"({"items":["a","d","b"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }
}

TEST_F(DepPrecisionListLenBuiltinsTest, GroupBy_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"tags":["a","a","b"]})");
    auto expr = std::format(
        "let j = {}; g = builtins.groupBy (x: x) j.tags; in builtins.toString (builtins.length g.a)",
        fj(file.path));

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

TEST_F(DepPrecisionListLenBuiltinsTest, ReplaceStrings_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"from":["hello"],"to":["hi"]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.replaceStrings j.from j.to "hello world")",
        fj(file.path));

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

TEST_F(DepPrecisionListLenBuiltinsTest, TOML_Map_ArrayGrows_CacheMiss)
{
    TempTomlFile file("[[package]]\nname = \"foo\"\n\n[[package]]\nname = \"bar\"\n");
    auto expr = std::format(
        R"(let t = {}; in builtins.concatStringsSep "," (builtins.map (x: x.name) t.package))",
        ft(file.path));

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

// ═══════════════════════════════════════════════════════════════════════
// Additional builtins (from builtins-misc.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionListLenBuiltinsTest, CoerceToString_ListGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = std::format("let j = {}; in toString j.items", fj(file.path));

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
        EXPECT_EQ(loaderCalls, 1); // #len dep fails
        EXPECT_THAT(v, IsStringEq("a b c"));
    }
}

TEST_F(DepPrecisionListLenBuiltinsTest, CatAttrs_ListGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":[{"name":"a"},{"name":"b"}]})");
    auto expr = std::format(
        R"(let j = {}; in builtins.concatStringsSep "," (builtins.catAttrs "name" j.items))",
        fj(file.path));

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
        EXPECT_EQ(loaderCalls, 1); // #len dep fails
        EXPECT_THAT(v, IsStringEq("a,b,c"));
    }
}

TEST_F(DepPrecisionListLenBuiltinsTest, ZipAttrsWith_ListGrows_CacheMiss)
{
    TempJsonFile file(R"({"sets":[{"a":"x"},{"a":"y"}]})");
    auto expr = std::format(
        R"(let j = {}; in (builtins.zipAttrsWith (name: vals: builtins.concatStringsSep "+" vals) j.sets).a)",
        fj(file.path));

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
        EXPECT_EQ(loaderCalls, 1); // #len dep fails
        EXPECT_THAT(v, IsStringEq("x+y+z"));
    }
}

TEST_F(DepPrecisionListLenBuiltinsTest, RemoveAttrs_NameListGrows_CacheMiss)
{
    TempJsonFile file(R"({"names":["b"],"data":{"a":"x","b":"y","c":"z"}})");
    auto expr = std::format(
        R"(let j = {}; in builtins.concatStringsSep "," (builtins.attrValues (builtins.removeAttrs j.data j.names)))",
        fj(file.path));

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
        EXPECT_EQ(loaderCalls, 1); // #len dep on names list fails
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(DepPrecisionListLenBuiltinsTest, ToXML_ArrayGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = std::format("builtins.toXML ({})", fj(file.path));

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
        EXPECT_EQ(loaderCalls, 1); // #len dep fails
    }
}

TEST_F(DepPrecisionListLenBuiltinsTest, ListToAttrs_ElementAppended_CacheMiss)
{
    TempJsonFile file(R"([{"name":"a","value":"x"},{"name":"b","value":"y"}])");
    auto expr = std::format("(builtins.listToAttrs ({})).a", fj(file.path));

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

TEST_F(DepPrecisionListLenBuiltinsTest, EmptyArray_ThenGrows_CacheMiss)
{
    TempJsonFile file(R"({"items":[],"name":"foo"})");
    auto expr = std::format(
        R"(let j = {}; in toString (builtins.length j.items) + "-" + j.name)",
        fj(file.path));

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
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("2-foo"));
    }
}

} // namespace nix::eval_trace
