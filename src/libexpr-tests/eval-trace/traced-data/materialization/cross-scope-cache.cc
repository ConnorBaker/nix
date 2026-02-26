/**
 * End-to-end cross-scope cache behavior tests.
 * Tests that shape deps survive cache hit/miss cycles and that
 * independent siblings invalidate correctly.
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

TEST_F(MaterializationDepTest, CacheHit_ShapeDepsPreserved_InStoredTrace)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    // First run — fresh evaluation
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps1 = getStoredDeps("names");
    EXPECT_TRUE(hasDep(deps1, DepType::StructuredContent, "#keys"))
        << "First run: SC #keys should be stored\n" << dumpDeps(deps1);

    // Second run — should be a cache hit, but stored deps should remain
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto deps2 = getStoredDeps("names");
    EXPECT_TRUE(hasDep(deps2, DepType::StructuredContent, "#keys"))
        << "Second run: SC #keys should still be in stored trace\n" << dumpDeps(deps2);
}

TEST_F(MaterializationDepTest, CacheHit_ReplayedDepsMatchFreshDeps)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    // Fresh evaluation
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto freshDeps = getStoredDeps("names");

    // Second evaluation — should hit cache
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    auto cachedDeps = getStoredDeps("names");

    EXPECT_TRUE(hasDep(freshDeps, DepType::StructuredContent, "#keys"))
        << "Fresh deps should have SC #keys\n" << dumpDeps(freshDeps);
    EXPECT_TRUE(hasDep(cachedDeps, DepType::StructuredContent, "#keys"))
        << "Cached deps should have SC #keys\n" << dumpDeps(cachedDeps);
}

TEST_F(MaterializationDepTest, MultiChild_IndependentInvalidation)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; scalar = 42; }}",
        fj(f.path));

    // First evaluation
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
        auto * scalarAttr = root.attrs()->get(state.symbols.create("scalar"));
        ASSERT_NE(scalarAttr, nullptr);
        state.forceValue(*scalarAttr->value, noPos);
    }

    // scalar should have its own stored result
    auto scalarResult = getStoredResult("scalar");
    ASSERT_TRUE(scalarResult.has_value());

    // Change JSON keys — invalidates 'names' but not 'scalar'
    f.modify(R"({"a":1,"b":2,"c":3})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
        auto * scalarAttr = root.attrs()->get(state.symbols.create("scalar"));
        ASSERT_NE(scalarAttr, nullptr);
        state.forceValue(*scalarAttr->value, noPos);
    }

    // Names should reflect the new keys
    auto namesResult = getStoredResult("names");
    ASSERT_TRUE(namesResult.has_value());
    auto * strs = std::get_if<std::vector<std::string>>(& *namesResult);
    ASSERT_NE(strs, nullptr);
    std::vector<std::string> expectedNames{"a", "b", "c"};
    EXPECT_EQ(*strs, expectedNames) << "names should be [a, b, c] after key addition";

    // scalar should still be valid
    auto scalarResult2 = getStoredResult("scalar");
    ASSERT_TRUE(scalarResult2.has_value());
    auto * ival = std::get_if<int_t>(& *scalarResult2);
    ASSERT_NE(ival, nullptr);
    EXPECT_EQ(ival->x.value, 42);
}

TEST_F(MaterializationDepTest, NestedAttrset_CrossScope_ShapeDeps)
{
    TempJsonFile f(R"({"inner":{"x":1}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d.inner; }}",
        fj(f.path));

    // Evaluate and check that SC #keys is recorded for the nested attr
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
        EXPECT_EQ(namesAttr->value->listSize(), 1u);
    }

    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Nested cross-scope: SC #keys should be recorded\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, NestedAttrset_CrossScope_CacheMiss)
{
    TempJsonFile f(R"({"inner":{"x":1}})");
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
        ASSERT_EQ(namesAttr->value->listSize(), 1u);
    }

    // Add a key to the nested object
    f.modify(R"({"inner":{"x":1,"y":2}})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
        EXPECT_EQ(namesAttr->value->listSize(), 2u)
            << "Nested: should have 2 keys after addition";
    }
}

} // namespace nix::eval_trace::test
