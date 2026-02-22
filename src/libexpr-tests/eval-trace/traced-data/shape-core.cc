#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/traced-data.hh"
#include "nix/expr/json-to-value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── mapAttrs / key-set interaction ────────────────────────────────────
// These tests verify that the lazy attrset design is correct when combined
// with Nix builtins that iterate keys (mapAttrs, attrNames). The key
// invariant: key-set changes only affect traces at the level that
// materializes the attrset structure, where only Content deps exist
// (no StructuredContent override possible).

TEST_F(TracedDataTest, TracedJSON_MapAttrsNoForce)
{
    // mapAttrs iterates keys but doesn't force values → only Content dep
    // recorded. Any file change must invalidate (no StructuredContent override).
    TempJsonFile file(R"({"a": "1", "b": "2"})");
    auto expr = R"(builtins.mapAttrs (n: v: v + "-mapped") (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nAttrs);
    }

    // Add a key → Content fails, no StructuredContent deps → re-eval
    file.modify(R"({"a": "1", "b": "2", "c": "3"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate
    }
}

TEST_F(TracedDataTest, TracedJSON_MapAttrsValueAccess)
{
    // mapAttrs + access specific value → StructuredContent dep recorded for
    // that value. Adding an unused key to the JSON file must NOT invalidate
    // because the StructuredContent override covers the accessed value and
    // the cached result is that specific value (not the full attrset).
    TempJsonFile file(R"({"a": "stable", "b": "other"})");
    auto expr = R"((builtins.mapAttrs (n: v: v + "-mapped") (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"())).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable-mapped"));
    }

    // Add unused key → Content fails, StructuredContent for "a" passes → override
    file.modify(R"({"a": "stable", "b": "other", "c": "new!!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // served from cache
        EXPECT_THAT(v, IsStringEq("stable-mapped"));
    }

    // Change accessed value → StructuredContent fails → re-eval
    file.modify(R"({"a": "CHANGED", "b": "other", "c": "new!!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED-mapped"));
    }
}

TEST_F(TracedDataTest, TracedJSON_AttrNamesNoLeafAccess)
{
    // attrNames enumerates keys without forcing values → only Content dep.
    // Any file change invalidates (no StructuredContent override).
    TempJsonFile file(R"({"x": 1, "y": 2, "z": 3})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    // Add a key → Content fails, no StructuredContent → must re-eval
    file.modify(R"({"w": 0, "x": 1, "y": 2, "z": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate
    }
}

TEST_F(TracedDataTest, TracedJSON_MapAttrsRemoveUnusedKey)
{
    // Flake-compat scenario: mapAttrs over lock file inputs, access specific
    // input. Removing an unused input must NOT invalidate.
    TempJsonFile file(R"({"nixpkgs": {"rev": "abc123"}, "unused-input": {"rev": "def456"}})");
    auto expr = R"((builtins.mapAttrs (n: v: v.rev) (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"())).nixpkgs)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("abc123"));
    }

    // Remove unused input → Content fails, StructuredContent for nixpkgs.rev passes
    file.modify(R"({"nixpkgs": {"rev": "abc123"}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // served from cache
        EXPECT_THAT(v, IsStringEq("abc123"));
    }
}

// ── List size change correctness ─────────────────────────────────────
// Regression test: TracedExpr's allStrings recording path used to force
// all list elements in the same DependencyTracker, mixing StructuredContent
// deps with the Content dep. Adding an element would cause Content to fail
// but all existing StructuredContent deps to pass, incorrectly overriding.

TEST_F(TracedDataTest, TracedJSON_ListElementAdded)
{
    // Access a specific element of a JSON array via fromJSON(readFile f).
    TempJsonFile file(R"({"items": ["alpha", "beta"]})");
    auto expr = R"(builtins.elemAt (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()).items 0)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("alpha"));
    }

    // Add an element to the array (size changes, accessed element unchanged)
    file.modify(R"({"items": ["alpha", "beta", "gamma"]})");
    invalidateFileCache(file.path);

    // StructuredContent for items.[0] passes, Content fails.
    // Override should apply because the cached result is "alpha" (the leaf
    // value, not the list) — the list size is irrelevant to this result.
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // served from cache (override correct)
        EXPECT_THAT(v, IsStringEq("alpha"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ListSizeChange_NoLeafAccess)
{
    // Access the entire list without forcing elements → only Content dep.
    // Any file change must invalidate.
    TempJsonFile file(R"(["a", "b", "c"])");
    auto expr = "builtins.fromJSON (builtins.readFile "
        + file.path.string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    // Change list size
    file.modify(R"(["a", "b", "c", "d"])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Shape observation tracking tests
// ═══════════════════════════════════════════════════════════════════════

// ── Core bug reproduction ─────────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_LengthPlusElemAt_ShapeChange)
{
    // THE motivating bug: toString(length arr) + "-" + elemAt arr 0
    // If array grows but element 0 is unchanged, shape dep #len must fail
    // to prevent serving stale "2-alpha" when answer should be "3-alpha".
    TempJsonFile file(R"({"arr": ["alpha", "beta"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (toString (builtins.length j.arr)) + "-" + (builtins.elemAt j.arr 0))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("2-alpha"));
    }

    // Add element: array grows from 2 to 3, but element 0 unchanged
    file.modify(R"({"arr": ["alpha", "beta", "gamma"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (shape dep #len fails)
        EXPECT_THAT(v, IsStringEq("3-alpha"));
    }
}

TEST_F(TracedDataTest, TracedJSON_LengthPlusElemAt_NoShapeChange)
{
    // Same expression, but values change while length stays the same.
    // Shape dep #len passes AND leaf dep passes → override applies.
    TempJsonFile file(R"({"arr": ["alpha", "beta"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (toString (builtins.length j.arr)) + "-" + (builtins.elemAt j.arr 0))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("2-alpha"));
    }

    // Change element 1 (unaccessed), keep length and element 0 same
    file.modify(R"({"arr": ["alpha", "CHANGED"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // served from cache (override applies)
        EXPECT_THAT(v, IsStringEq("2-alpha"));
    }
}

// ── List length shape deps ────────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_LengthOnly_ShapeChange)
{
    // length(arr) must invalidate when array grows
    TempJsonFile file(R"({"arr": ["a", "b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length j.arr)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"arr": ["a", "b", "c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(TracedDataTest, TracedJSON_LengthOnly_ContentChange)
{
    // length(arr) survives when values change but length stays same
    TempJsonFile file(R"({"arr": ["a", "b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length j.arr)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"arr": ["CHANGED!", "b"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // served from cache
        EXPECT_THAT(v, IsIntEq(2));
    }
}

TEST_F(TracedDataTest, TracedJSON_RootArrayLength)
{
    // length(fromJSON(readFile f)) where root is array
    TempJsonFile file(R"(["x", "y", "z"])");
    auto expr = R"(builtins.length (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"(["x", "y", "z", "w"])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate
        EXPECT_THAT(v, IsIntEq(4));
    }
}

TEST_F(TracedDataTest, TracedJSON_NestedListLength)
{
    // length(obj.items) where items is nested list
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [1, 2, 3, 4]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate
        EXPECT_THAT(v, IsIntEq(4));
    }
}

// ── Attrset key set shape deps ────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_AttrNamesOnly_KeySetChange)
{
    // attrNames(obj) invalidates when key added
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify(R"({"a": 1, "b": 2, "c": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#keys fails)
    }
}

TEST_F(TracedDataTest, TracedJSON_AttrNamesOnly_ValueChange)
{
    // attrNames(obj) survives when values change but keys same
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify(R"({"a": 99, "b": 88})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // served from cache (#keys passes)
    }
}

TEST_F(TracedDataTest, TracedJSON_AttrNamesPlusValue_KeySetChange)
{
    // concatStringsSep "," (attrNames obj) + ":" + obj.a
    // Adding a key must invalidate because #keys changes
    TempJsonFile file(R"({"a": "alpha", "b": "beta"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.concatStringsSep "," (builtins.attrNames j)) + ":" + j.a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b:alpha"));
    }

    file.modify(R"({"a": "alpha", "b": "beta", "c": "gamma"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#keys fails)
        EXPECT_THAT(v, IsStringEq("a,b,c:alpha"));
    }
}

TEST_F(TracedDataTest, TracedJSON_NestedAttrNames)
{
    // attrNames(obj.inner) where inner is nested object
    TempJsonFile file(R"({"inner": {"x": 1, "y": 2}})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()).inner)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify(R"({"inner": {"x": 1, "y": 2, "z": 3}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#keys fails)
    }
}

// ── hasAttr shape deps ────────────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_HasAttr_KeyAdded)
{
    // hasAttr "b" obj returns false, key "b" added → must invalidate
    TempJsonFile file(R"({"a": 1})");
    auto expr = R"(builtins.hasAttr "b" (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    file.modify(R"({"a": 1, "b": 2})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#has:b changes false→true)
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(TracedDataTest, TracedJSON_HasAttr_KeyRemoved)
{
    // hasAttr "b" obj returns true, key "b" removed → must invalidate
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(builtins.hasAttr "b" (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"a": 1000})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#has:b changes true→false)
        EXPECT_THAT(v, IsFalse());
    }
}

TEST_F(TracedDataTest, TracedJSON_HasAttr_ValueChange)
{
    // hasAttr "a" obj survives when values change but keys same
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(builtins.hasAttr "a" (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"a": 999, "b": 888})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // served from cache (#has:a still true)
        EXPECT_THAT(v, IsTrue());
    }
}

// ── Regression tests (existing behavior preserved) ────────────────────

TEST_F(TracedDataTest, TracedJSON_PointAccessSurvivesKeyAddition)
{
    // data.x still works when key y added (no shape dep, override applies)
    TempJsonFile file(R"({"x": "stable"})");
    auto expr = R"((builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()).x)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    file.modify(R"({"x": "stable", "y": "added!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // override applies (no shape dep)
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ElemAtSurvivesArrayGrowth)
{
    // elemAt(arr, 0) still works when array grows (no shape dep, override applies)
    TempJsonFile file(R"(["first", "second"])");
    auto expr = R"(builtins.elemAt (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()) 0)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("first"));
    }

    file.modify(R"(["first", "second", "third"])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // override applies (no shape dep)
        EXPECT_THAT(v, IsStringEq("first"));
    }
}

TEST_F(TracedDataTest, TracedJSON_MapAttrsValueAccess_NoShapeDep)
{
    // (mapAttrs f data).a still works when key added (no shape builtin used)
    TempJsonFile file(R"({"a": "stable", "b": "other"})");
    auto expr = R"((builtins.mapAttrs (n: v: v + "!") (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"())).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable!"));
    }

    file.modify(R"({"a": "stable", "b": "other", "c": "new!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // override applies (no shape builtin)
        EXPECT_THAT(v, IsStringEq("stable!"));
    }
}

// ── TOML shape deps ──────────────────────────────────────────────────

TEST_F(TracedDataTest, TracedTOML_LengthChange)
{
    // TOML array length change invalidates
    TempTomlFile file("items = [\"a\", \"b\"]\n");
    auto expr = R"(let t = builtins.fromTOML (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length t.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify("items = [\"a\", \"b\", \"c\"]\n");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#len fails)
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(TracedDataTest, TracedTOML_KeySetChange)
{
    // TOML table key set change invalidates
    TempTomlFile file("[section]\na = 1\nb = 2\n");
    auto expr = R"(builtins.attrNames (builtins.fromTOML (builtins.readFile )" + file.path.string() + R"()).section)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify("[section]\na = 1\nb = 2\nc = 3\n");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#keys fails)
    }
}

} // namespace nix::eval_trace
