#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/data/traced-data.hh"
#include "nix/expr/json-to-value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ═══════════════════════════════════════════════════════════════════════
// Provenance propagation tests
// ═══════════════════════════════════════════════════════════════════════
//
// These tests verify that container-reconstructing operations (mapAttrs,
// filter, sort, removeAttrs, etc.) propagate provenance from tracked
// inputs to new output containers, allowing shape deps to be recorded
// on derived containers.

// ── mapAttrs: #keys dep recorded on derived attrset ──────────────────
// This is the core "mapAttrs gap" scenario from the design doc.
// Without propagation, attrNames on mapAttrs output fails to record
// a #keys dep because the output Bindings* is not in the provenance map.

TEST_F(TracedDataTest, Propagation_MapAttrs_KeysTracked)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            mapped = builtins.mapAttrs (k: v: v + 1) data;
        in builtins.attrNames mapped
    )";

    // Fresh evaluation: attrNames on mapAttrs output
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    // File unchanged → serve from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Key added to JSON → must re-evaluate (different-size content triggers stat change)
    file.modify(R"({"a": 1, "b": 2, "c": 3333})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Must re-evaluate: key set changed
        EXPECT_EQ(v.listSize(), 3);
    }
}

// ── mapAttrs: value change in unused key doesn't invalidate ──────────
// When mapAttrs derives from tracked JSON, changing a value that the
// trace doesn't depend on should still allow two-level override.

TEST_F(TracedDataTest, Propagation_MapAttrs_UnusedValueChange)
{
    TempJsonFile file(R"({"used": 10, "other": 20})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            mapped = builtins.mapAttrs (k: v: v + 1) data;
        in mapped.used
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(11));
    }

    // Change "other" value (different size to trigger stat change)
    file.modify(R"({"used": 10, "other": 99999})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // Two-level override: only .used accessed
        EXPECT_THAT(v, IsIntEq(11));
    }
}

// ── removeAttrs: provenance propagated to subset ─────────────────────

TEST_F(TracedDataTest, Propagation_RemoveAttrs_KeysTracked)
{
    TempJsonFile file(R"({"a": 1, "b": 2, "c": 3})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            subset = builtins.removeAttrs data ["c"];
        in builtins.attrNames subset
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    // File unchanged → serve from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Key added → must re-evaluate (provenance propagation catches shape change)
    file.modify(R"({"a": 1, "b": 2, "c": 3, "d": 44444})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Must re-evaluate
    }
}

// ── intersectAttrs: provenance propagated from tracked input ─────────

TEST_F(TracedDataTest, Propagation_IntersectAttrs_KeysTracked)
{
    TempJsonFile file(R"({"a": 1, "b": 2, "c": 3})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            result = builtins.intersectAttrs { a = true; b = true; } data;
        in builtins.attrNames result
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── filter: provenance propagated to filtered list ───────────────────

TEST_F(TracedDataTest, Propagation_Filter_LenTracked)
{
    TempJsonFile file(R"({"items": [1, 2, 3, 4, 5]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            filtered = builtins.filter (x: x > 2) data.items;
        in builtins.length filtered
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── sort: provenance propagated (same length) ────────────────────────

TEST_F(TracedDataTest, Propagation_Sort_LenTracked)
{
    TempJsonFile file(R"({"items": [3, 1, 2]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            sorted = builtins.sort builtins.lessThan data.items;
        in builtins.length sorted
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Array grows → must re-evaluate (length changed)
    file.modify(R"({"items": [3, 1, 2, 4444]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(4));
    }
}

// ── ExprOpUpdate (//): provenance propagated from tracked input ──────

TEST_F(TracedDataTest, Propagation_OpUpdate_KeysTracked)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            merged = data // { c = 3; };
        in builtins.attrNames merged
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3);
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Key added to JSON → must re-evaluate
    file.modify(R"({"a": 1, "b": 2, "d": 44444})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 4); // a, b, c, d
    }
}

// ── partition: inner lists propagated ────────────────────────────────

TEST_F(TracedDataTest, Propagation_Partition_InnerListsTracked)
{
    TempJsonFile file(R"({"items": [1, 2, 3, 4, 5]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            parts = builtins.partition (x: x > 2) data.items;
        in builtins.length parts.right
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── groupBy: inner group lists propagated ────────────────────────────

TEST_F(TracedDataTest, Propagation_GroupBy_InnerListsTracked)
{
    TempJsonFile file(R"({"items": [1, 2, 3, 4]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            groups = builtins.groupBy (x: if x > 2 then "big" else "small") data.items;
        in builtins.length groups.big
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── Negative: non-tracked container → no provenance ──────────────────
// Operations on containers NOT from ExprTracedData should NOT
// produce spurious shape deps (provenance map lookup returns null).

TEST_F(TracedDataTest, Propagation_Negative_NonTrackedContainer)
{
    // This expression builds a list literal (not from JSON),
    // then sorts it and checks length. No provenance should be propagated.
    auto expr = R"(
        let items = [3 1 2];
            sorted = builtins.sort builtins.lessThan items;
        in builtins.length sorted
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    // Should serve from cache (no file deps to invalidate)
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Fragile area tests: edge cases in provenance propagation
// ═══════════════════════════════════════════════════════════════════════

// ── Chained reconstruction: sort(filter(pred, tracked)) ──────────────
// Provenance must survive multiple reconstruction steps.

TEST_F(TracedDataTest, Propagation_Chained_SortFilter)
{
    TempJsonFile file(R"({"items": [5, 3, 1, 4, 2]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            filtered = builtins.filter (x: x > 2) data.items;
            sorted = builtins.sort builtins.lessThan filtered;
        in builtins.length sorted
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3)); // [3, 4, 5]
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Array grows → must re-evaluate
    file.modify(R"({"items": [5, 3, 1, 4, 2, 6666]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(4)); // [3, 4, 5, 6666]
    }
}

// ── Empty output from filter ──────────────────────────────────────────
// When filter produces an empty list, length returns 0 without needing
// list provenance. The input list's #len dep covers invalidation.

TEST_F(TracedDataTest, Propagation_Filter_EmptyOutput)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            filtered = builtins.filter (x: x > 100) data.items;
        in builtins.length filtered
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(0));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── ++ with one tracked input among many ─────────────────────────────
// concatLists should propagate from whichever input is tracked.

TEST_F(TracedDataTest, Propagation_ConcatLists_OneTracked)
{
    TempJsonFile file(R"({"items": [10, 20]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            combined = [1 2] ++ data.items ++ [30];
        in builtins.length combined
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(5)); // [1, 2, 10, 20, 30]
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── mapAttrs then hasAttr on derived container ───────────────────────
// hasAttr records #keys dep on the attrset. After propagation,
// this should work on mapAttrs output.

TEST_F(TracedDataTest, Propagation_MapAttrs_HasAttr)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            mapped = builtins.mapAttrs (k: v: v + 1) data;
        in mapped ? a
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.boolean(), true);
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Remove key "a" from JSON
    file.modify(R"({"b": 2, "c": 33333})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Must re-evaluate: key "a" removed
        EXPECT_EQ(v.boolean(), false);
    }
}

// ── Negative: removeAttrs on non-tracked doesn't crash ───────────────

TEST_F(TracedDataTest, Propagation_Negative_RemoveAttrsNonTracked)
{
    auto expr = R"(
        let data = { a = 1; b = 2; c = 3; };
            subset = builtins.removeAttrs data ["c"];
        in builtins.attrNames subset
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── tail of tracked list → propagation handles size-1 input ──────────

TEST_F(TracedDataTest, Propagation_Tail_SingleElement)
{
    TempJsonFile file(R"({"items": [42]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
        in builtins.length (builtins.tail data.items)
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(0)); // tail of [42] = []
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── // with tracked LHS and literal RHS ──────────────────────────────
// The layer optimization path vs merge path in ExprOpUpdate should
// both propagate correctly.

TEST_F(TracedDataTest, Propagation_OpUpdate_LayerPath)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    // Small RHS triggers the layering optimization path
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            merged = data // { z = 3; };
        in builtins.attrNames merged
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3); // x, y, z
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

} // namespace nix::eval_trace
