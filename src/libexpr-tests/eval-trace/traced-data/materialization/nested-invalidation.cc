/**
 * Cross-scope nested invalidation tests.
 *
 * Tests that cache invalidation propagates correctly through nested
 * TracedData access patterns across materialization boundaries.
 * Each test exercises a specific nesting pattern with file modification.
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── 3-level nesting: d.a.b ──────────────────────────────────────────

TEST_F(MaterializationDepTest, ThreeLevel_AttrNames_RecordsSCKeys)
{
    TempJsonFile f(R"({"a":{"b":{"x":1,"y":2}}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d.a.b; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "3-level nested attrNames should record SC #keys\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#type"))
        << "attrNames should NOT record SC #type\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, ThreeLevel_KeyAdded_CacheMiss)
{
    TempJsonFile f(R"({"a":{"b":{"x":1}}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d.a.b; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 1u);
    }

    f.modify(R"({"a":{"b":{"x":1,"y":2}}})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->listSize(), 2u)
            << "3-level: should detect key addition in deeply nested object";
    }
}

// ── Nested + merge (//) ─────────────────────────────────────────────

TEST_F(MaterializationDepTest, NestedMerge_AttrNames_RecordsSCKeys)
{
    TempJsonFile f(R"({"inner":{"a":1}})");
    auto expr = std::format(
        "let d = ({}).inner // {{ extra = 1; }}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Nested merge attrNames should record SC #keys\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, NestedMerge_KeyAdded_CacheMiss)
{
    TempJsonFile f(R"({"inner":{"a":1}})");
    auto expr = std::format(
        "let d = ({}).inner // {{ extra = 1; }}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 2u); // a + extra
    }

    f.modify(R"({"inner":{"a":1,"b":2}})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->listSize(), 3u)
            << "Nested merge: should detect key addition (a, b, extra)";
    }
}

// ── Nested hasAttr cross-scope invalidation ─────────────────────────

TEST_F(MaterializationDepTest, NestedHasAttr_KeyRemoved_CacheMiss)
{
    TempJsonFile f(R"({"inner":{"x":1,"y":2}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; has = d.inner ? x; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("has"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_TRUE(attr->value->boolean());
    }

    // Remove x from inner
    f.modify(R"({"inner":{"y":2}})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("has"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_FALSE(attr->value->boolean())
            << "Nested hasAttr: should detect key removal from inner object";
    }
}

TEST_F(MaterializationDepTest, NestedHasAttr_RecordsSCHasKey)
{
    TempJsonFile f(R"({"inner":{"x":1}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; has = d.inner ? x; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("has"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
    }

    auto deps = getStoredDeps("has");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#has:x"))
        << "Nested hasAttr should record SC #has:x\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Nested hasAttr should NOT record SC #keys\n" << dumpDeps(deps);
}

// ── Nested typeOf cross-scope invalidation ──────────────────────────

TEST_F(MaterializationDepTest, NestedTypeOf_RecordsSCType)
{
    TempJsonFile f(R"({"inner":{"x":1}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; t = builtins.typeOf d.inner; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("t"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(std::string(attr->value->c_str()), "set");
    }

    auto deps = getStoredDeps("t");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#type"))
        << "Nested typeOf should record SC #type\n" << dumpDeps(deps);
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Nested typeOf should NOT record SC #keys\n" << dumpDeps(deps);
}

// ── Nested attrNames through mapAttrs ───────────────────────────────

TEST_F(MaterializationDepTest, MapAttrsNested_AttrNames_RecordsSCKeys)
{
    TempJsonFile f(R"({"inner":{"a":1,"b":2}})");
    auto expr = std::format(
        "let d = builtins.mapAttrs (n: v: v) (({}).inner); "
        "in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "mapAttrs on nested TracedData should record SC #keys\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, MapAttrsNested_KeyAdded_CacheMiss)
{
    TempJsonFile f(R"({"inner":{"a":1}})");
    auto expr = std::format(
        "let d = builtins.mapAttrs (n: v: v) (({}).inner); "
        "in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 1u);
    }

    f.modify(R"({"inner":{"a":1,"b":2}})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->listSize(), 2u)
            << "mapAttrs nested: should detect key addition in inner object";
    }
}

// ── Value change in nested object (should NOT invalidate attrNames) ─

TEST_F(MaterializationDepTest, NestedValueChange_AttrNames_CacheHit)
{
    TempJsonFile f(R"({"inner":{"x":1,"y":2}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d.inner; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        ASSERT_EQ(attr->value->listSize(), 2u);
    }

    // Change values but not keys
    f.modify(R"({"inner":{"x":99,"y":99}})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(attr->value->listSize(), 2u)
            << "Value change in nested object should NOT invalidate attrNames";
    }
}

} // namespace nix::eval_trace::test
