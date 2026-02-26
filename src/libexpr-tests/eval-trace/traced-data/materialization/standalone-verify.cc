/**
 * Tests for standalone dep verification — the class of bug where
 * child traces have ImplicitShape/SC deps but no Content dep of
 * their own, relying on the parent's Content dep.
 *
 * This exercises the fix in verifyTrace() that checks standalone
 * ImplicitShape deps in the !hasContentFailure path.
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── Standalone ImplicitShape: nested key set change detected ────────

TEST_F(MaterializationDepTest, StandaloneIS_NestedKeyChange_Invalidates)
{
    TempJsonFile f(R"({"inner":{"a":1}})");
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
        ASSERT_EQ(attr->value->listSize(), 1u);
    }

    f.modify(R"({"inner":{"a":1,"b":2}})");
    invalidateFileCache(f.path);

    // d\0inner should fail verify (standalone ImplicitShape #keys changed)
    auto innerResult = getStoredResult(makePath({"d", "inner"}));
    EXPECT_FALSE(innerResult.has_value())
        << "d\\0inner should fail verify after nested key change\n"
        << "(standalone ImplicitShape should detect key set change)";
}

TEST_F(MaterializationDepTest, StandaloneIS_NestedValueChange_Survives)
{
    TempJsonFile f(R"({"inner":{"a":1}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; val = d.inner.a; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
    }

    // Change value at inner.a — d's ImplicitShape (top-level keys) unchanged,
    // d.inner's ImplicitShape (inner keys) unchanged, but Content changes.
    // The parent (d) should verify-pass (ImplicitShape override, keys unchanged).
    // d.inner should verify: its ImplicitShape #keys is {"a"}, which is unchanged.
    // But d.inner.a is a leaf — its value changed. The leaf's trace has different
    // deps than the parent's.
    f.modify(R"({"inner":{"a":99}})");
    invalidateFileCache(f.path);

    // d should still verify (top-level keys unchanged)
    auto dResult = getStoredResult("d");
    EXPECT_TRUE(dResult.has_value())
        << "d should still verify (top-level keys unchanged)";
}

// ── Standalone ImplicitShape: top-level key set UNCHANGED, nested changed ──

TEST_F(MaterializationDepTest, StandaloneIS_TopUnchangedNestedChanged)
{
    TempJsonFile f(R"({"x":{"a":1},"y":{"b":2}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; xkeys = builtins.attrNames d.x; ykeys = builtins.attrNames d.y; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * xk = root.attrs()->get(state.symbols.create("xkeys"));
        ASSERT_NE(xk, nullptr);
        state.forceValue(*xk->value, noPos);
        ASSERT_EQ(xk->value->listSize(), 1u); // ["a"]
        auto * yk = root.attrs()->get(state.symbols.create("ykeys"));
        ASSERT_NE(yk, nullptr);
        state.forceValue(*yk->value, noPos);
        ASSERT_EQ(yk->value->listSize(), 1u); // ["b"]
    }

    // Top-level keys {x, y} unchanged, but x's inner keys change
    f.modify(R"({"x":{"a":1,"c":3},"y":{"b":2}})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");

        auto * xk = root.attrs()->get(state.symbols.create("xkeys"));
        ASSERT_NE(xk, nullptr);
        state.forceValue(*xk->value, noPos);
        EXPECT_EQ(xk->value->listSize(), 2u)
            << "xkeys should detect key change in x (a, c)";

        auto * yk = root.attrs()->get(state.symbols.create("ykeys"));
        ASSERT_NE(yk, nullptr);
        state.forceValue(*yk->value, noPos);
        EXPECT_EQ(yk->value->listSize(), 1u)
            << "ykeys should remain unchanged (b)";
    }
}

// ── Standalone SC deps: cross-scope without Content dep ─────────────

TEST_F(MaterializationDepTest, StandaloneSC_CrossScope_KeyChange)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
    }

    // Verify names has SC #keys as a standalone dep (no Content in names' trace)
    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "names should have standalone SC #keys\n" << dumpDeps(deps);
    EXPECT_EQ(countDepsByType(deps, DepType::Content), 0u)
        << "names should have NO Content dep (parent d owns it)\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, DepType::ParentContext, ""))
        << "names should have ParentContext dep on d\n" << dumpDeps(deps);
}

// ── Three-level standalone: each level has standalone ImplicitShape ──

TEST_F(MaterializationDepTest, ThreeLevelStandalone_DeepKeyChange)
{
    TempJsonFile f(R"({"a":{"b":{"c":1}}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; t = builtins.typeOf d.a.b; }}",
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

    // Change d.a.b from object to array — typeOf should detect this
    f.modify(R"({"a":{"b":[1,2,3]}})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * attr = root.attrs()->get(state.symbols.create("t"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
        EXPECT_EQ(std::string(attr->value->c_str()), "list")
            << "Three-level: typeOf should detect type change from set to list";
    }
}

// ── Dep dump for d\0inner showing exact standalone dep structure ─────

TEST_F(MaterializationDepTest, StandaloneIS_DepStructure)
{
    TempJsonFile f(R"({"inner":{"x":1}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d.inner; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        // Force d and d.inner to populate their traces
        auto * dAttr = root.attrs()->get(state.symbols.create("d"));
        ASSERT_NE(dAttr, nullptr);
        state.forceValue(*dAttr->value, noPos);
        if (dAttr->value->type() == nAttrs) {
            auto * innerAttr = dAttr->value->attrs()->get(state.symbols.create("inner"));
            if (innerAttr) state.forceValue(*innerAttr->value, noPos);
        }
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    // Verify d\0inner's dep structure: should have ParentContext + ImplicitShape, NO Content
    auto innerDeps = getStoredDeps(makePath({"d", "inner"}));
    EXPECT_TRUE(hasDep(innerDeps, DepType::ParentContext, ""))
        << "d\\0inner should have ParentContext dep\n" << dumpDeps(innerDeps);
    EXPECT_TRUE(hasDep(innerDeps, DepType::ImplicitShape, "#keys"))
        << "d\\0inner should have ImplicitShape #keys dep\n" << dumpDeps(innerDeps);
    EXPECT_EQ(countDepsByType(innerDeps, DepType::Content), 0u)
        << "d\\0inner should have NO Content dep (parent d owns it)\n" << dumpDeps(innerDeps);

    // Verify d's dep structure: should have Content + ImplicitShape
    auto dDeps = getStoredDeps("d");
    EXPECT_TRUE(hasDep(dDeps, DepType::Content, ""))
        << "d should have Content dep\n" << dumpDeps(dDeps);
    EXPECT_TRUE(hasDep(dDeps, DepType::ImplicitShape, "#keys"))
        << "d should have ImplicitShape #keys dep\n" << dumpDeps(dDeps);
}

} // namespace nix::eval_trace::test
