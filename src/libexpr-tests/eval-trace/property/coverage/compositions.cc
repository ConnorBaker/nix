// Multi-step compositional chain property tests — the REAL nixpkgs patterns.
//
// Each test exercises a chain of 2–3 builtins on traced data and verifies
// that the cache correctly invalidates on relevant changes (soundness) and
// correctly serves cache hits on irrelevant changes (precision).
//
// Test strategy: deterministic, hand-crafted expressions (no RapidCheck).
// All expressions follow the standard cold→warm-hit→mutate→warm-miss/hit
// pattern from helpers.hh.

#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// ── Fixture ──────────────────────────────────────────────────────────────────

class EvalTraceProperty_Compositions : public TraceCacheFixture {
public:
    EvalTraceProperty_Compositions() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-compositions-test");
    }
};

// ── Test a: MapAttrs_ThenAccess_Soundness ─────────────────────────────────
//
// Expression: (builtins.mapAttrs (k: v: v + 1) (builtins.fromJSON (builtins.readFile f))).x
//
// f = {"x": 10, "y": 20}.  Changing x → mapped value changes → must miss.
TEST_F(EvalTraceProperty_Compositions, MapAttrs_ThenAccess_Soundness)
{
    TempJsonFile f(R"({"x": 10, "y": 20})");
    auto nixCode =
        "(builtins.mapAttrs (k: v: v + 1) (builtins.fromJSON (builtins.readFile "
        + f.path.string() + "))).x";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change x — accessed key's value changes.
    f.modify(R"({"x": 99, "y": 20})");
    invalidateFileCache(f.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: x changed, mapAttrs result differs";
        EXPECT_EQ(v.type(), nInt);
    }
}

// ── Test b: MapAttrs_ThenAccess_Precision ────────────────────────────────
//
// Same expression, but change y (not accessed after map).  Should hit.
TEST_F(EvalTraceProperty_Compositions, MapAttrs_ThenAccess_Precision)
{
    TempJsonFile f(R"({"x": 10, "y": 20})");
    auto nixCode =
        "(builtins.mapAttrs (k: v: v + 1) (builtins.fromJSON (builtins.readFile "
        + f.path.string() + "))).x";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
    }

    // Warm eval — confirm baseline hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change y — not accessed; SC dep on x should remain valid.
    f.modify(R"({"x": 10, "y": 999})");
    invalidateFileCache(f.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0)
            << "cache hit expected: y changed but only x is accessed — SC dep on x still valid";
    }
}

// ── Test c: Filter_ThenLength_Soundness ──────────────────────────────────
//
// Expression: builtins.length (builtins.filter (x: x > 0)
//                              (builtins.fromJSON (builtins.readFile f)))
//
// f = [1,-2,3] → filter result = [1,3] → length = 2.
// Change to [1,2,3] → filter result = [1,2,3] → length = 3 → must miss.
TEST_F(EvalTraceProperty_Compositions, Filter_ThenLength_Soundness)
{
    TempJsonFile f(R"([1,-2,3])");
    auto nixCode =
        "builtins.length (builtins.filter (x: x > 0) (builtins.fromJSON (builtins.readFile "
        + f.path.string() + ")))";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 2);
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change [-2] to [2] so filter result grows: [1,2,3] → length 3.
    f.modify(R"([1,2,3])");
    invalidateFileCache(f.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: filter result length changed";
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 3);
    }
}

// ── Test d: Filter_ThenLength_CorrectlyInvalidates ───────────────────────
//
// Change [1,-2,3] to [4,-2,6] — the positive elements change in value but
// the filter result still has the same length (2).
// Per-element SC deps cause correct over-invalidation; this is a soundness test, not precision.
// filter evaluates the predicate on each element, recording an SC dep per value.
// Changing element values (even without changing the count of positives) correctly invalidates.
TEST_F(EvalTraceProperty_Compositions, Filter_ThenLength_CorrectlyInvalidates)
{
    TempJsonFile f(R"([1,-2,3])");
    auto nixCode =
        "builtins.length (builtins.filter (x: x > 0) (builtins.fromJSON (builtins.readFile "
        + f.path.string() + ")))";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 2);
    }

    // Warm eval — confirm baseline hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change values but keep the same number of positives (2 out of 3).
    f.modify(R"([4,-2,6])");
    invalidateFileCache(f.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        // filter records per-element SC deps; element value changes cause a cache miss
        // even though the number of positives (and thus the filter result length) is unchanged.
        EXPECT_EQ(calls, 1)
            << "cache miss expected: filter records per-element SC deps — "
               "element value changes correctly invalidate (over-invalidation)";
    }
}

// ── Test e: IntersectAttrs_ThenMapAttrs_ThenAccess ───────────────────────
//
// Expression:
//   (builtins.mapAttrs (k: v: v * 2)
//     (builtins.intersectAttrs (fromJSON fA) (fromJSON fB))).x
//
// 3-step chain: intersectAttrs keeps keys from fA that exist in fB,
// then mapAttrs doubles each value, then we access .x.
// fA = {"x": 1, "z": 3}, fB = {"x": 5, "y": 7} → intersection = {"x": 5}
// → mapped = {"x": 10} → result = 10.
// Change fB's x → must miss.
TEST_F(EvalTraceProperty_Compositions, IntersectAttrs_ThenMapAttrs_ThenAccess)
{
    TempJsonFile fA(R"({"x": 1, "z": 3})");
    TempJsonFile fB(R"({"x": 5, "y": 7})");
    auto nixCode =
        "(builtins.mapAttrs (k: v: v * 2) (builtins.intersectAttrs "
        "(builtins.fromJSON (builtins.readFile " + fA.path.string() + ")) "
        "(builtins.fromJSON (builtins.readFile " + fB.path.string() + ")))).x";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 10);
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change fB's x → intersection result changes.
    fB.modify(R"({"x": 99, "y": 7})");
    invalidateFileCache(fB.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: fB.x changed, intersect→map→access result differs";
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 198);
    }
}

// ── Test f: AttrNames_ThenLength ──────────────────────────────────────────
//
// Expression: builtins.length (builtins.attrNames (builtins.fromJSON (builtins.readFile f)))
//
// Counts the number of keys.  Change that adds a key → must miss.
TEST_F(EvalTraceProperty_Compositions, AttrNames_ThenLength)
{
    TempJsonFile f(R"({"a": 1, "b": 2})");
    auto nixCode =
        "builtins.length (builtins.attrNames (builtins.fromJSON (builtins.readFile "
        + f.path.string() + ")))";

    // Cold eval: 2 keys.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 2);
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Add a key → 3 keys now.
    f.modify(R"({"a": 1, "b": 2, "c": 3})");
    invalidateFileCache(f.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: key count increased";
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 3);
    }
}

// ── Test g: ReadFile_ThenReplaceStrings_ThenStringLength ─────────────────
//
// Expression:
//   builtins.stringLength (builtins.replaceStrings ["a"] ["bb"] (builtins.readFile f))
//
// Each 'a' in f is replaced by 'bb' (one char → two chars).  Changing f
// changes both the replacement result and its length → must miss.
TEST_F(EvalTraceProperty_Compositions, ReadFile_ThenReplaceStrings_ThenStringLength)
{
    TempTextFile f("cat");  // "cat" → replaceStrings → "cbbt": length 4
    auto nixCode =
        "builtins.stringLength (builtins.replaceStrings [\"a\"] [\"bb\"] "
        "(builtins.readFile " + f.path.string() + "))";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 4);  // "cat" → "cbbt"
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change f so the replacement result length differs.
    f.modify("aaa");  // "aaa" → "bbbbbb": length 6 (3 * 2)
    invalidateFileCache(f.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: file content changed, stringLength differs";
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 6);
    }
}

// ── Test h: ConcatStringsSep_FromMappedList ───────────────────────────────
//
// Expression:
//   builtins.concatStringsSep ":"
//     (builtins.map (x: builtins.toString x) (builtins.fromJSON (builtins.readFile f)))
//
// f = [1,2,3] → map toString → ["1","2","3"] → concatStringsSep ":" → "1:2:3"
// Change element → must miss.
TEST_F(EvalTraceProperty_Compositions, ConcatStringsSep_FromMappedList)
{
    TempJsonFile f(R"([1,2,3])");
    auto nixCode =
        "builtins.concatStringsSep \":\" "
        "(builtins.map (x: builtins.toString x) "
        "(builtins.fromJSON (builtins.readFile " + f.path.string() + ")))";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "1:2:3");
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change element 2 → result changes.
    f.modify(R"([1,99,3])");
    invalidateFileCache(f.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: array element changed, concatStringsSep result differs";
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "1:99:3");
    }
}

// ── Test i: FromJSON_ThenToJSON_RoundTrip ────────────────────────────────
//
// Expression: builtins.toJSON (builtins.fromJSON (builtins.readFile f))
//
// Identity round-trip: read JSON, parse, re-serialize.  Changing f → must miss.
TEST_F(EvalTraceProperty_Compositions, FromJSON_ThenToJSON_RoundTrip)
{
    TempJsonFile f(R"({"key": "value"})");
    auto nixCode =
        "builtins.toJSON (builtins.fromJSON (builtins.readFile "
        + f.path.string() + "))";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change the JSON content.
    f.modify(R"({"key": "changed"})");
    invalidateFileCache(f.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: JSON content changed, toJSON result differs";
        EXPECT_EQ(v.type(), nString);
    }
}

// ── Test j: RemoveAttrs_ThenAttrNames_ThenLength ─────────────────────────
//
// Expression:
//   builtins.length (builtins.attrNames
//     (builtins.removeAttrs (builtins.fromJSON (builtins.readFile f)) ["x"]))
//
// f = {"x": 1, "y": 2, "z": 3} → removeAttrs ["x"] → {"y": 2, "z": 3}
// → attrNames → ["y","z"] → length = 2.
// Adding a new key (not "x") → length changes → must miss.
TEST_F(EvalTraceProperty_Compositions, RemoveAttrs_ThenAttrNames_ThenLength)
{
    TempJsonFile f(R"({"x": 1, "y": 2, "z": 3})");
    auto nixCode =
        "builtins.length (builtins.attrNames (builtins.removeAttrs "
        "(builtins.fromJSON (builtins.readFile " + f.path.string() + ")) [\"x\"]))";

    // Cold eval: length = 2 (y and z remain after removing x).
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 2);
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Add key "w" → after removing "x", result has 3 keys now.
    f.modify(R"({"x": 1, "y": 2, "z": 3, "w": 4})");
    invalidateFileCache(f.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: new key 'w' added, length of remaining attrs changed";
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 3);
    }
}

} // namespace nix::eval_trace::proptest
