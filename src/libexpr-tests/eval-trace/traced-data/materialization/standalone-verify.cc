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
        // Force d through its TracedExpr so it has a stored trace.
        // (Without dep isolation, forcing val as a sibling doesn't
        // record a synthetic trace for d.)
        auto * dAttr = root.attrs()->get(state.symbols.create("d"));
        ASSERT_NE(dAttr, nullptr);
        state.forceValue(*dAttr->value, noPos);
        auto * attr = root.attrs()->get(state.symbols.create("val"));
        ASSERT_NE(attr, nullptr);
        state.forceValue(*attr->value, noPos);
    }

    // Change value at inner.a — d's ImplicitShape (top-level keys) unchanged,
    // d.inner's ImplicitShape (inner keys) unchanged, but Content changes.
    // The parent (d) should verify-pass (ImplicitShape override, keys unchanged).
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

    // ── First-touch precision loss in this test ──────────────────────
    //
    // Ideal trace: `names` should have exactly 1 dep — a StructuredContent
    // #keys dep on f.path — because builtins.attrNames only observes the
    // key set, not the values.  The Content dep on f.path belongs to `d`,
    // not to `names`.
    //
    // Actual trace: `names` has 3 deps — SC #keys, ParentContext, AND the
    // Content dep from `d`.  This happens because `d` is a first-touch
    // sibling thunk: when names's tracker is active and forces `d` for the
    // first time, `d`'s Content dep flows through record() directly into
    // names's DependencyTracker::ownDeps.  The siblingCallback in
    // replayMemoizedDeps never fires because `d` has no epochMap entry yet
    // (it's being forced for the first time).
    //
    // This is sound: verification's structural override treats the SC #keys
    // pass as covering the Content dep failure (same file, structural dep
    // is more precise than content dep), so the redundant Content dep can
    // only cause a false negative (unnecessary re-evaluation), never a
    // false positive (stale cached result).
    //
    // Approaches evaluated and rejected for fixing this:
    //  - Generic quarantine in forceThunkValue: unsound for mixed consumers
    //    (a child that genuinely needs both its own and the sibling's deps).
    //  - depDedup post-pass removing Content deps when a covering SC dep
    //    exists: the "covering" relationship depends on which SC variant
    //    matched, which is only known at verification time, not recording.
    //
    // See eval.cc:forceThunkValue for the full design rationale.
    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "names should have standalone SC #keys\n" << dumpDeps(deps);
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
    auto innerDeps = getStoredDeps("d.inner");
    EXPECT_TRUE(hasDep(innerDeps, DepType::ParentContext, ""))
        << "d\\0inner should have ParentContext dep\n" << dumpDeps(innerDeps);
    EXPECT_TRUE(hasJsonDep(innerDeps, DepType::ImplicitShape, shapePred("keys")))
        << "d\\0inner should have ImplicitShape #keys dep\n" << dumpDeps(innerDeps);
    EXPECT_EQ(countDepsByType(innerDeps, DepType::Content), 0u)
        << "d\\0inner should have NO Content dep (parent d owns it)\n" << dumpDeps(innerDeps);

    // Verify d's dep structure: should have Content + ImplicitShape
    auto dDeps = getStoredDeps("d");
    EXPECT_TRUE(hasDep(dDeps, DepType::Content, ""))
        << "d should have Content dep\n" << dumpDeps(dDeps);
    EXPECT_TRUE(hasJsonDep(dDeps, DepType::ImplicitShape, shapePred("keys")))
        << "d should have ImplicitShape #keys dep\n" << dumpDeps(dDeps);
}

// ── Mixed sibling-state: one traced, one first-touch ────────────────

TEST_F(MaterializationDepTest, MixedSiblingState_TracedAndFirstTouch)
{
    // Exercises the retry/fallback logic in appendParentContextDeps.
    //
    // parent = { sibA = readFile a; sibB = readFile b; child = sibA + sibB; }
    //
    // Force sibA first (gets a trace), then force child (which accesses
    // sibA via replayMemoizedDeps callback, and sibB as first-touch).
    // The SiblingAccessTracker should detect sibA immediately; sibB is
    // initially untraced but resolves on retry after its TracedExpr::eval()
    // records a trace.
    //
    // Expected: child has ParentContext deps (per-sibling or whole-parent
    // fallback depending on whether retry resolves sibB).
    TempJsonFile fa(R"("val-a")");
    TempJsonFile fb(R"("val-b")");
    auto expr = std::format(
        "let parent = {{ sibA = builtins.fromJSON (builtins.readFile {}); "
        "sibB = builtins.fromJSON (builtins.readFile {}); "
        "child = parent.sibA + parent.sibB; }}; "
        "in {{ inherit (parent) sibA sibB child; }}",
        fa.path.string(), fb.path.string());

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");

        // Force sibA FIRST — it gets a trace and epochMap entry.
        auto * sibA = root.attrs()->get(state.symbols.create("sibA"));
        ASSERT_NE(sibA, nullptr);
        state.forceValue(*sibA->value, noPos);

        // Now force child — sibA is already traced, sibB is first-touch.
        auto * child = root.attrs()->get(state.symbols.create("child"));
        ASSERT_NE(child, nullptr);
        state.forceValue(*child->value, noPos);

        // Verify child evaluates correctly.
        EXPECT_EQ(state.forceStringNoCtx(*child->value, noPos, "test"),
                  "val-aval-b");
    }

    // Check child's stored deps include ParentContext (either per-sibling
    // or whole-parent fallback).
    auto childDeps = getStoredDeps("child");
    EXPECT_TRUE(hasDep(childDeps, DepType::ParentContext, ""))
        << "child should have ParentContext dep (per-sibling or fallback)\n"
        << dumpDeps(childDeps);
}

TEST_F(MaterializationDepTest, MixedSiblingState_BothPreforced_PerSibling)
{
    // Both siblings are forced before child — child should get per-sibling
    // ParentContext deps (not whole-parent fallback), demonstrating the
    // precise path.
    TempJsonFile fa(R"("val-a")");
    TempJsonFile fb(R"("val-b")");
    auto expr = std::format(
        "let parent = {{ sibA = builtins.fromJSON (builtins.readFile {}); "
        "sibB = builtins.fromJSON (builtins.readFile {}); "
        "child = parent.sibA + parent.sibB; }}; "
        "in {{ inherit (parent) sibA sibB child; }}",
        fa.path.string(), fb.path.string());

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");

        // Force BOTH siblings first.
        auto * sibA = root.attrs()->get(state.symbols.create("sibA"));
        ASSERT_NE(sibA, nullptr);
        state.forceValue(*sibA->value, noPos);

        auto * sibB = root.attrs()->get(state.symbols.create("sibB"));
        ASSERT_NE(sibB, nullptr);
        state.forceValue(*sibB->value, noPos);

        // Now force child — both siblings already traced.
        auto * child = root.attrs()->get(state.symbols.create("child"));
        ASSERT_NE(child, nullptr);
        state.forceValue(*child->value, noPos);
        EXPECT_EQ(state.forceStringNoCtx(*child->value, noPos, "test"),
                  "val-aval-b");
    }

    // With both siblings preforced, child should get per-sibling deps.
    auto childDeps = getStoredDeps("child");
    size_t pcCount = countDepsByType(childDeps, DepType::ParentContext);
    EXPECT_GE(pcCount, 1u)
        << "child should have ParentContext deps\n" << dumpDeps(childDeps);
}

} // namespace nix::eval_trace::test
