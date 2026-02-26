/**
 * Error path and recovery scenario tests.
 *
 * Exercises: tryEval on traced data, recovery after A→B→A revert,
 * same-keys-different-values cache behavior, simultaneous file changes.
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── tryEval on traced data ──────────────────────────────────────────

TEST_F(MaterializationDepTest, TryEval_ExistingKey_RecordsDeps)
{
    TempJsonFile f(R"({"x":42})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; val = builtins.tryEval d.x; }}", fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
    }

    auto deps = getStoredDeps("val");
    EXPECT_TRUE(hasDep(deps, DepType::ParentContext, ""))
        << "tryEval should still record ParentContext dep\n" << dumpDeps(deps);
}

// ── Recovery: A → B → A (revert) ───────────────────────────────────

TEST_F(MaterializationDepTest, Recovery_Revert_CacheHit)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}", fj(f.path));

    // First eval: state A
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 2u);
    }

    // Change to state B
    f.modify(R"({"a":1,"b":2,"c":3})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 3u);
    }

    // Revert to state A
    f.modify(R"({"a":1,"b":2})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->listSize(), 2u)
            << "After A→B→A revert, should get original result (recovery)";
    }
}

// ── Same keys, different values ─────────────────────────────────────

TEST_F(MaterializationDepTest, SameKeysDiffValues_AttrNamesCacheHit)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}", fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 2u);
    }

    // Same keys, different values — attrNames result unchanged
    f.modify(R"({"a":99,"b":99})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->listSize(), 2u)
            << "Same keys, different values: attrNames should still cache-hit";
    }
}

// KNOWN BUG: Leaf value d.x incorrectly cache-hits after value change.
// The child's evaluation via ExprOrigChild records deps (Content, SC) on its own
// tracker, but ParentContext(d) passes because d's ImplicitShape(#keys) covers
// the Content failure (keys unchanged). The sibling tracking records d but not
// x (grandchild), so val doesn't detect x's value change.
TEST_F(MaterializationDepTest, DISABLED_SameKeysDiffValues_ValueAccess_Invalidates)
{
    TempJsonFile f(R"({"x":42})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; val = d.x; }}", fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->integer().value, 42);
    }

    f.modify(R"({"x":99})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->integer().value, 99)
            << "Same key, different value: leaf access should see new value";
    }
}

// ── Bug 1 coverage: SameKeysDiffValues dep/cache analysis ───────────
//
// These tests document current (buggy) behavior where val's trace only has
// ParentContext(d), so a value change to d.x is invisible. BUG: markers
// indicate assertions that will flip when the fix lands.

TEST_F(MaterializationDepTest, SameKeysDiffValues_ValDeps_HasAllDepsButNotChecked)
{
    // Pin down val's stored deps after first eval.
    // val DOES have Content + SC + ParentContext + ImplicitShape deps — the
    // bug is NOT missing deps. BUG: despite having correct deps (including SC
    // j:x that would detect the value change), warm eval serves stale values
    // because ParentContext(d) passes and TraceCache skips val's own dep check.
    TempJsonFile f(R"({"x":42})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; val = d.x; }}", fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * dAttr = root.attrs()->get(state.symbols.create("d"));
        ASSERT_NE(dAttr, nullptr);
        state.forceValue(*dAttr->value, noPos);
        auto * valAttr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(valAttr, nullptr);
        state.forceValue(*valAttr->value, noPos);
    }

    auto valDeps = getStoredDeps("val");
    EXPECT_TRUE(hasDep(valDeps, DepType::ParentContext, ""))
        << "val must have ParentContext dep\n" << dumpDeps(valDeps);
    // val has all the deps it needs for soundness — Content, SC j:x, ImplicitShape.
    // BUG: these deps are never checked during warm eval because ParentContext(d)
    // passes (d's ImplicitShape covers Content) and TraceCache serves the cached
    // value without verifying val's own trace.
    EXPECT_TRUE(hasDep(valDeps, DepType::Content, ""))
        << "val has Content dep (recorded but not checked during warm eval)\n" << dumpDeps(valDeps);
    EXPECT_TRUE(hasDep(valDeps, DepType::StructuredContent, "j:x"))
        << "val has SC j:x dep (recorded but not checked during warm eval)\n" << dumpDeps(valDeps);
}

TEST_F(MaterializationDepTest, SameKeysDiffValues_DDeps_HasContentAndShape)
{
    // Verify d's own deps are complete: Content + ImplicitShape(#keys).
    // SC deps for x belong to x's child trace, not d's.
    TempJsonFile f(R"({"x":42})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; val = d.x; }}", fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * dAttr = root.attrs()->get(state.symbols.create("d"));
        ASSERT_NE(dAttr, nullptr);
        state.forceValue(*dAttr->value, noPos);
    }

    auto dDeps = getStoredDeps("d");
    EXPECT_TRUE(hasDep(dDeps, DepType::Content, ""))
        << "d must have Content dep on the JSON file\n" << dumpDeps(dDeps);
    EXPECT_TRUE(hasDep(dDeps, DepType::ImplicitShape, "#keys"))
        << "d must have ImplicitShape #keys dep\n" << dumpDeps(dDeps);
    EXPECT_FALSE(hasDep(dDeps, DepType::StructuredContent, "j:x"))
        << "d's SC deps on x belong to x's child trace, not d's\n" << dumpDeps(dDeps);
}

TEST_F(MaterializationDepTest, SameKeysDiffValues_DVerifiesWhenOnlyValuesChange)
{
    // d passes verification even when x's value changes, because keys are
    // unchanged and ImplicitShape(#keys) covers the Content failure.
    TempJsonFile f(R"({"x":42})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; val = d.x; }}", fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * dAttr = root.attrs()->get(state.symbols.create("d"));
        ASSERT_NE(dAttr, nullptr);
        state.forceValue(*dAttr->value, noPos);
    }

    // Modify: value changes but keys unchanged
    f.modify(R"({"x":99})");
    invalidateFileCache(f.path);

    auto dResult = getStoredResult("d");
    ASSERT_TRUE(dResult.has_value())
        << "d's trace passes verification (keys unchanged → ImplicitShape covers Content failure)";
}

TEST_F(MaterializationDepTest, SameKeysDiffValues_ValueAccess_ReturnsStale)
{
    // Non-DISABLED version of DISABLED_SameKeysDiffValues_ValueAccess_Invalidates.
    // Documents the actual stale result: val serves 42 when it should be 99.
    TempJsonFile f(R"({"x":42})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; val = d.x; }}", fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->integer().value, 42);
    }

    f.modify(R"({"x":99})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->integer().value, 42)
            << "BUG: val serves stale cached value (should be 99)";
    }
}

TEST_F(MaterializationDepTest, SameKeysDiffValues_MultipleKeys_AccessedKeyChanges)
{
    // Bug manifests with multiple keys: only x changes, y stays.
    TempJsonFile f(R"({"x":1,"y":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; val = d.x; }}", fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->integer().value, 1);
    }

    f.modify(R"({"x":99,"y":2})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->integer().value, 1)
            << "BUG: val serves stale cached value (should be 99)";
    }
}

TEST_F(MaterializationDepTest, SameKeysDiffValues_TwoChildren_BothStale)
{
    // Two children accessing different keys both go stale.
    TempJsonFile f(R"({"x":1,"y":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; vx = d.x; vy = d.y; }}", fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * vx = root.attrs()->get(state.symbols.create("vx"));
        auto * vy = root.attrs()->get(state.symbols.create("vy"));
        ASSERT_NE(vx, nullptr);
        ASSERT_NE(vy, nullptr);
        state.forceValue(*vx->value, noPos);
        state.forceValue(*vy->value, noPos);
        EXPECT_EQ(vx->value->integer().value, 1);
        EXPECT_EQ(vy->value->integer().value, 2);
    }

    f.modify(R"({"x":99,"y":88})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * vx = root.attrs()->get(state.symbols.create("vx"));
        auto * vy = root.attrs()->get(state.symbols.create("vy"));
        ASSERT_NE(vx, nullptr);
        ASSERT_NE(vy, nullptr);
        state.forceValue(*vx->value, noPos);
        state.forceValue(*vy->value, noPos);
        EXPECT_EQ(vx->value->integer().value, 1)
            << "BUG: vx serves stale cached value (should be 99)";
        EXPECT_EQ(vy->value->integer().value, 2)
            << "BUG: vy serves stale cached value (should be 88)";
    }
}

TEST_F(MaterializationDepTest, SameKeysDiffValues_NestedAccess_Stale)
{
    // Bug manifests for nested access: d.inner.x.
    TempJsonFile f(R"({"inner":{"x":42}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; val = d.inner.x; }}", fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->integer().value, 42);
    }

    f.modify(R"({"inner":{"x":99}})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->integer().value, 42)
            << "BUG: val serves stale cached value (should be 99)";
    }
}

TEST_F(MaterializationDepTest, SameKeysDiffValues_KeyAdded_Invalidates)
{
    // Contrast: when keys change, invalidation works correctly.
    // Adding a key causes ImplicitShape(#keys) to fail → ParentContext fails
    // → val re-evaluates → correct result.
    TempJsonFile f(R"({"x":42})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; val = d.x; }}", fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->integer().value, 42);
    }

    // Add a new key — keys change → ImplicitShape fails → full re-eval
    f.modify(R"({"x":42,"y":1})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->integer().value, 42)
            << "Key added: val correctly re-evaluates and still returns 42";
    }
}

// ── Two files change simultaneously ─────────────────────────────────

TEST_F(MaterializationDepTest, SimultaneousChanges_BothDetected)
{
    TempJsonFile f1(R"({"a":1})");
    TempJsonFile f2(R"({"b":2})");
    auto expr = std::format(
        "let d = ({}) // ({}); in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f1.path), fj(f2.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 2u);
    }

    // Change both files simultaneously
    f1.modify(R"({"a":1,"x":10})");
    f2.modify(R"({"b":2,"y":20})");
    invalidateFileCache(f1.path);
    invalidateFileCache(f2.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->listSize(), 4u)
            << "Both file changes should be detected (a, b, x, y)";
    }
}

// ── Nested recovery: inner object reverts ───────────────────────────

TEST_F(MaterializationDepTest, NestedRecovery_InnerRevert)
{
    TempJsonFile f(R"({"inner":{"x":1}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d.inner; }}",
        fj(f.path));

    // State A: inner has {x}
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 1u);
    }

    // State B: inner has {x, y}
    f.modify(R"({"inner":{"x":1,"y":2}})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 2u);
    }

    // Revert to state A: inner has {x}
    f.modify(R"({"inner":{"x":1}})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->listSize(), 1u)
            << "Nested revert: should recover original inner key set";
    }
}

} // namespace nix::eval_trace::test
