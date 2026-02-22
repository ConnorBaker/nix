#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/traced-data.hh"
#include "nix/expr/json-to-value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── JSON: No provenance (literal string) → eager fallback ────────────

TEST_F(TracedDataTest, TracedJSON_NoProvenance)
{
    // fromJSON on a literal string (not from readFile) should use eager parsing
    auto expr = R"(builtins.fromJSON ''{"x": 42}'')";

    {
        auto cache = makeCache(expr);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        auto * xVal = v->attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xVal, nullptr);
        state.forceValue(*xVal->value, noPos);
        EXPECT_THAT(*xVal->value, IsIntEq(42));
    }

    // Verification: no file deps at all, just the trace
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── JSON: Direct readFile→fromJSON produces structural deps ──────────

TEST_F(TracedDataTest, TracedJSON_DirectReadFile)
{
    TempJsonFile file(R"({"x": 42})");
    // readFile result is directly used (no modification), so provenance matches.
    // This test verifies that direct readFile→fromJSON produces structural deps
    // and the trace is served on second access.
    auto expr = "builtins.fromJSON (builtins.readFile " + file.path.string() + ")";

    {
        auto cache = makeCache(expr);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        auto * xVal = v->attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xVal, nullptr);
        state.forceValue(*xVal->value, noPos);
        EXPECT_THAT(*xVal->value, IsIntEq(42));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── JSON: Modified string (hash mismatch) → eager fallback ───────────

TEST_F(TracedDataTest, TracedJSON_ModifiedString)
{
    TempJsonFile file(R"({"x": 42})");
    // readFile result is concatenated with extra text, so the content hash
    // won't match the ReadFileProvenance → eager fallback (no structural deps).
    // The trace should still work via the whole-file Content dep.
    auto expr = "builtins.fromJSON (builtins.readFile " + file.path.string() + ")";

    // Use an expression that modifies the string, breaking provenance
    auto modExpr = "builtins.fromJSON (''{\"y\": 99}'')";

    {
        auto cache = makeCache(modExpr);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        auto * yVal = v->attrs()->get(state.symbols.create("y"));
        ASSERT_NE(yVal, nullptr);
        state.forceValue(*yVal->value, noPos);
        EXPECT_THAT(*yVal->value, IsIntEq(99));
    }

    // Trace still works (served from cache — no file deps, just trace)
    {
        int loaderCalls = 0;
        auto cache = makeCache(modExpr, &loaderCalls);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── TOML: Scalar access records StructuredContent dep ────────────────

TEST_F(TracedDataTest, TracedTOML_ScalarAccess)
{
    TempTomlFile file("[section]\nname = \"hello\"\nversion = \"1.0\"\n");
    auto expr = R"(let t = builtins.fromTOML (builtins.readFile )" + file.path.string() + R"(); in t.section.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hello"));
    }
}

// ── TOML: Unused key change → trace valid (two-level override) ───────

TEST_F(TracedDataTest, TracedTOML_UnusedKeyChange)
{
    TempTomlFile file("[section]\nused = \"stable\"\nunused = \"original\"\n");
    auto expr = R"(let t = builtins.fromTOML (builtins.readFile )" + file.path.string() + R"(); in t.section.used)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    // Modify unused key (different size)
    file.modify("[section]\nused = \"stable\"\nunused = \"changed-value-longer!!\"\n");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

// ── TOML: Used key change → trace invalid ────────────────────────────

TEST_F(TracedDataTest, TracedTOML_UsedKeyChange)
{
    TempTomlFile file("[section]\nname = \"hello\"\nextra = \"x\"\n");
    auto expr = R"(let t = builtins.fromTOML (builtins.readFile )" + file.path.string() + R"(); in t.section.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Modify the accessed key (different size)
    file.modify("[section]\nname = \"world-changed!!\"\nextra = \"x\"\n");
    invalidateFileCache(file.path);

    // StructuredContent dep on section.name fails → trace invalid
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("world-changed!!"));
    }
}

// ── JSON: Multiple scalar accesses from same file ────────────────────

TEST_F(TracedDataTest, TracedJSON_MultipleAccess)
{
    TempJsonFile file(R"({"a": "alpha", "b": "beta", "c": "gamma"})");
    // Access two keys: a and b. c is not accessed.
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.a + "-" + j.b)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("alpha-beta"));
    }

    // Change unaccessed key c → trace still valid
    file.modify(R"({"a": "alpha", "b": "beta", "c": "CHANGED!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("alpha-beta"));
    }

    // Change accessed key b → trace invalid
    file.modify(R"({"a": "alpha", "b": "BETA-NEW!!", "c": "CHANGED!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("alpha-BETA-NEW!!"));
    }
}

// ── JSON: Root-level array ───────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_RootArray)
{
    TempJsonFile file(R"(["first", "second", "third"])");
    auto expr = R"(builtins.elemAt (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()) 1)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("second"));
    }

    // Verify trace is served from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("second"));
    }

    // Change unaccessed element → trace still valid
    file.modify(R"(["CHANGED!!", "second", "third"])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("second"));
    }
}

// ── JSON: Keys containing dots, brackets, quotes, backslashes ────────

TEST_F(TracedDataTest, TracedJSON_DottedKey)
{
    // JSON key "a.b" must be quoted in the data path to avoid being
    // misinterpreted as two separate keys "a" and "b".
    TempJsonFile file(R"({"a.b": "dotted", "a": {"b": "nested"}})");
    // Access the dotted key "a.b" (not a.b which is nested)
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j."a.b")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("dotted"));
    }

    // Verify trace is served from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("dotted"));
    }

    // Change the nested a.b (not the dotted key) → trace still valid
    file.modify(R"({"a.b": "dotted", "a": {"b": "CHANGED"}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("dotted"));
    }

    // Change the dotted key → trace invalid
    file.modify(R"({"a.b": "CHANGED", "a": {"b": "nested"}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED"));
    }
}

TEST_F(TracedDataTest, TracedJSON_BracketKey)
{
    // JSON key containing brackets
    TempJsonFile file(R"({"[0]": "bracket-key", "normal": "value"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j."[0]")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("bracket-key"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("bracket-key"));
    }
}

TEST_F(TracedDataTest, TracedJSON_QuoteAndBackslashKey)
{
    // JSON keys with quotes and backslashes (escaped in JSON source)
    TempJsonFile file(R"({"a\"b": "has-quote", "c\\d": "has-backslash"})");
    // In Nix, access with string interpolation to get literal key
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.${"a\"b"})";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("has-quote"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("has-quote"));
    }
}

} // namespace nix::eval_trace
