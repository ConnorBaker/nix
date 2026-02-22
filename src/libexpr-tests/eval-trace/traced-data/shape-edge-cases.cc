#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/traced-data.hh"
#include "nix/expr/json-to-value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Edge cases ────────────────────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_EmptyArrayLength)
{
    // length([]) shape dep (file changes to [1])
    TempJsonFile file(R"({"arr": []})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length j.arr)";

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

TEST_F(TracedDataTest, TracedJSON_EmptyObjectAttrNames)
{
    // attrNames({}) shape dep (file changes to {"a":1})
    TempJsonFile file(R"({"obj": {}})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()).obj)";

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

TEST_F(TracedDataTest, TracedJSON_KeyWithHash)
{
    // JSON key containing '#' is properly escaped, no ambiguity with shape suffix
    TempJsonFile file(R"({"a#b": "value", "c": "other"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in j.${"a#b"})";

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

TEST_F(TracedDataTest, TracedJSON_DeeplyNestedMixedKeys)
{
    // Deeply nested path through dotted key → array index → bracket key.
    // Tests escapeDataPathKey ↔ parseDataPath roundtrip for complex paths:
    // the dep key encodes "a.b".[0]."c[d]" and verification must parse
    // this exact path to navigate the JSON DOM correctly.
    TempJsonFile file(R"({"a.b": [{"c[d]": "deep-value"}], "x": "other"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.elemAt j.${"a.b"} 0).${"c[d]"})";

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

TEST_F(TracedDataTest, TracedJSON_KeyNamedHashSuffix)
{
    // Keys literally named "#len" and "#keys" must be quoted by
    // escapeDataPathKey (because '#' triggers quoting), preventing
    // computeCurrentHash from confusing them with shape dep suffixes.
    TempJsonFile file(R"({"#len": "hash-len-val", "#keys": "hash-keys-val", "other": "x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in j.${"#len"} + "-" + j.${"#keys"})";

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

TEST_F(TracedDataTest, TracedJSON_LengthAfterTypeChangeToObject)
{
    // Array changes to object — #len shape dep must fail because
    // computeCurrentHash checks is_array() and returns nullopt for objects.
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length j.items)";

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

TEST_F(TracedDataTest, TracedJSON_AttrNamesAfterTypeChangeToArray)
{
    // Object changes to array — #keys shape dep must fail because
    // computeCurrentHash checks is_object() and returns nullopt for arrays.
    TempJsonFile file(R"({"data": {"x": 1, "y": 2}})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()).data)";

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

TEST_F(TracedDataTest, TracedJSON_TwoFilesOnlyOneCovered)
{
    // Two files read in same trace: f1 parsed with fromJSON (SC deps),
    // f2 read raw (only Content dep). When f2 changes, the override must
    // NOT apply because f2's Content failure is not covered by any SC dep.
    // Tests the structuralCoveredFiles check in verifyTrace.
    TempJsonFile f1(R"({"name": "hello"})");
    TempJsonFile f2("raw-content-here");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + f1.path.string()
        + R"(); raw = builtins.readFile )" + f2.path.string()
        + R"(; in j.name + "-" + raw)";

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

TEST_F(TracedDataTest, TracedJSON_NavigationFailureMidPath)
{
    // Intermediate key removed — navigateJson fails at middle segment,
    // not at leaf. Tests that mid-path navigation failure (as opposed to
    // leaf-key removal tested by TracedJSON_RemoveUsedKey) correctly
    // invalidates the StructuredContent dep.
    TempJsonFile file(R"({"outer": {"inner": {"deep": "value"}}, "x": "padding"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in j.outer.inner.deep)";

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
