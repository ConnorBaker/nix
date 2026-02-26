/**
 * Cross-scope materialization tests for typeOf / SC #type.
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

TEST_F(MaterializationDepTest, CrossScope_TypeOf_Attrs_RecordsSCType)
{
    TempJsonFile f(R"({"a":1})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; t = builtins.typeOf d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * tAttr = root.attrs()->get(state.symbols.create("t"));
        ASSERT_NE(tAttr, nullptr);
        state.forceValue(*tAttr->value, noPos);
    }

    auto deps = getStoredDeps("t");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#type"))
        << "Cross-scope typeOf: SC #type should be recorded\n"
        << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_TypeOf_Nested)
{
    TempJsonFile f(R"({"inner":{"x":1}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; t = builtins.typeOf d.inner; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * tAttr = root.attrs()->get(state.symbols.create("t"));
        ASSERT_NE(tAttr, nullptr);
        state.forceValue(*tAttr->value, noPos);
    }

    auto deps = getStoredDeps("t");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#type"))
        << "Cross-scope nested typeOf: SC #type should be recorded\n"
        << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_TypeOf_NoSCKeys)
{
    TempJsonFile f(R"({"a":1})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; t = builtins.typeOf d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * tAttr = root.attrs()->get(state.symbols.create("t"));
        ASSERT_NE(tAttr, nullptr);
        state.forceValue(*tAttr->value, noPos);
    }

    auto deps = getStoredDeps("t");
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "typeOf should NOT record SC #keys\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_TypeOf_TypeChange_CacheMiss)
{
    TempJsonFile f(R"({"a":1})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; t = builtins.typeOf d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * tAttr = root.attrs()->get(state.symbols.create("t"));
        ASSERT_NE(tAttr, nullptr);
        state.forceValue(*tAttr->value, noPos);
        EXPECT_EQ(std::string(tAttr->value->c_str()), "set");
    }

    // Add keys — type stays "set" but the attrset changes
    f.modify(R"({"a":1,"b":2})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * tAttr = root.attrs()->get(state.symbols.create("t"));
        ASSERT_NE(tAttr, nullptr);
        state.forceValue(*tAttr->value, noPos);
        EXPECT_EQ(std::string(tAttr->value->c_str()), "set")
            << "typeOf should still be 'set' after key addition";
    }
}

} // namespace nix::eval_trace::test
