#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepPrecisionEdgeCasesTest : public DepPrecisionTest {};

// ── Edge cases ────────────────────────────────────────────────────────

TEST_F(DepPrecisionEdgeCasesTest, TracedJSON_EmptyArrayLength)
{
    // length([]) shape dep (file changes to [1])
    TempJsonFile file(R"({"arr": []})");
    auto expr = std::format("let j = {}; in builtins.length j.arr", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(0));
    }

    file.modify(R"({"arr": [1]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#len fails)
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(DepPrecisionEdgeCasesTest, TracedJSON_EmptyObjectAttrNames)
{
    // attrNames({}) shape dep (file changes to {"a":1})
    TempJsonFile file(R"({"obj": {}})");
    auto expr = std::format("builtins.attrNames ({}).obj", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify(R"({"obj": {"a": 1}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#keys fails)
    }
}

TEST_F(DepPrecisionEdgeCasesTest, TracedJSON_KeyWithHash)
{
    // JSON key containing '#' is properly escaped, no ambiguity with shape suffix
    TempJsonFile file(R"({"a#b": "value", "c": "other"})");
    auto expr = std::format(R"(let j = {}; in j.${{"{}"}})", fj(file.path), "a#b");

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("value"));
    }

    // Verify trace served from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("value"));
    }

    // Change the value → trace invalid
    file.modify(R"({"a#b": "CHANGED!", "c": "other"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED!"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Robustness and edge case tests
// ═══════════════════════════════════════════════════════════════════════

// ── Data path escaping roundtrip ──────────────────────────────────────

TEST_F(DepPrecisionEdgeCasesTest, TracedJSON_DeeplyNestedMixedKeys)
{
    // Deeply nested path through dotted key → array index → bracket key.
    // Tests escapeDataPathKey ↔ parseDataPath roundtrip for complex paths:
    // the dep key encodes "a.b".[0]."c[d]" and verification must parse
    // this exact path to navigate the JSON DOM correctly.
    TempJsonFile file(R"({"a.b": [{"c[d]": "deep-value"}], "x": "other"})");
    auto expr = std::format(R"(let j = {}; in (builtins.elemAt j.${{"{}"}} 0).${{"{}"}})", fj(file.path), "a.b", "c[d]");

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("deep-value"));
    }

    // Verify served from cache (roundtrip works)
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("deep-value"));
    }

    // Change unaccessed key → override should apply
    file.modify(R"({"a.b": [{"c[d]": "deep-value"}], "x": "CHANGED!!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // override applies
        EXPECT_THAT(v, IsStringEq("deep-value"));
    }

    // Change accessed value → must re-evaluate
    file.modify(R"({"a.b": [{"c[d]": "NEW-deep-val!!"}], "x": "CHANGED!!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("NEW-deep-val!!"));
    }
}

// ── Shape suffix disambiguation ───────────────────────────────────────

TEST_F(DepPrecisionEdgeCasesTest, TracedJSON_KeyNamedHashSuffix)
{
    // Keys literally named "#len" and "#keys" must be quoted by
    // escapeDataPathKey (because '#' triggers quoting), preventing
    // computeCurrentHash from confusing them with shape dep suffixes.
    TempJsonFile file(R"({"#len": "hash-len-val", "#keys": "hash-keys-val", "other": "x"})");
    auto expr = std::format(R"(let j = {}; in j.${{"{}"}} + "-" + j.${{"{}"}})", fj(file.path), "#len", "#keys");

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hash-len-val-hash-keys-val"));
    }

    // Change unaccessed key → override applies (no shape suffix confusion)
    file.modify(R"({"#len": "hash-len-val", "#keys": "hash-keys-val", "other": "CHANGED!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hash-len-val-hash-keys-val"));
    }

    // Change accessed key "#len" → must re-evaluate
    file.modify(R"({"#len": "CHANGED-val!!", "#keys": "hash-keys-val", "other": "CHANGED!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED-val!!-hash-keys-val"));
    }
}

// ── Container type changes ────────────────────────────────────────────

TEST_F(DepPrecisionEdgeCasesTest, TracedJSON_LengthAfterTypeChangeToObject)
{
    // Array changes to object — #len shape dep must fail because
    // computeCurrentHash checks is_array() and returns nullopt for objects.
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = std::format("let j = {}; in builtins.length j.items", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    // Change items from array to object (type change)
    file.modify(R"({"items": {"a": 1, "b": 2, "c": 3}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Re-eval calls length on attrset → throws
        EXPECT_THROW(forceRoot(*cache), Error);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(DepPrecisionEdgeCasesTest, TracedJSON_AttrNamesAfterTypeChangeToArray)
{
    // Object changes to array — #keys shape dep must fail because
    // computeCurrentHash checks is_object() and returns nullopt for arrays.
    TempJsonFile file(R"({"data": {"x": 1, "y": 2}})");
    auto expr = std::format("builtins.attrNames ({}).data", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    // Change data from object to array (type change)
    file.modify(R"({"data": [1, 2, 3, 4, 5]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Re-eval calls attrNames on list → throws
        EXPECT_THROW(forceRoot(*cache), Error);
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Two-level override coverage gap ───────────────────────────────────

TEST_F(DepPrecisionEdgeCasesTest, TracedJSON_TwoFilesOnlyOneCovered)
{
    // Two files read in same trace: f1 parsed with fromJSON (SC deps),
    // f2 read raw (only Content dep). When f2 changes, the override must
    // NOT apply because f2's Content failure is not covered by any SC dep.
    // Tests the structuralCoveredFiles check in verifyTrace.
    TempJsonFile f1(R"({"name": "hello"})");
    TempTextFile f2("raw-content-here");
    auto expr = std::format("let j = {}; raw = builtins.readFile {}; in j.name + \"-\" + raw", fj(f1.path), f2.path.string());

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello-raw-content-here"));
    }

    // Modify f2 only (f1 unchanged) — override must NOT apply
    f2.modify("raw-CHANGED-content!!");
    invalidateFileCache(f2.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (f2 uncovered)
        EXPECT_THAT(v, IsStringEq("hello-raw-CHANGED-content!!"));
    }
}

TEST_F(DepPrecisionEdgeCasesTest, TracedJSON_NavigationFailureMidPath)
{
    // Intermediate key removed — navigateJson fails at middle segment,
    // not at leaf. Tests that mid-path navigation failure (as opposed to
    // leaf-key removal tested by TracedJSON_RemoveUsedKey) correctly
    // invalidates the StructuredContent dep.
    TempJsonFile file(R"({"outer": {"inner": {"deep": "value"}}, "x": "padding"})");
    auto expr = std::format("let j = {}; in j.outer.inner.deep", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("value"));
    }

    // Remove intermediate key "inner" (renamed)
    file.modify(R"({"outer": {"RENAMED": {"deep": "value"}}, "x": "padding!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Re-eval: j.outer.inner → missing key → throws
        EXPECT_THROW(forceRoot(*cache), Error);
        EXPECT_EQ(loaderCalls, 1);
    }
}

} // namespace nix::eval_trace
