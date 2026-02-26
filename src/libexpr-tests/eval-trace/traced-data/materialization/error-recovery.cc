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
// The leaf trace has [ParentContext(d)] but no Content dep — the parent
// passes verify (keys unchanged), so the leaf's ParentContext passes
// and the leaf is served stale.
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
