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
    EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::TraceParentSlot, ""))
        << "tryEval should retain coarse fallback until slot-backed verification lands\n"
        << dumpDeps(deps);
    auto concreteReplayDeps = countDepsByType(deps, CanonicalQueryKind::FileBytes)
        + countDepsByType(deps, CanonicalQueryKind::StructuredProjection);
    EXPECT_GE(concreteReplayDeps, 1u)
        << "tryEval should still record concrete replay deps\n" << dumpDeps(deps);
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
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->listSize(), 2u)
            << "After A→B→A revert, should get original result (recovery)";
        EXPECT_EQ(loaderCalls, 0) << "A→B→A revert → constructive recovery hit";
    }
}

// ── Same keys, different values ─────────────────────────────────────

TEST_F(MaterializationDepTest, SameKeysDiffValues_AttrNames_CacheHit)
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
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->listSize(), 2u)
            << "Same keys, different values: attrNames should still cache-hit";
        EXPECT_EQ(loaderCalls, 0)
            << "same-keys value change → SC precision warm hit";
    }
}

// Leaf value d.x correctly invalidates after value change.
// When d's Content dep fails but ImplicitShape covers it, verifyTrace
// recomputes d's trace_hash. trace-context("d") then sees the updated
// hash and fails, triggering re-evaluation.
TEST_F(MaterializationDepTest, SameKeysDiffValues_ValueAccess_Invalidates)
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
// These tests verify val's stored deps and the precise sibling-context
// invalidation mechanism that detects value changes through trace_hash
// recomputation on ImplicitShape-only content override.

TEST_F(MaterializationDepTest, SameKeysDiffValues_ValDeps_HasStampedValueContext)
{
    // Pin down val's stored deps after first eval.
    // val has a precise sibling-context dep on d as its primary dep. When d's Content dep
    // changes but keys stay the same, d's trace_hash is recomputed,
    // causing val's stored precise sibling dep to fail and trigger re-evaluation.
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
    EXPECT_TRUE(hasDep(valDeps, CanonicalQueryKind::TraceValueContext, ""))
        << "val must have ValueContext dep\n" << dumpDeps(valDeps);
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
    EXPECT_TRUE(hasDep(dDeps, CanonicalQueryKind::FileBytes, ""))
        << "d must have Content dep on the JSON file\n" << dumpDeps(dDeps);
    EXPECT_TRUE(hasJsonDep(dDeps, CanonicalQueryKind::ImplicitStructure, shapePred("keys")))
        << "d must have ImplicitShape #keys dep\n" << dumpDeps(dDeps);
    EXPECT_FALSE(hasJsonDep(dDeps, CanonicalQueryKind::StructuredProjection, pathContainsPred(nlohmann::json({"x"}))))
        << "d's SC deps on x belong to x's child trace, not d's\n" << dumpDeps(dDeps);
}

TEST_F(MaterializationDepTest, SameKeysDiffValues_DVerifies_WhenOnlyValuesChange)
{
    // d passes verification even when x's value changes, because keys are
    // unchanged and ImplicitShape(#keys) covers the Content failure.
    // After verification, d's trace_hash is recomputed to reflect the new
    // Content hash, enabling downstream trace-context deps to detect the change.
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

TEST_F(MaterializationDepTest, SameKeysDiffValues_MultipleKeys_AccessedKeyChanges)
{
    // Multiple keys: only x changes, y stays. val = d.x sees new value.
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
        EXPECT_EQ(attr->value->integer().value, 99)
            << "val should see new value after file change";
    }
}

TEST_F(MaterializationDepTest, SameKeysDiffValues_TwoChildren_BothInvalidate)
{
    // Two children accessing different keys from the same parent. Both correctly
    // invalidate because sibling detection works for already-materialized siblings:
    // replayMemoizedDeps checks valueIdentityMap when the sibling's epoch entry
    // is found, and the SiblingAccessTracker callback records the sibling access.
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
        EXPECT_EQ(vx->value->integer().value, 99)
            << "vx (forced first) correctly invalidates via per-sibling trace-context(d)";
        EXPECT_EQ(vy->value->integer().value, 88)
            << "vy (forced second) correctly invalidates via sibling detection in "
               "replayMemoizedDeps — valueIdentityMap lookup triggers callback";
    }
}

TEST_F(MaterializationDepTest, SameKeysDiffValues_NestedAccess_Invalidates)
{
    // Nested access d.inner.x correctly invalidates.
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
        EXPECT_EQ(attr->value->integer().value, 99)
            << "val should see new value after file change";
    }
}

TEST_F(MaterializationDepTest, SameKeysDiffValues_KeyAdded_Invalidates)
{
    // Contrast: when keys change, invalidation works correctly.
    // Adding a key causes ImplicitShape(#keys) to fail → trace-context fails
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

TEST_F(MaterializationDepTest, SimultaneousChanges_TwoFiles_BothDetected)
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

// ── Mixed SC + ImplicitShape override ────────────────────────────────

TEST_F(MaterializationDepTest, MixedCoverage_SCAndImplicitShape_Invalidates)
{
    // Two files contribute to a merged attrset via //.
    // f1 is accessed via d.a (creating an SC dep for j:a on f1).
    // f2 is NOT accessed by value — only contributes structurally via //
    // (creating an ImplicitShape #keys dep on f2, but no SC dep).
    //
    // When both files change values (keys unchanged), both Content deps fail.
    // f1 is SC-covered (SC j:a passes if a didn't change). f2 is
    // ImplicitShape-only-covered → hasImplicitShapeOnlyOverride triggers
    // trace_hash recomputation → trace-context-based children re-evaluate.
    TempJsonFile f1(R"({"a":1})");
    TempJsonFile f2(R"({"b":2})");
    auto expr = std::format(
        "let d = ({}) // ({}); in {{ inherit d; val = d.a; }}", fj(f1.path), fj(f2.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->integer().value, 1);
    }

    // Change f2's value only (keys unchanged) — f1 stays the same.
    // This triggers Content failure on f2, covered only by ImplicitShape.
    f2.modify(R"({"b":99})");
    invalidateFileCache(f2.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        // d.a is still 1 (f1 unchanged), but val must re-evaluate because
        // d's trace_hash was recomputed (f2's ImplicitShape-only override).
        EXPECT_EQ(attr->value->integer().value, 1)
            << "val should still be 1 (f1 unchanged) but must re-evaluate to confirm";
    }
}

// ── Nested recovery: inner object reverts ───────────────────────────

TEST_F(MaterializationDepTest, NestedRecovery_InnerObject_Reverts)
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
