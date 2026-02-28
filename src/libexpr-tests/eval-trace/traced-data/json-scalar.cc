#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/data/traced-data.hh"
#include "nix/expr/json-to-value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── JSON: Scalar access records StructuredContent dep ────────────────

TEST_F(TracedDataTest, TracedJSON_ScalarAccess)
{
    TempJsonFile file(R"({"name": "hello", "version": "1.0"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.name)";

    // Fresh evaluation: access .name → should record StructuredContent dep
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Verification: file unchanged → serve from trace
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hello"));
    }
}

// ── JSON: Nested access records dep at correct path ──────────────────

TEST_F(TracedDataTest, TracedJSON_NestedAccess)
{
    TempJsonFile file(R"({"a": {"b": {"c": 42}}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.a.b.c)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(42));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsIntEq(42));
    }
}

// ── JSON: Unused key change → trace valid (two-level override) ───────

TEST_F(TracedDataTest, TracedJSON_UnusedKeyChange)
{
    TempJsonFile file(R"({"used": "stable", "unused": "original"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.used)";

    // Fresh evaluation
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    // Modify unused key (different size to ensure stat change)
    file.modify(R"({"used": "stable", "unused": "changed-value!!"})");
    invalidateFileCache(file.path);

    // Verification: Content dep fails (file changed), but StructuredContent
    // dep on "used" still passes → two-level override → trace valid
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

// ── JSON: Used key change → trace invalid ────────────────────────────

TEST_F(TracedDataTest, TracedJSON_UsedKeyChange)
{
    TempJsonFile file(R"({"name": "hello", "extra": "x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Modify the accessed key
    file.modify(R"({"name": "world!!", "extra": "x"})");
    invalidateFileCache(file.path);

    // Verification: StructuredContent dep on "name" fails → trace invalid
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("world!!"));
    }
}

// ── JSON: Add new key → trace valid ─────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_AddKey)
{
    TempJsonFile file(R"({"used": "stable"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.used)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    // Add a new key (different file size)
    file.modify(R"({"used": "stable", "newkey": "newvalue!!"})");
    invalidateFileCache(file.path);

    // Content dep fails but StructuredContent dep on "used" passes
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

// ── JSON: Remove unused key → trace valid ────────────────────────────

TEST_F(TracedDataTest, TracedJSON_RemoveUnusedKey)
{
    TempJsonFile file(R"({"used": "stable", "unused": "original-val"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.used)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    // Remove unused key
    file.modify(R"({"used": "stable"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

// ── JSON: Remove used key → trace invalid (navigation fails) ─────────

TEST_F(TracedDataTest, TracedJSON_RemoveUsedKey)
{
    TempJsonFile file(R"({"name": "hello", "other": "x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Remove the accessed key (different size)
    file.modify(R"({"other": "x-remaining!!"})");
    invalidateFileCache(file.path);

    // Verification: StructuredContent dep navigation fails → trace invalid.
    // The loader re-evaluates, but the expression accesses .name which was
    // removed, so evaluation throws. We only care that the trace was invalidated
    // (loaderCalls == 1).
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        EXPECT_THROW(forceRoot(*cache), Error);
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── JSON: Array element access ───────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_ArrayAccess)
{
    TempJsonFile file(R"({"items": ["alpha", "beta", "gamma"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in builtins.elemAt j.items 1)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("beta"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("beta"));
    }
}

// ── JSON: Array element change (accessed vs unaccessed) ──────────────

TEST_F(TracedDataTest, TracedJSON_ArrayElementChange)
{
    TempJsonFile file(R"({"items": ["alpha", "beta", "gamma"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in builtins.elemAt j.items 1)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("beta"));
    }

    // Change unaccessed element (index 0) — trace should still be valid
    file.modify(R"({"items": ["CHANGED!!", "beta", "gamma"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("beta"));
    }

    // Now change the accessed element (index 1)
    file.modify(R"({"items": ["CHANGED!!", "BETA-NEW!!", "gamma"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("BETA-NEW!!"));
    }
}

} // namespace nix::eval_trace
