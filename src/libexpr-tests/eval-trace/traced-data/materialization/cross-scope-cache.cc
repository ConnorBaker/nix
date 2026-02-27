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
    EXPECT_TRUE(hasJsonDep(deps1, DepType::StructuredContent, shapePred("keys")))
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
    EXPECT_TRUE(hasJsonDep(deps2, DepType::StructuredContent, shapePred("keys")))
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

    EXPECT_TRUE(hasJsonDep(freshDeps, DepType::StructuredContent, shapePred("keys")))
        << "Fresh deps should have SC #keys\n" << dumpDeps(freshDeps);
    EXPECT_TRUE(hasJsonDep(cachedDeps, DepType::StructuredContent, shapePred("keys")))
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
        for (size_t i = 0; i < namesAttr->value->listSize(); i++)
            state.forceValue(*namesAttr->value->listView()[i], noPos);
        auto * scalarAttr = root.attrs()->get(state.symbols.create("scalar"));
        ASSERT_NE(scalarAttr, nullptr);
        state.forceValue(*scalarAttr->value, noPos);
    }

    // Names stored as list_t; verify child element traces
    auto namesResult = getStoredResult("names");
    ASSERT_TRUE(namesResult.has_value());
    auto * lt = std::get_if<list_t>(& *namesResult);
    ASSERT_NE(lt, nullptr);
    EXPECT_EQ(lt->size, 3u) << "names should have 3 elements after key addition";

    std::vector<std::string> expectedNames{"a", "b", "c"};
    for (size_t i = 0; i < expectedNames.size(); i++) {
        auto childResult = getStoredResult(TraceStore::buildAttrPath({"names", std::to_string(i)}));
        ASSERT_TRUE(childResult.has_value()) << "Child " << i << " should have a stored trace";
        auto * s = std::get_if<string_t>(& *childResult);
        ASSERT_NE(s, nullptr) << "Child " << i << " should be a string";
        EXPECT_EQ(s->first, expectedNames[i]);
    }

    // scalar should still be valid
    auto scalarResult2 = getStoredResult("scalar");
    ASSERT_TRUE(scalarResult2.has_value());
    auto * ival = std::get_if<int_t>(& *scalarResult2);
    ASSERT_NE(ival, nullptr);
    EXPECT_EQ(ival->x.value, 42);
}

TEST_F(MaterializationDepTest, ListOfStrings_OnlyForcedElementsStored)
{
    // A 5-element list of strings; force only indices 1 and 3.
    // Only those two should have stored traces — the others should not.
    TempJsonFile f(R"({"a":"x","b":"y","c":"z","d":"w","e":"v"})");
    auto expr = std::format(
        "let d = {}; in {{ names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
        ASSERT_EQ(namesAttr->value->listSize(), 5u);

        // Force only elements 1 and 3
        state.forceValue(*namesAttr->value->listView()[1], noPos);
        state.forceValue(*namesAttr->value->listView()[3], noPos);
    }

    // The list itself should be stored as list_t with size 5
    auto result = getStoredResult("names");
    ASSERT_TRUE(result.has_value());
    auto * lt = std::get_if<list_t>(& *result);
    ASSERT_NE(lt, nullptr);
    EXPECT_EQ(lt->size, 5u);

    // attrNames returns sorted keys: ["a","b","c","d","e"]
    // Only indices 1 ("b") and 3 ("d") were forced → only they should be stored
    std::vector<std::string> allExpected{"a", "b", "c", "d", "e"};
    for (size_t i = 0; i < 5; i++) {
        auto childPath = TraceStore::buildAttrPath({"names", std::to_string(i)});
        auto childResult = getStoredResult(childPath);
        if (i == 1 || i == 3) {
            ASSERT_TRUE(childResult.has_value()) << "Forced child " << i << " should be stored";
            auto * s = std::get_if<string_t>(& *childResult);
            ASSERT_NE(s, nullptr) << "Child " << i << " should be a string";
            EXPECT_EQ(s->first, allExpected[i]) << "Child " << i << " value mismatch";
        } else {
            EXPECT_FALSE(childResult.has_value())
                << "Unforced child " << i << " should NOT have a stored trace";
        }
    }
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
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
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
