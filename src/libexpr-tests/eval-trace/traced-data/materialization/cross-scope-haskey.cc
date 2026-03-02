/**
 * Cross-scope materialization tests for hasAttr / SC #has:key.
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

TEST_F(MaterializationDepTest, CrossScope_HasAttr_Exists)
{
    TempJsonFile f(R"({"x":1,"y":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; has = d ? x; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * hasAttr = root.attrs()->get(state.symbols.create("has"));
        ASSERT_NE(hasAttr, nullptr);
        state.forceValue(*hasAttr->value, noPos);
    }

    auto deps = getStoredDeps("has");
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
        << "Cross-scope hasAttr exists: SC #has:x should be recorded\n"
        << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_HasAttr_Missing)
{
    TempJsonFile f(R"({"x":1,"y":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; has = d ? z; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * hasAttr = root.attrs()->get(state.symbols.create("has"));
        ASSERT_NE(hasAttr, nullptr);
        state.forceValue(*hasAttr->value, noPos);
    }

    auto deps = getStoredDeps("has");
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("z")))
        << "Cross-scope hasAttr missing: SC #has:z should be recorded\n"
        << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_HasAttr_OrDefault)
{
    TempJsonFile f(R"({"x":1})");
    // Use `if d ? x then d.x else 0` which explicitly calls hasAttr
    auto expr = std::format(
        "let d = {}; in {{ inherit d; val = if d ? x then d.x else 0; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * valAttr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(valAttr, nullptr);
        state.forceValue(*valAttr->value, noPos);
    }

    auto deps = getStoredDeps("val");
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
        << "Cross-scope hasAttr + select: SC #has:x should be recorded\n"
        << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_HasAttr_TOML)
{
    TempTomlFile f("x = 1\ny = 2\n");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; has = d ? x; }}",
        ft(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * hasAttr = root.attrs()->get(state.symbols.create("has"));
        ASSERT_NE(hasAttr, nullptr);
        state.forceValue(*hasAttr->value, noPos);
    }

    auto deps = getStoredDeps("has");
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("x")))
        << "Cross-scope TOML hasAttr: SC #has:x should be recorded\n"
        << dumpDeps(deps);
}

// ── Negative tests ─────────────────────────────────────────────────

TEST_F(MaterializationDepTest, CrossScope_HasAttr_NoSCKeys)
{
    TempJsonFile f(R"({"x":1,"y":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; has = d ? x; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * hasAttr = root.attrs()->get(state.symbols.create("has"));
        ASSERT_NE(hasAttr, nullptr);
        state.forceValue(*hasAttr->value, noPos);
    }

    auto deps = getStoredDeps("has");
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "hasAttr should NOT record SC #keys\n" << dumpDeps(deps);
}

TEST_F(MaterializationDepTest, CrossScope_HasAttr_NoSCType)
{
    TempJsonFile f(R"({"x":1,"y":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; has = d ? x; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * hasAttr = root.attrs()->get(state.symbols.create("has"));
        ASSERT_NE(hasAttr, nullptr);
        state.forceValue(*hasAttr->value, noPos);
    }

    auto deps = getStoredDeps("has");
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
        << "hasAttr should NOT record SC #type\n" << dumpDeps(deps);
}

// ── Cache invalidation tests ───────────────────────────────────────

TEST_F(MaterializationDepTest, CrossScope_HasAttr_KeyRemoved_CacheMiss)
{
    TempJsonFile f(R"({"x":1,"y":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; has = d ? x; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * hasAttr = root.attrs()->get(state.symbols.create("has"));
        ASSERT_NE(hasAttr, nullptr);
        state.forceValue(*hasAttr->value, noPos);
        EXPECT_TRUE(hasAttr->value->boolean());
    }

    // Remove the key
    f.modify(R"({"y":2})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * hasAttr = root.attrs()->get(state.symbols.create("has"));
        ASSERT_NE(hasAttr, nullptr);
        state.forceValue(*hasAttr->value, noPos);
        EXPECT_FALSE(hasAttr->value->boolean()) << "After key removal, hasAttr should return false";
    }
}

TEST_F(MaterializationDepTest, CrossScope_HasAttr_UnrelatedChange_CacheHit)
{
    TempJsonFile f(R"({"x":1,"y":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; has = d ? x; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * hasAttr = root.attrs()->get(state.symbols.create("has"));
        ASSERT_NE(hasAttr, nullptr);
        state.forceValue(*hasAttr->value, noPos);
    }

    // Change value of 'x' but keep the key
    f.modify(R"({"x":99,"y":2})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * hasAttr = root.attrs()->get(state.symbols.create("has"));
        ASSERT_NE(hasAttr, nullptr);
        state.forceValue(*hasAttr->value, noPos);
        EXPECT_TRUE(hasAttr->value->boolean()) << "Key still exists after value change";
    }
}

} // namespace nix::eval_trace::test
