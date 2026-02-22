#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/traced-data.hh"
#include "nix/expr/json-to-value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Array shrinkage edge cases ────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_ArrayShrinksLengthFails)
{
    // Array shrinks from 3 to 1 — #len shape dep must fail.
    // Tests the shrinkage direction (existing tests only grow arrays).
    TempJsonFile file(R"({"arr": ["a", "b", "c"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length j.arr)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"arr": ["a"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(TracedDataTest, TracedJSON_ArrayBecomesEmptyLengthFails)
{
    // Non-empty array becomes empty — #len shape dep must fail.
    // During recording, provenance was registered with first element as key.
    // During verification, computeCurrentHash navigates to the (now empty)
    // array and computes hash("0") != recorded hash("2").
    TempJsonFile file(R"({"arr": ["x", "y"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length j.arr)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"arr": []})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(0));
    }
}

// ── Shape dep re-record after invalidation ────────────────────────────

TEST_F(TracedDataTest, TracedJSON_ShapeDepRerecordAfterInvalidation)
{
    // Three-step test: fresh → shape invalidation → re-record → verify.
    // Ensures shape deps are correctly re-recorded after a trace is
    // invalidated by a shape change, and the new trace works correctly
    // on subsequent verification.
    TempJsonFile file(R"({"arr": ["alpha", "beta"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (toString (builtins.length j.arr)) + "-" + (builtins.elemAt j.arr 0))";

    // Step 1: Fresh evaluation
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("2-alpha"));
    }

    // Step 2: Shape change (array grows) → invalidation + re-record
    file.modify(R"({"arr": ["alpha", "beta", "gamma"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len fails (2→3) → re-eval
        EXPECT_THAT(v, IsStringEq("3-alpha"));
    }

    // Step 3: Value change (length same, elem 1 changed) → new trace survives
    file.modify(R"({"arr": ["alpha", "CHANGED!", "gamma"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // re-recorded #len=3 passes, .[0] passes → override
        EXPECT_THAT(v, IsStringEq("3-alpha"));
    }
}

// ── Nested container provenance ───────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_HasAttrOnNestedObject)
{
    // hasAttr on a nested traced object. Tests that container provenance
    // survives the Value copy during attribute selection (ExprSelect::eval
    // copies Values: v = *attr->value), so the Bindings* key remains stable.
    TempJsonFile file(R"({"inner": {"x": 1, "y": 2}})");
    auto expr = R"(builtins.hasAttr "x" (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()).inner)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    // Remove "x" from inner, add "z" → #keys changes → must re-evaluate
    file.modify(R"({"inner": {"y": 2, "z": 33}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys fails
        EXPECT_THAT(v, IsFalse());
    }

    // Values change but keys unchanged → #keys passes (nested provenance works)
    file.modify(R"({"inner": {"y": 99, "z": 999}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // #keys passes → override
        EXPECT_THAT(v, IsFalse());
    }
}

// ── Multiple containers from same file ────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_MultipleContainersShapeDeps)
{
    // Two arrays from the same file, both with #len shape deps.
    // One array grows, the other stays the same. The failing #len dep
    // must prevent the override even though the other #len passes.
    TempJsonFile file(R"({"arr1": [1, 2], "arr2": [3, 4, 5]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (toString (builtins.length j.arr1)) + "-" + (toString (builtins.length j.arr2)))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("2-3"));
    }

    // Only arr2 grows → its #len fails, overall override must NOT apply
    file.modify(R"({"arr1": [1, 2], "arr2": [3, 4, 5, 6]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (one #len fails)
        EXPECT_THAT(v, IsStringEq("2-4"));
    }

    // Both lengths unchanged, values change → both #len pass → override
    file.modify(R"({"arr1": [9, 8], "arr2": [7, 6, 5, 4]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // both #len pass → override
        EXPECT_THAT(v, IsStringEq("2-4"));
    }
}

// ── TOML type change ──────────────────────────────────────────────────

TEST_F(TracedDataTest, TracedTOML_LengthAfterTypeChangeToTable)
{
    // TOML array changes to table — #len shape dep must fail.
    // Tests navigateToml's is_array() type guard in computeCurrentHash.
    TempTomlFile file("items = [\"a\", \"b\", \"c\"]\n");
    auto expr = R"(let t = builtins.fromTOML (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length t.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    // Change items from array to table
    file.modify("[items]\na = 1\nb = 2\nc = 3\n");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Re-eval: length on attrset → throws
        EXPECT_THROW(forceRoot(*cache), Error);
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// ReadFileProvenance map tests (multi-file, reuse, forced-before-fromJSON)
// ═══════════════════════════════════════════════════════════════════════

// ── Two files read before fromJSON ────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_TwoFilesReadBeforeFromJSON)
{
    // Read two files, then fromJSON both. Both should get traced data
    // (verifies no single-slot overwrite).
    TempJsonFile f1(R"({"x": "hello"})");
    TempJsonFile f2(R"({"y": "world"})");
    auto expr = R"(let a = builtins.readFile )" + f1.path.string()
        + R"(; b = builtins.readFile )" + f2.path.string()
        + R"(; in (builtins.fromJSON a).x + "-" + (builtins.fromJSON b).y)";

    // Fresh evaluation
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello-world"));
    }

    // Verify trace served from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hello-world"));
    }

    // Modify f1 value of x → first trace invalidates
    f1.modify(R"({"x": "CHANGED!!"})");
    invalidateFileCache(f1.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED!!-world"));
    }

    // Modify f2 value of y → second trace invalidates
    f2.modify(R"({"y": "CHANGED!!"})");
    invalidateFileCache(f2.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED!!-CHANGED!!"));
    }
}

// ── Same readFile result fed to two fromJSON calls ────────────────────

TEST_F(TracedDataTest, TracedJSON_SameReadFileTwoFromJSON)
{
    // Same readFile result used by two fromJSON calls. Both should get
    // traced data (verifies non-consuming lookup).
    TempJsonFile file(R"({"x": "alpha", "y": "beta"})");
    auto expr = R"(let s = builtins.readFile )" + file.path.string()
        + R"(; in (builtins.fromJSON s).x + "-" + (builtins.fromJSON s).y)";

    // Fresh evaluation
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("alpha-beta"));
    }

    // Verify trace served from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("alpha-beta"));
    }

    // Change x value → trace invalid (StructuredContent dep on x fails)
    file.modify(R"({"x": "CHANGED!!", "y": "beta"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED!!-beta"));
    }

    // Change only y (unaccessed by x dep) — but y is also accessed,
    // so StructuredContent dep on y fails → trace invalid
    file.modify(R"({"x": "CHANGED!!", "y": "ALSO-NEW!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED!!-ALSO-NEW!!"));
    }

    // Change only unused key (add "z") → trace valid (two-level override)
    file.modify(R"({"x": "CHANGED!!", "y": "ALSO-NEW!!", "z": "extra!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("CHANGED!!-ALSO-NEW!!"));
    }
}

// ── Forced before fromJSON (builtins.seq) ─────────────────────────────

TEST_F(TracedDataTest, TracedJSON_ForcedBeforeFromJSON)
{
    // Force readFile result (via builtins.seq) before fromJSON. Provenance
    // should still be available because the map is non-consuming.
    // seq forces s but returns s (same string), so depHash(s) matches.
    TempJsonFile file(R"({"x": "hello", "extra": "padding"})");
    auto expr = R"(let s = builtins.readFile )" + file.path.string()
        + R"(; forced = builtins.seq s s; in (builtins.fromJSON forced).x)";

    // Fresh evaluation
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Verify trace served from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Change unused key → two-level override applies
    file.modify(R"({"x": "hello", "extra": "CHANGED-padding!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Change used key → trace invalid
    file.modify(R"({"x": "CHANGED!!", "extra": "CHANGED-padding!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED!!"));
    }
}

} // namespace nix::eval_trace
