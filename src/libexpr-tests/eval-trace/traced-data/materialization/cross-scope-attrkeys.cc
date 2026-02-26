/**
 * Cross-scope materialization tests for attrNames / SC #keys.
 * These tests exercise the cross-TracedExpr boundary where
 * materialization occurs: a root attrset with a TracedData child
 * and a sibling that observes the child's shape.
 *
 * Before the fix: these tests FAIL (no SC shape deps on materialized values).
 * After the fix: all tests PASS.
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── Basic cross-scope #keys tests ──────────────────────────────────

TEST_F(MaterializationDepTest, CrossScope_AttrNames_JSON_RecordsSCKeys)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Cross-scope: SC #keys should be recorded for attrNames on materialized value\n"
        << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_AttrNames_TOML)
{
    TempTomlFile f("[section]\na = 1\nb = 2\n");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        ft(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Cross-scope TOML: SC #keys should be recorded\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_AttrNames_ReadDir)
{
    TempDir td;
    td.addFile("fileA");
    td.addFile("fileB");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        rd(td.path()));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Cross-scope readDir: SC #keys should be recorded\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_AttrNames_MapAttrs_PreservesProvenance)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = builtins.mapAttrs (n: v: v + 1) ({}); in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Cross-scope mapAttrs: SC #keys should be preserved through mapAttrs\n"
        << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_AttrNames_Nested_RecordsSCKeys)
{
    TempJsonFile f(R"({"inner":{"x":1,"y":2}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d.inner; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Cross-scope nested: SC #keys should be recorded for d.inner attrNames\n"
        << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_AttrValues_RecordsSCKeys)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; vals = builtins.attrValues d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * valsAttr = root.attrs()->get(state.symbols.create("vals"));
        ASSERT_NE(valsAttr, nullptr);
        state.forceValue(*valsAttr->value, noPos);
    }

    auto deps = getStoredDeps("vals");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Cross-scope attrValues: SC #keys should be recorded\n"
        << dumpDeps(deps);
}

// ── Negative tests ─────────────────────────────────────────────────

TEST_F(MaterializationDepTest, CrossScope_AttrNames_NoSCType)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#type"))
        << "Cross-scope: attrNames should NOT record SC #type\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_AttrNames_NoSCHasKey)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#has:"))
        << "Cross-scope: attrNames should NOT record SC #has\n" << dumpDeps(deps);
}

// ── Multi-origin tests ─────────────────────────────────────────────

TEST_F(MaterializationDepTest, CrossScope_AttrNames_MultiOrigin_RecordsSCKeysPerOrigin)
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
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    auto scKeysCount = countDeps(deps, DepType::StructuredContent, "#keys");
    EXPECT_GE(scKeysCount, 2u)
        << "Multi-origin: should have SC #keys for each origin\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_AttrNames_MixedOrigin_RecordsSCKeysForTracedOnly)
{
    TempJsonFile f(R"({"a":1})");
    auto expr = std::format(
        "let d = ({}) // {{ extra = 1; }}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Mixed origin: SC #keys should be recorded for the TracedData origin\n"
        << dumpDeps(deps);
}

// ── Cache invalidation tests ───────────────────────────────────────

TEST_F(MaterializationDepTest, CrossScope_AttrNames_KeyAdded_CacheMiss)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    // First evaluation — populates cache
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    // Change JSON: add a key
    f.modify(R"({"a":1,"b":2,"c":3})");
    invalidateFileCache(f.path);

    // Second evaluation — should get fresh deps
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    // The result should reflect the new keys
    auto result = getStoredResult("names");
    ASSERT_TRUE(result.has_value());
    auto * strs = std::get_if<std::vector<std::string>>(& *result);
    ASSERT_NE(strs, nullptr);
    ASSERT_EQ(strs->size(), 3u) << "After key addition, names should have 3 elements";
    // Verify the actual names
    std::vector<std::string> expected{"a", "b", "c"};
    EXPECT_EQ(*strs, expected) << "Names should be [a, b, c]";
}

TEST_F(MaterializationDepTest, CrossScope_AttrNames_ValueChanged_CacheHit)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    // First evaluation
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    // Change a value but not the keys
    f.modify(R"({"a":99,"b":99})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto result = getStoredResult("names");
    ASSERT_TRUE(result.has_value());
    auto * strs = std::get_if<std::vector<std::string>>(& *result);
    ASSERT_NE(strs, nullptr);
    std::vector<std::string> expected{"a", "b"};
    EXPECT_EQ(*strs, expected) << "Names should still be [a, b] after value change";
}

} // namespace nix::eval_trace::test
