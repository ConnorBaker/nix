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

TEST_F(MaterializationDepTest, StandaloneIS_TopUnchanged_NestedChanged)
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

    // Top-level keys {x, y} unchanged, but x's inner keys change.
    // d's ImplicitShape on the top-level key set is still valid, but
    // d.x's ImplicitShape on {a} is NOT — so xkeys must re-evaluate.
    // ykeys (d.y, untouched) must stay a cache hit.
    f.modify(R"({"x":{"a":1,"c":3},"y":{"b":2}})");
    invalidateFileCache(f.path);

    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
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

        // Soundness: the loader must have re-run because d.x's key
        // set changed. A regression where the cache served the stale
        // xkeys value from the cold recording would still leave the
        // listSize assertions above wrong, but the loaderCalls check
        // catches the complementary failure mode where the cache is
        // silently bypassed every time (listSize passes because of
        // re-eval on every call, not because the cache detected the
        // mutation and retried).
        EXPECT_EQ(calls, 1)
            << "loader must re-run: d.x's ImplicitShape key set changed";
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

    // First-touch still leaks concrete deps from `d` into `names`, and we keep
    // a coarse stamped parent-slot fallback until the finer slot-stamp cutover
    // lands.
    auto deps = getStoredDeps("names");
    EXPECT_TRUE(hasJsonDep(deps, CanonicalQueryKind::StructuredProjection, shapePred("keys")))
        << "names should have standalone SC #keys\n" << dumpDeps(deps);
    EXPECT_TRUE(hasDep(deps, CanonicalQueryKind::TraceParentSlot, ""))
        << "names should retain coarse fallback until value-context slot verification lands\n"
        << dumpDeps(deps);
}

// ── Three-level standalone: each level has standalone ImplicitShape ──

TEST_F(MaterializationDepTest, ThreeLevelStandalone_TypeOf_DeepKeyChange)
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

TEST_F(MaterializationDepTest, StandaloneIS_DInner_DepStructure)
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

    // In the current first-touch path, d\0inner keeps its own standalone dep
    // set via implicit-shape plus coarse stamped parent-slot fallback.
    auto innerDeps = getStoredDeps("d.inner");
    EXPECT_TRUE(hasJsonDep(innerDeps, CanonicalQueryKind::ImplicitStructure, shapePred("keys")))
        << "d\\0inner should keep standalone implicit-shape coverage\n"
        << dumpDeps(innerDeps);
    EXPECT_TRUE(hasDep(innerDeps, CanonicalQueryKind::TraceParentSlot, ""))
        << "d\\0inner should retain coarse fallback in the current first-touch path\n"
        << dumpDeps(innerDeps);

    // Verify d's dep structure: should have Content + ImplicitShape
    auto dDeps = getStoredDeps("d");
    EXPECT_TRUE(hasDep(dDeps, CanonicalQueryKind::FileBytes, ""))
        << "d should have Content dep\n" << dumpDeps(dDeps);
    EXPECT_TRUE(hasJsonDep(dDeps, CanonicalQueryKind::ImplicitStructure, shapePred("keys")))
        << "d should have ImplicitShape #keys dep\n" << dumpDeps(dDeps);
}

// ── Mixed sibling-state: one traced, one first-touch ────────────────

TEST_F(MaterializationDepTest, MixedSiblingState_Traced_AndFirstTouch)
{
    // Exercises mixed sibling state with replay-only sibling tracking.
    //
    // parent = { sibA = readFile a; sibB = readFile b; child = sibA + sibB; }
    //
    // Force sibA first (gets a trace), then force child (which accesses
    // sibA via replayMemoizedDeps callback, and sibB as first-touch).
    // Concrete deps from ordinary replay do not replace the coarse stamped fallback
    // fallback yet in this mixed case.
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

    // Check child's stored deps still keep the coarse stamped fallback in
    // this mixed first-touch case.
    auto childDeps = getStoredDeps("child");
    EXPECT_EQ(countDepsByType(childDeps, CanonicalQueryKind::FileBytes), 2u)
        << "child should keep the two concrete content deps\n" << dumpDeps(childDeps);
    EXPECT_TRUE(hasDep(childDeps, CanonicalQueryKind::TraceParentSlot, ""))
        << "child should retain coarse fallback until value-context slot verification lands\n"
        << dumpDeps(childDeps);
}

TEST_F(MaterializationDepTest, MixedSiblingState_BothPreforced_PerSibling)
{
    // Both siblings are forced before child. Fresh production writes no longer
    // have a legacy ValueContext/ParentSlot escape hatch; if replay capture
    // lands here it must emit stamped precise deps, otherwise the current
    // first-touch path still records concrete deps plus coarse stamped fallback.
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

    // Even with both siblings prefetched, the current path keeps the stamped
    // coarse fallback until slot stamps/current-state deletion land.
    auto childDeps = getStoredDeps("child");
    EXPECT_EQ(countDepsByType(childDeps, CanonicalQueryKind::FileBytes), 2u)
        << "child should keep the two concrete content deps\n" << dumpDeps(childDeps);
    EXPECT_EQ(countDepsByType(childDeps, CanonicalQueryKind::TraceParentSlot), 1u)
        << "child should retain exactly one coarse fallback\n"
        << dumpDeps(childDeps);
}

} // namespace nix::eval_trace::test
