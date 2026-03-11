#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ═══════════════════════════════════════════════════════════════════════
// Pointer equality tests for eval-trace materialization.
//
// The bug: when an attrset (or list) containing functions is accessed
// through TWO DIFFERENT TracedExpr materialization paths, each path
// creates fresh Value*/Bindings* objects. The `==` operator's pointer
// fast-paths (&v1 == &v2, v1.attrs() == v2.attrs()) both fail, causing
// element-wise recursion that hits function comparison (always false).
//
// This manifests in nixpkgs as:
//   pkgs.stdenv.hostPlatform == pkgs.stdenv.targetPlatform → false
// when both point to the same object but are materialized through
// different trace paths.
//
// CRITICAL TESTING PATTERN — "via copy" tests:
//
// The Nix evaluator's `ExprOpEq::eval` creates stack-local Values and
// `ExprSelect::eval` copies the Bindings Value into them (`v = *vAttrs`
// at ExprSelect::eval). This means `eqValues` receives Values at DIFFERENT
// addresses than the originals in siblingIdentityMap. Tests that call
// `eqValues(*attr->value, ...)` directly use the ORIGINAL Bindings
// pointers — these pass trivially via siblingIdentityMap lookup.
//
// To catch the real ExprOpEq bug, we MUST test with VALUE COPIES:
//   Value copy1 = *aAttr->value;   // simulates ExprSelect copy
//   state.eqValues(copy1, copy2, ...)
//
// Tests suffixed "_ViaCopy" exercise this path. They would have caught
// the original regression where haveSameResolvedTarget failed because
// the Value* copies were not found in siblingIdentityMap.
// ═══════════════════════════════════════════════════════════════════════

// ── Attrset pointer equality (cross-path materialization) ────────────

TEST_F(TracedDataTest, PointerEquality_AttrsWithFunction_CrossPath)
{
    // Cache: { a = platform; b = platform; } where platform has a function.
    // Force root.a and root.b through separate TracedExpr children, then
    // compare them with eqValues. This is the nixpkgs pattern.
    auto expr = R"(
        let platform = { f = y: y; system = "x86_64-linux"; };
        in { a = platform; b = platform; }
    )";

    // Cold run: record traces
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        ASSERT_EQ(root.type(), nAttrs);

        auto * aAttr = root.attrs()->get(state.symbols.create("a"));
        auto * bAttr = root.attrs()->get(state.symbols.create("b"));
        ASSERT_NE(aAttr, nullptr);
        ASSERT_NE(bAttr, nullptr);

        state.forceValue(*aAttr->value, noPos);
        state.forceValue(*bAttr->value, noPos);

        EXPECT_TRUE(state.eqValues(*aAttr->value, *bAttr->value, noPos,
            "pointer equality: cold run, same platform via different paths"))
            << "Cold run: a and b should be equal (same platform object)";
    }

    // Hot run: from cache — each child materializes independently
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        ASSERT_EQ(root.type(), nAttrs);

        auto * aAttr = root.attrs()->get(state.symbols.create("a"));
        auto * bAttr = root.attrs()->get(state.symbols.create("b"));
        ASSERT_NE(aAttr, nullptr);
        ASSERT_NE(bAttr, nullptr);

        state.forceValue(*aAttr->value, noPos);
        state.forceValue(*bAttr->value, noPos);

        EXPECT_TRUE(state.eqValues(*aAttr->value, *bAttr->value, noPos,
            "pointer equality: hot run, same platform via different paths"))
            << "Hot run: a and b should be equal (same platform object from cache)";
    }
}

TEST_F(TracedDataTest, PointerEquality_AttrsWithFunction_CrossPath_Negative)
{
    // Negative case: two DIFFERENT attrsets with functions.
    // Should be false regardless of tracing.
    auto expr = R"(
        let p1 = { f = y: y; system = "x86_64-linux"; };
            p2 = { f = y: y; system = "x86_64-linux"; };
        in { a = p1; b = p2; }
    )";

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        EXPECT_FALSE(state.eqValues(*a->value, *b->value, noPos,
            "different attrsets with functions"))
            << "Different attrsets with functions must not be equal";
    }

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        EXPECT_FALSE(state.eqValues(*a->value, *b->value, noPos,
            "different attrsets with functions from cache"))
            << "Different attrsets with functions must not be equal from cache";
    }
}

TEST_F(TracedDataTest, PointerEquality_AttrsNoFunction_CrossPath)
{
    // Baseline: same attrset WITHOUT functions. Element-wise comparison
    // should succeed even without pointer fast-path.
    auto expr = R"(
        let x = { a = 1; b = "hello"; };
        in { left = x; right = x; }
    )";

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * l = root.attrs()->get(state.symbols.create("left"));
        auto * r = root.attrs()->get(state.symbols.create("right"));
        state.forceValue(*l->value, noPos);
        state.forceValue(*r->value, noPos);

        EXPECT_TRUE(state.eqValues(*l->value, *r->value, noPos,
            "same attrset without functions"))
            << "Same attrset without functions should be equal";
    }

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * l = root.attrs()->get(state.symbols.create("left"));
        auto * r = root.attrs()->get(state.symbols.create("right"));
        state.forceValue(*l->value, noPos);
        state.forceValue(*r->value, noPos);

        EXPECT_TRUE(state.eqValues(*l->value, *r->value, noPos,
            "same attrset without functions from cache"))
            << "Same attrset without functions from cache should be equal";
    }
}

// ── Nixpkgs-like platform pattern ────────────────────────────────────

TEST_F(TracedDataTest, PointerEquality_PlatformPattern_CrossPath)
{
    // Simulates: stdenv.hostPlatform == stdenv.targetPlatform
    // Both aliases point to the same platform object.
    auto expr = R"(
        let platform = {
            system = "x86_64-linux";
            isLinux = true;
            canExecute = other: other.system == "x86_64-linux";
        };
        stdenv = {
            hostPlatform = platform;
            targetPlatform = platform;
        };
        in stdenv
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        ASSERT_EQ(root.type(), nAttrs) << label;

        auto * host = root.attrs()->get(state.symbols.create("hostPlatform"));
        auto * target = root.attrs()->get(state.symbols.create("targetPlatform"));
        ASSERT_NE(host, nullptr) << label;
        ASSERT_NE(target, nullptr) << label;

        state.forceValue(*host->value, noPos);
        state.forceValue(*target->value, noPos);

        EXPECT_TRUE(state.eqValues(*host->value, *target->value, noPos,
            "hostPlatform == targetPlatform"))
            << label << ": hostPlatform == targetPlatform should be true";
    };

    doTest("cold run", nullptr);

    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

// ── List pointer equality (cross-path materialization) ───────────────

TEST_F(TracedDataTest, PointerEquality_ListWithFunction_CrossPath)
{
    // Cache: { a = xs; b = xs; } where xs is a list with a function.
    // Force root.a and root.b, compare with eqValues.
    auto expr = R"(
        let xs = [ (y: y) 1 2 ];
        in { a = xs; b = xs; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        EXPECT_TRUE(state.eqValues(*a->value, *b->value, noPos,
            "same list with function via different paths"))
            << label << ": same list object should be equal";
    };

    doTest("cold run", nullptr);

    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_ListWithFunction_CrossPath_Negative)
{
    // Negative: two different lists with functions.
    auto expr = R"(
        let a = [ (y: y) 1 ];
            b = [ (y: y) 1 ];
        in { left = a; right = b; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * l = root.attrs()->get(state.symbols.create("left"));
        auto * r = root.attrs()->get(state.symbols.create("right"));
        state.forceValue(*l->value, noPos);
        state.forceValue(*r->value, noPos);

        EXPECT_FALSE(state.eqValues(*l->value, *r->value, noPos,
            "different lists with functions"))
            << label << ": different lists must not be equal";
    };

    doTest("cold run", nullptr);

    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_ListNoFunction_CrossPath)
{
    // Baseline: same list without functions.
    auto expr = R"(
        let xs = [ 1 2 3 ];
        in { a = xs; b = xs; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        EXPECT_TRUE(state.eqValues(*a->value, *b->value, noPos,
            "same list without functions"))
            << label << ": same list without functions should be equal";
    };

    doTest("cold run", nullptr);

    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

// ── SmallList (0-2 elements) cross-path ──────────────────────────────

TEST_F(TracedDataTest, PointerEquality_SmallListWithFunction_CrossPath)
{
    // SmallList (<=2 elements) uses inline storage — no heap pointer to share.
    auto expr = R"(
        let xs = [ (y: y) ];
        in { a = xs; b = xs; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        EXPECT_TRUE(state.eqValues(*a->value, *b->value, noPos,
            "small list with function"))
            << label << ": same small list should be equal";
    };

    doTest("cold run", nullptr);

    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_SmallListTwoFunctions_CrossPath)
{
    auto expr = R"(
        let xs = [ (y: y) (z: z + 1) ];
        in { a = xs; b = xs; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        EXPECT_TRUE(state.eqValues(*a->value, *b->value, noPos,
            "small list with two functions"))
            << label << ": same 2-elem list with functions should be equal";
    };

    doTest("cold run", nullptr);

    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

// ── Nested containers ────────────────────────────────────────────────

TEST_F(TracedDataTest, PointerEquality_NestedAttrsInList_CrossPath)
{
    // List containing attrset with function, accessed via two paths.
    auto expr = R"(
        let xs = [ { f = y: y; val = 1; } "hello" ];
        in { a = xs; b = xs; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        EXPECT_TRUE(state.eqValues(*a->value, *b->value, noPos,
            "list of attrset-with-function"))
            << label << ": same nested structure should be equal";
    };

    doTest("cold run", nullptr);

    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

// ── In-expression comparison (baseline — thunk memoization works) ────

TEST_F(TracedDataTest, PointerEquality_InExpr_AttrsWithFunction)
{
    // When == happens inside the cached expression, thunk memoization
    // ensures both sides resolve to the same Value*. This should always
    // pass (it's the baseline, not the bug).
    auto expr = R"(
        let platform = { f = y: y; system = "x86_64-linux"; };
            stdenv = { hostPlatform = platform; targetPlatform = platform; };
        in stdenv.hostPlatform == stdenv.targetPlatform
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        EXPECT_TRUE(v.boolean()) << "in-expression comparison should work (cold)";
    }

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        EXPECT_TRUE(v.boolean()) << "in-expression comparison should work (hot)";
    }
}

TEST_F(TracedDataTest, PointerEquality_InExpr_ListWithFunction)
{
    auto expr = R"(let x = [ (y: y) 1 ]; in x == x)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        EXPECT_TRUE(v.boolean()) << "in-expression list == should work (cold)";
    }

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        EXPECT_TRUE(v.boolean()) << "in-expression list == should work (hot)";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Three-tier haveSameResolvedTarget coverage
//
// Tier 1: Same parent + same canonicalSiblingIdx → zero rootLoader
// Tier 2: Cold-cached resolvedTarget (set by evaluatePhase2) → pointer compare
// Tier 3: Fallback via getResolvedTarget → navigateToReal → rootLoader
// ═══════════════════════════════════════════════════════════════════════

// ── Tier 1: Same-parent attrs aliases (hot path, zero rootLoader) ────

TEST_F(TracedDataTest, PointerEquality_Tier1_AttrsAlias_HotNoRootLoader)
{
    // Two attrs of the same parent aliased to the same Value*.
    // On hot path, Tier 1 detects this without rootLoader.
    auto expr = R"(
        let platform = { f = y: y; system = "x86_64-linux"; };
        in { a = platform; b = platform; }
    )";

    // Cold run: populate traces (alias detected in buildCachedResult)
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        EXPECT_TRUE(state.eqValues(*a->value, *b->value, noPos,
            "cold run baseline"));
    }

    // Hot run: Tier 1 — rootLoader must NOT be called for alias detection
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        ASSERT_EQ(root.type(), nAttrs);

        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);

        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        int callsBefore = loaderCalls;

        EXPECT_TRUE(state.eqValues(*a->value, *b->value, noPos,
            "hot: tier 1 parent-level alias"))
            << "Hot run: a and b should be equal via Tier 1 alias detection";

        EXPECT_EQ(loaderCalls, callsBefore)
            << "Tier 1: rootLoader must not be called for same-parent alias detection";
    }
}

TEST_F(TracedDataTest, PointerEquality_Tier1_ThreeWayAlias_HotNoRootLoader)
{
    // Three attrs aliased to the same Value*. All pairs should match via Tier 1.
    auto expr = R"(
        let platform = { f = y: y; system = "x86_64-linux"; };
        in { a = platform; b = platform; c = platform; }
    )";

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        for (auto name : {"a", "b", "c"}) {
            auto * attr = root.attrs()->get(state.symbols.create(name));
            state.forceValue(*attr->value, noPos);
        }
    }

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        auto * c = root.attrs()->get(state.symbols.create("c"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        state.forceValue(*c->value, noPos);

        int callsBefore = loaderCalls;

        EXPECT_TRUE(state.eqValues(*a->value, *b->value, noPos, "a==b"));
        EXPECT_TRUE(state.eqValues(*b->value, *c->value, noPos, "b==c"));
        EXPECT_TRUE(state.eqValues(*a->value, *c->value, noPos, "a==c"));

        EXPECT_EQ(loaderCalls, callsBefore)
            << "Tier 1: three-way alias must not trigger rootLoader";
    }
}

TEST_F(TracedDataTest, PointerEquality_Tier1_ListElementAlias_HotNoRootLoader)
{
    // Within-list aliases: same element appears twice at different indices.
    // Tier 1 detects both elements point to the same canonical index.
    auto expr = R"(
        let item = { f = y: y; val = 42; };
        in [ item item ]
    )";

    // Cold run
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        ASSERT_EQ(root.type(), nList);
        ASSERT_EQ(root.listSize(), 2u);
        auto view = root.listView();
        state.forceValue(*view[0], noPos);
        state.forceValue(*view[1], noPos);
        EXPECT_TRUE(state.eqValues(*view[0], *view[1], noPos,
            "cold: within-list aliases"));
    }

    // Hot run: Tier 1
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        ASSERT_EQ(root.type(), nList);
        auto view = root.listView();
        state.forceValue(*view[0], noPos);
        state.forceValue(*view[1], noPos);

        int callsBefore = loaderCalls;

        EXPECT_TRUE(state.eqValues(*view[0], *view[1], noPos,
            "hot: tier 1 list element alias"))
            << "Hot run: same list elements should be equal via Tier 1";

        EXPECT_EQ(loaderCalls, callsBefore)
            << "Tier 1: rootLoader must not be called for within-list alias detection";
    }
}

// ── Tier 1 negative: different values share parent but different index ──

TEST_F(TracedDataTest, PointerEquality_Tier1_Negative_DifferentAttrs)
{
    // Two different values in the same parent — different canonical indices.
    // Must return false (not aliased).
    auto expr = R"(
        let p1 = { f = y: y; system = "x86_64-linux"; };
            p2 = { f = y: y; system = "aarch64-linux"; };
        in { a = p1; b = p2; }
    )";

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        EXPECT_FALSE(state.eqValues(*a->value, *b->value, noPos,
            "cold: different values"));
    }

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        EXPECT_FALSE(state.eqValues(*a->value, *b->value, noPos,
            "hot: different values, not aliased"));
    }
}

// ── Tier 2/3: Cross-parent aliases (Bindings* comparison) ────────────

// ── Tier 3: Cross-parent hot path (navigates, triggers rootLoader) ───

TEST_F(TracedDataTest, PointerEquality_Tier3_CrossParent_HotFallback)
{
    // Cross-parent aliases on hot path: Tier 1 fails (different parents),
    // Tier 2 fails (resolvedTarget not set on hot path), Tier 3 fires.
    // After navigation, the resolved targets share the same Bindings*
    // (both thunks evaluate to the same platform attrset), so
    // resolvedTargetsMatch detects the alias.
    auto expr = R"(
        let platform = { f = y: y; };
        in { stdenv = { host = platform; }; other = { host = platform; }; }
    )";

    // Cold run to populate traces
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * stdenv = root.attrs()->get(state.symbols.create("stdenv"));
        auto * other = root.attrs()->get(state.symbols.create("other"));
        state.forceValue(*stdenv->value, noPos);
        state.forceValue(*other->value, noPos);
        auto * h1 = stdenv->value->attrs()->get(state.symbols.create("host"));
        auto * h2 = other->value->attrs()->get(state.symbols.create("host"));
        state.forceValue(*h1->value, noPos);
        state.forceValue(*h2->value, noPos);
        EXPECT_TRUE(state.eqValues(*h1->value, *h2->value, noPos,
            "cold: cross-parent via Tier 2 Bindings match"));
    }

    // Hot run: Tier 3 fallback — rootLoader WILL be called, but result is correct
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * stdenv = root.attrs()->get(state.symbols.create("stdenv"));
        auto * other = root.attrs()->get(state.symbols.create("other"));
        state.forceValue(*stdenv->value, noPos);
        state.forceValue(*other->value, noPos);

        auto * h1 = stdenv->value->attrs()->get(state.symbols.create("host"));
        auto * h2 = other->value->attrs()->get(state.symbols.create("host"));
        state.forceValue(*h1->value, noPos);
        state.forceValue(*h2->value, noPos);

        EXPECT_TRUE(state.eqValues(*h1->value, *h2->value, noPos,
            "hot: cross-parent via Tier 3 Bindings match"))
            << "Cross-parent aliases should match via Tier 3 Bindings comparison";
    }
}

// ── No aliases: aliasOf vector stays empty ───────────────────────────

TEST_F(TracedDataTest, PointerEquality_NoAliases_EmptyAliasOf)
{
    // When no attrs share the same Value*, aliasOf should be empty
    // (not a vector of all -1s). Verify by checking no Tier 1 match
    // and that comparison still works correctly.
    auto expr = R"(
        { a = 1; b = 2; c = 3; }
    )";

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        ASSERT_EQ(root.type(), nAttrs);
    }

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        EXPECT_FALSE(state.eqValues(*a->value, *b->value, noPos,
            "different int values"));
    }
}

// ── Mixed: some attrs aliased, some not ──────────────────────────────

TEST_F(TracedDataTest, PointerEquality_Tier1_MixedAliasAndUnique)
{
    // a and c are aliased to the same value; b is different.
    // a==c should pass via Tier 1 (zero rootLoader), a==b should fail.
    auto expr = R"(
        let shared = { f = y: y; val = 1; };
            unique = { f = y: y; val = 2; };
        in { a = shared; b = unique; c = shared; }
    )";

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        for (auto name : {"a", "b", "c"}) {
            auto * attr = root.attrs()->get(state.symbols.create(name));
            state.forceValue(*attr->value, noPos);
        }
    }

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        auto * c = root.attrs()->get(state.symbols.create("c"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        state.forceValue(*c->value, noPos);

        // a==c: same alias group → Tier 1 (zero rootLoader)
        int callsBefore = loaderCalls;
        EXPECT_TRUE(state.eqValues(*a->value, *c->value, noPos,
            "a==c: same alias group"))
            << "a and c are aliased, should be equal";
        EXPECT_EQ(loaderCalls, callsBefore)
            << "Tier 1: a==c must not trigger rootLoader";

        // a!=b: different values (may trigger Tier 3 fallback for function attrs)
        EXPECT_FALSE(state.eqValues(*a->value, *b->value, noPos,
            "a!=b: different alias groups"))
            << "a and b are different, should not be equal";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// VALUE COPY tests — simulate ExprOpEq::eval → ExprSelect::eval path.
//
// These tests exercise the code path that caused the real nixpkgs
// hostPlatform == targetPlatform failure: ExprSelect copies the Value
// struct from the Bindings slot to a stack variable, and ExprOpEq
// passes these stack copies to eqValues. The copies have different
// Value* addresses than the originals in siblingIdentityMap.
//
// The fix uses a secondary Bindings*-based lookup in bindingsIdentityMap.
// These tests verify that the secondary lookup works correctly.
// ═══════════════════════════════════════════════════════════════════════

// ── Core regression test: platform pattern via Value copy ────────────

TEST_F(TracedDataTest, PointerEquality_ViaCopy_PlatformPattern)
{
    // The exact nixpkgs pattern: stdenv.hostPlatform == stdenv.targetPlatform
    // Both are aliases to the same platform object containing functions.
    // ExprOpEq creates Value copies, breaking siblingIdentityMap lookup.
    auto expr = R"(
        let platform = {
            system = "x86_64-linux";
            isLinux = true;
            canExecute = other: other.system == "x86_64-linux";
        };
        stdenv = {
            hostPlatform = platform;
            targetPlatform = platform;
        };
        in stdenv
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        ASSERT_EQ(root.type(), nAttrs) << label;

        auto * host = root.attrs()->get(state.symbols.create("hostPlatform"));
        auto * target = root.attrs()->get(state.symbols.create("targetPlatform"));
        ASSERT_NE(host, nullptr) << label;
        ASSERT_NE(target, nullptr) << label;

        state.forceValue(*host->value, noPos);
        state.forceValue(*target->value, noPos);

        // Simulate ExprSelect::eval's `v = *vAttrs`
        Value hostCopy = *host->value;
        Value targetCopy = *target->value;

        // These copies are at stack addresses NOT in siblingIdentityMap.
        // Without the Bindings*-based secondary lookup, this comparison fails.
        EXPECT_TRUE(state.eqValues(hostCopy, targetCopy, noPos,
            "hostPlatform == targetPlatform via copy"))
            << label << ": Value copies must compare equal via bindingsIdentityMap";
    };

    doTest("cold run", nullptr);

    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_ViaCopy_AttrsWithFunction)
{
    // Same as CrossPath test but with explicit Value copies.
    auto expr = R"(
        let platform = { f = y: y; system = "x86_64-linux"; };
        in { a = platform; b = platform; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);

        auto * aAttr = root.attrs()->get(state.symbols.create("a"));
        auto * bAttr = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*aAttr->value, noPos);
        state.forceValue(*bAttr->value, noPos);

        // ExprOpEq copy path
        Value aCopy = *aAttr->value;
        Value bCopy = *bAttr->value;

        EXPECT_TRUE(state.eqValues(aCopy, bCopy, noPos,
            "attrs with function via copy"))
            << label << ": copied aliased attrs must be equal";
    };

    doTest("cold run", nullptr);

    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_ViaCopy_Negative)
{
    // Negative: different attrsets with functions, compared via copies.
    // Must return false — the secondary lookup should not create false positives.
    auto expr = R"(
        let p1 = { f = y: y; system = "x86_64-linux"; };
            p2 = { f = y: y; system = "x86_64-linux"; };
        in { a = p1; b = p2; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);

        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        Value aCopy = *a->value;
        Value bCopy = *b->value;

        EXPECT_FALSE(state.eqValues(aCopy, bCopy, noPos,
            "different attrsets via copy"))
            << label << ": different attrsets must not be equal even via copy";
    };

    doTest("cold run", nullptr);

    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_ViaCopy_ThreeWayAlias)
{
    // Three aliases compared pairwise via copies.
    auto expr = R"(
        let platform = { f = y: y; system = "x86_64-linux"; };
        in { a = platform; b = platform; c = platform; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);

        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        auto * c = root.attrs()->get(state.symbols.create("c"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        state.forceValue(*c->value, noPos);

        Value aCopy = *a->value;
        Value bCopy = *b->value;
        Value cCopy = *c->value;

        EXPECT_TRUE(state.eqValues(aCopy, bCopy, noPos, "a==b via copy"))
            << label << ": a==b via copy";
        EXPECT_TRUE(state.eqValues(bCopy, cCopy, noPos, "b==c via copy"))
            << label << ": b==c via copy";
        EXPECT_TRUE(state.eqValues(aCopy, cCopy, noPos, "a==c via copy"))
            << label << ": a==c via copy";
    };

    doTest("cold run", nullptr);

    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_ViaCopy_MixedAliasAndUnique)
{
    // a and c alias same value, b is different. Via copies.
    auto expr = R"(
        let shared = { f = y: y; val = 1; };
            unique = { f = y: y; val = 2; };
        in { a = shared; b = unique; c = shared; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);

        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        auto * c = root.attrs()->get(state.symbols.create("c"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        state.forceValue(*c->value, noPos);

        Value aCopy = *a->value;
        Value bCopy = *b->value;
        Value cCopy = *c->value;

        EXPECT_TRUE(state.eqValues(aCopy, cCopy, noPos, "a==c via copy"))
            << label << ": aliased a==c must be equal via copy";
        EXPECT_FALSE(state.eqValues(aCopy, bCopy, noPos, "a!=b via copy"))
            << label << ": different a!=b must not be equal via copy";
    };

    doTest("cold run", nullptr);

    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_ViaCopy_NestedPlatform)
{
    // Deeper nesting: root.stdenv.hostPlatform vs root.stdenv.targetPlatform
    // This matches the exact nixpkgs access path through ExprSelect.
    auto expr = R"(
        let platform = {
            system = "x86_64-linux";
            go = { CGO_ENABLED = "1"; buildMode = x: x; };
            rust = { rustcTarget = "x86_64-unknown-linux-gnu"; bindgenHook = y: y; };
        };
        in {
            stdenv = {
                hostPlatform = platform;
                targetPlatform = platform;
            };
        }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);

        // Navigate: root → stdenv → hostPlatform/targetPlatform
        auto * stdenvAttr = root.attrs()->get(state.symbols.create("stdenv"));
        ASSERT_NE(stdenvAttr, nullptr) << label;
        state.forceValue(*stdenvAttr->value, noPos);

        auto * host = stdenvAttr->value->attrs()->get(state.symbols.create("hostPlatform"));
        auto * target = stdenvAttr->value->attrs()->get(state.symbols.create("targetPlatform"));
        ASSERT_NE(host, nullptr) << label;
        ASSERT_NE(target, nullptr) << label;

        state.forceValue(*host->value, noPos);
        state.forceValue(*target->value, noPos);

        // Simulate ExprOpEq copy
        Value hostCopy = *host->value;
        Value targetCopy = *target->value;

        EXPECT_TRUE(state.eqValues(hostCopy, targetCopy, noPos,
            "nested hostPlatform == targetPlatform via copy"))
            << label << ": nested platform comparison via copy must succeed";
    };

    doTest("cold run", nullptr);

    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_ViaCopy_NoFunction)
{
    // Baseline: aliased attrsets WITHOUT functions, via copies.
    // Should work even without haveSameResolvedTarget (element-wise comparison).
    auto expr = R"(
        let x = { a = 1; b = "hello"; };
        in { left = x; right = x; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);

        auto * l = root.attrs()->get(state.symbols.create("left"));
        auto * r = root.attrs()->get(state.symbols.create("right"));
        state.forceValue(*l->value, noPos);
        state.forceValue(*r->value, noPos);

        Value lCopy = *l->value;
        Value rCopy = *r->value;

        EXPECT_TRUE(state.eqValues(lCopy, rCopy, noPos,
            "no-function attrset via copy"))
            << label << ": no-function attrsets should be equal via copy";
    };

    doTest("cold run", nullptr);

    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

// ═══════════════════════════════════════════════════════════════════════
// Group 1: Separate thunks with shared Bindings* (Tier 3 probe)
//
// Two sibling attrs are different Value* (separate `id platform` thunks)
// but share the same Bindings* after forcing. detectAliases misses them
// (different thunk Value*), so Tier 1 fails. Tests whether Tier 3
// catches Bindings*-level equality.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, PointerEquality_SeparateThunks_SharedBindings)
{
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            id = x: x;
        in { a = id platform; b = id platform; }
    )";

    // Cold run
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        EXPECT_TRUE(state.eqValues(*a->value, *b->value, noPos,
            "cold: separate thunks, shared Bindings*"))
            << "Cold: id platform calls should resolve to same object";
    }

    // Hot run
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        EXPECT_TRUE(state.eqValues(*a->value, *b->value, noPos,
            "hot: separate thunks, shared Bindings*"))
            << "Hot: id platform calls should resolve to same object from cache";
    }
}

TEST_F(TracedDataTest, PointerEquality_SeparateThunks_SharedBindings_ViaCopy)
{
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            id = x: x;
        in { a = id platform; b = id platform; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        Value aCopy = *a->value;
        Value bCopy = *b->value;
        EXPECT_TRUE(state.eqValues(aCopy, bCopy, noPos,
            "separate thunks shared Bindings* via copy"))
            << label << ": Value copies of id platform must be equal";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_SeparateThunks_SharedBindings_Negative)
{
    // Different function calls produce different Bindings*, and canExecute
    // is a function → element-wise comparison fails.
    auto expr = R"(
        let mkPlatform = sys: { system = sys; canExecute = other: other.system == sys; };
        in { a = mkPlatform "x86_64-linux"; b = mkPlatform "x86_64-linux"; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        EXPECT_FALSE(state.eqValues(*a->value, *b->value, noPos,
            "separate mkPlatform calls"))
            << label << ": different function calls must produce unequal results";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

// ═══════════════════════════════════════════════════════════════════════
// Group 2: Function default arguments (mirrors nixpkgs stdenv construction)
//
// mkStdenv has default args referencing the same let-bound platform.
// Both defaults evaluate to the same Bindings* via structural copy.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, PointerEquality_FunctionDefaults_SharedPlatform)
{
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            mkStdenv = { buildPlatform ? platform, hostPlatform ? platform }: {
              inherit buildPlatform hostPlatform;
            };
        in mkStdenv {}
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        ASSERT_EQ(root.type(), nAttrs) << label;

        auto * bp = root.attrs()->get(state.symbols.create("buildPlatform"));
        auto * hp = root.attrs()->get(state.symbols.create("hostPlatform"));
        ASSERT_NE(bp, nullptr) << label;
        ASSERT_NE(hp, nullptr) << label;

        state.forceValue(*bp->value, noPos);
        state.forceValue(*hp->value, noPos);

        EXPECT_TRUE(state.eqValues(*bp->value, *hp->value, noPos,
            "buildPlatform == hostPlatform via defaults"))
            << label << ": both defaults reference same platform";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_FunctionDefaults_SharedPlatform_ViaCopy)
{
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            mkStdenv = { buildPlatform ? platform, hostPlatform ? platform }: {
              inherit buildPlatform hostPlatform;
            };
        in mkStdenv {}
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * bp = root.attrs()->get(state.symbols.create("buildPlatform"));
        auto * hp = root.attrs()->get(state.symbols.create("hostPlatform"));
        state.forceValue(*bp->value, noPos);
        state.forceValue(*hp->value, noPos);

        Value bpCopy = *bp->value;
        Value hpCopy = *hp->value;
        EXPECT_TRUE(state.eqValues(bpCopy, hpCopy, noPos,
            "function defaults shared platform via copy"))
            << label << ": Value copies from defaults must be equal";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_FunctionDefaults_DifferentPlatforms_Negative)
{
    auto expr = R"(
        let p1 = { system = "x86_64-linux"; canExecute = other: true; };
            p2 = { system = "aarch64-linux"; canExecute = other: true; };
            mkStdenv = { buildPlatform ? p1, hostPlatform ? p2 }: {
              inherit buildPlatform hostPlatform;
            };
        in mkStdenv {}
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * bp = root.attrs()->get(state.symbols.create("buildPlatform"));
        auto * hp = root.attrs()->get(state.symbols.create("hostPlatform"));
        state.forceValue(*bp->value, noPos);
        state.forceValue(*hp->value, noPos);

        EXPECT_FALSE(state.eqValues(*bp->value, *hp->value, noPos,
            "different platform defaults"))
            << label << ": different default platforms must not be equal";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

// ═══════════════════════════════════════════════════════════════════════
// Group 3: inherit through function arguments
//
// `inherit` preserves pointer equality from the caller's attrset.
// Both attrs in the caller point to the same `platform` Value*.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, PointerEquality_InheritFromArg)
{
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            wrap = { buildPlatform, hostPlatform }: { inherit buildPlatform hostPlatform; };
        in wrap { buildPlatform = platform; hostPlatform = platform; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * bp = root.attrs()->get(state.symbols.create("buildPlatform"));
        auto * hp = root.attrs()->get(state.symbols.create("hostPlatform"));
        state.forceValue(*bp->value, noPos);
        state.forceValue(*hp->value, noPos);

        EXPECT_TRUE(state.eqValues(*bp->value, *hp->value, noPos,
            "inherit from arg preserves pointer equality"))
            << label << ": inherited attrs should point to same platform";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_InheritFromArg_ViaCopy)
{
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            wrap = { buildPlatform, hostPlatform }: { inherit buildPlatform hostPlatform; };
        in wrap { buildPlatform = platform; hostPlatform = platform; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * bp = root.attrs()->get(state.symbols.create("buildPlatform"));
        auto * hp = root.attrs()->get(state.symbols.create("hostPlatform"));
        state.forceValue(*bp->value, noPos);
        state.forceValue(*hp->value, noPos);

        Value bpCopy = *bp->value;
        Value hpCopy = *hp->value;
        EXPECT_TRUE(state.eqValues(bpCopy, hpCopy, noPos,
            "inherit from arg via copy"))
            << label << ": copies of inherited attrs must be equal";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_InheritFromArg_Negative)
{
    auto expr = R"(
        let p1 = { system = "x86_64-linux"; canExecute = other: true; };
            p2 = { system = "x86_64-linux"; canExecute = other: true; };
            wrap = { buildPlatform, hostPlatform }: { inherit buildPlatform hostPlatform; };
        in wrap { buildPlatform = p1; hostPlatform = p2; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * bp = root.attrs()->get(state.symbols.create("buildPlatform"));
        auto * hp = root.attrs()->get(state.symbols.create("hostPlatform"));
        state.forceValue(*bp->value, noPos);
        state.forceValue(*hp->value, noPos);

        EXPECT_FALSE(state.eqValues(*bp->value, *hp->value, noPos,
            "inherit different objects"))
            << label << ": different platform objects must not be equal";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

// ═══════════════════════════════════════════════════════════════════════
// Group 4: In-expression comparison through function body
//
// Comparison happens inside a function body (matches mk-python-derivation.nix).
// On hot path, the result (bool) may be served from cache directly.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, PointerEquality_InExpr_FunctionBody)
{
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            check = { stdenv }: stdenv.buildPlatform == stdenv.hostPlatform;
        in {
          result = check { stdenv = { buildPlatform = platform; hostPlatform = platform; }; };
          stdenv = { buildPlatform = platform; hostPlatform = platform; };
        }
    )";

    // Cold run
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * resultAttr = root.attrs()->get(state.symbols.create("result"));
        ASSERT_NE(resultAttr, nullptr);
        state.forceValue(*resultAttr->value, noPos);
        EXPECT_EQ(resultAttr->value->type(), nBool);
        EXPECT_TRUE(resultAttr->value->boolean())
            << "Cold: comparison inside function body should return true";
    }

    // Hot run
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * resultAttr = root.attrs()->get(state.symbols.create("result"));
        ASSERT_NE(resultAttr, nullptr);
        state.forceValue(*resultAttr->value, noPos);
        EXPECT_EQ(resultAttr->value->type(), nBool);
        EXPECT_TRUE(resultAttr->value->boolean())
            << "Hot: comparison inside function body should return true from cache";
    }
}

TEST_F(TracedDataTest, PointerEquality_InExpr_FunctionDefaults)
{
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            mkDeriv = { stdenv ? { buildPlatform = platform; hostPlatform = platform; } }:
              stdenv.buildPlatform == stdenv.hostPlatform;
        in mkDeriv {}
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        EXPECT_TRUE(v.boolean())
            << "Cold: equality inside function body with default arg should be true";
    }

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        EXPECT_TRUE(v.boolean())
            << "Hot: equality inside function body with default arg should be true";
    }
}

TEST_F(TracedDataTest, PointerEquality_InExpr_FunctionBody_Negative)
{
    auto expr = R"(
        let p1 = { system = "x86_64-linux"; canExecute = other: true; };
            p2 = { system = "aarch64-linux"; canExecute = other: true; };
            check = { stdenv }: stdenv.buildPlatform == stdenv.hostPlatform;
        in {
          result = check { stdenv = { buildPlatform = p1; hostPlatform = p2; }; };
        }
    )";

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * resultAttr = root.attrs()->get(state.symbols.create("result"));
        ASSERT_NE(resultAttr, nullptr);
        state.forceValue(*resultAttr->value, noPos);
        EXPECT_EQ(resultAttr->value->type(), nBool);
        EXPECT_FALSE(resultAttr->value->boolean())
            << "Cold: different platforms in function body should return false";
    }

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * resultAttr = root.attrs()->get(state.symbols.create("result"));
        ASSERT_NE(resultAttr, nullptr);
        state.forceValue(*resultAttr->value, noPos);
        EXPECT_EQ(resultAttr->value->type(), nBool);
        EXPECT_FALSE(resultAttr->value->boolean())
            << "Hot: different platforms in function body should return false";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Group 5: End-to-end `optionals` pattern (mirrors mk-python-derivation.nix)
//
// Tests the exact pattern that drops pythonImportsCheckHook:
//   nativeBuildInputs ++ optionals (buildPlatform == hostPlatform) [ hook ]
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, PointerEquality_Optionals_IncludesHook)
{
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            stdenv = { buildPlatform = platform; hostPlatform = platform; };
            optionals = cond: list: if cond then list else [];
        in {
          nativeBuildInputs = [ "dep1" "dep2" ]
            ++ optionals (stdenv.buildPlatform == stdenv.hostPlatform) [ "pythonImportsCheckHook" ];
        }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * nbi = root.attrs()->get(state.symbols.create("nativeBuildInputs"));
        ASSERT_NE(nbi, nullptr) << label;
        state.forceValue(*nbi->value, noPos);
        ASSERT_EQ(nbi->value->type(), nList) << label;
        EXPECT_EQ(nbi->value->listSize(), 3u)
            << label << ": nativeBuildInputs should have 3 elements (hook included)";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_Optionals_ExcludesHook_CrossCompile)
{
    auto expr = R"(
        let p1 = { system = "x86_64-linux"; canExecute = other: true; };
            p2 = { system = "aarch64-linux"; canExecute = other: true; };
            stdenv = { buildPlatform = p1; hostPlatform = p2; };
            optionals = cond: list: if cond then list else [];
        in {
          nativeBuildInputs = [ "dep1" "dep2" ]
            ++ optionals (stdenv.buildPlatform == stdenv.hostPlatform) [ "pythonImportsCheckHook" ];
        }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * nbi = root.attrs()->get(state.symbols.create("nativeBuildInputs"));
        ASSERT_NE(nbi, nullptr) << label;
        state.forceValue(*nbi->value, noPos);
        ASSERT_EQ(nbi->value->type(), nList) << label;
        EXPECT_EQ(nbi->value->listSize(), 2u)
            << label << ": cross-compile should exclude hook (2 elements)";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_Optionals_SeparateThunks)
{
    // Separate thunks (id platform) but same Bindings* after forcing.
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            id = x: x;
            stdenv = { buildPlatform = id platform; hostPlatform = id platform; };
            optionals = cond: list: if cond then list else [];
        in {
          nativeBuildInputs = [ "dep1" ]
            ++ optionals (stdenv.buildPlatform == stdenv.hostPlatform) [ "hook" ];
        }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * nbi = root.attrs()->get(state.symbols.create("nativeBuildInputs"));
        ASSERT_NE(nbi, nullptr) << label;
        state.forceValue(*nbi->value, noPos);
        ASSERT_EQ(nbi->value->type(), nList) << label;
        EXPECT_EQ(nbi->value->listSize(), 2u)
            << label << ": separate thunks with shared Bindings* should include hook (2 elements)";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

// ═══════════════════════════════════════════════════════════════════════
// Group 6: Deep nested sub-attrsets with functions
//
// Matches the warning output showing haveSameResolvedTarget falling back
// for .go, .rust, .rust.platform sub-attrsets.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, PointerEquality_DeepNested_SubAttrComparison)
{
    auto expr = R"(
        let platform = {
              system = "x86_64-linux";
              go = { CGO_ENABLED = "1"; buildMode = x: x; };
              rust = { rustcTarget = "x86_64-unknown-linux-gnu"; platform = { check = y: y; }; };
            };
        in { buildPlatform = platform; hostPlatform = platform; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);

        auto * bp = root.attrs()->get(state.symbols.create("buildPlatform"));
        auto * hp = root.attrs()->get(state.symbols.create("hostPlatform"));
        ASSERT_NE(bp, nullptr) << label;
        ASSERT_NE(hp, nullptr) << label;
        state.forceValue(*bp->value, noPos);
        state.forceValue(*hp->value, noPos);

        // Top-level comparison
        {
            Value bpCopy = *bp->value;
            Value hpCopy = *hp->value;
            EXPECT_TRUE(state.eqValues(bpCopy, hpCopy, noPos,
                "deep nested: top-level via copy"))
                << label << ": top-level platform comparison via copy must succeed";
        }

        // Sub-attr: .go
        {
            auto * bpGo = bp->value->attrs()->get(state.symbols.create("go"));
            auto * hpGo = hp->value->attrs()->get(state.symbols.create("go"));
            ASSERT_NE(bpGo, nullptr) << label;
            ASSERT_NE(hpGo, nullptr) << label;
            state.forceValue(*bpGo->value, noPos);
            state.forceValue(*hpGo->value, noPos);

            Value bpGoCopy = *bpGo->value;
            Value hpGoCopy = *hpGo->value;
            EXPECT_TRUE(state.eqValues(bpGoCopy, hpGoCopy, noPos,
                "deep nested: .go via copy"))
                << label << ": .go sub-attr comparison via copy must succeed";
        }

        // Sub-attr: .rust.platform (deeply nested)
        {
            auto * bpRust = bp->value->attrs()->get(state.symbols.create("rust"));
            auto * hpRust = hp->value->attrs()->get(state.symbols.create("rust"));
            ASSERT_NE(bpRust, nullptr) << label;
            ASSERT_NE(hpRust, nullptr) << label;
            state.forceValue(*bpRust->value, noPos);
            state.forceValue(*hpRust->value, noPos);

            auto * bpRustPlat = bpRust->value->attrs()->get(state.symbols.create("platform"));
            auto * hpRustPlat = hpRust->value->attrs()->get(state.symbols.create("platform"));
            ASSERT_NE(bpRustPlat, nullptr) << label;
            ASSERT_NE(hpRustPlat, nullptr) << label;
            state.forceValue(*bpRustPlat->value, noPos);
            state.forceValue(*hpRustPlat->value, noPos);

            Value bpRpCopy = *bpRustPlat->value;
            Value hpRpCopy = *hpRustPlat->value;
            EXPECT_TRUE(state.eqValues(bpRpCopy, hpRpCopy, noPos,
                "deep nested: .rust.platform via copy"))
                << label << ": .rust.platform comparison via copy must succeed";
        }
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_DeepNested_SubAttrComparison_Negative)
{
    auto expr = R"(
        let p1 = {
              system = "x86_64-linux";
              go = { CGO_ENABLED = "1"; buildMode = x: x; };
              rust = { rustcTarget = "x86_64-unknown-linux-gnu"; platform = { check = y: y; }; };
            };
            p2 = {
              system = "aarch64-linux";
              go = { CGO_ENABLED = "0"; buildMode = x: x; };
              rust = { rustcTarget = "aarch64-unknown-linux-gnu"; platform = { check = y: y; }; };
            };
        in { buildPlatform = p1; hostPlatform = p2; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);

        auto * bp = root.attrs()->get(state.symbols.create("buildPlatform"));
        auto * hp = root.attrs()->get(state.symbols.create("hostPlatform"));
        state.forceValue(*bp->value, noPos);
        state.forceValue(*hp->value, noPos);

        Value bpCopy = *bp->value;
        Value hpCopy = *hp->value;
        EXPECT_FALSE(state.eqValues(bpCopy, hpCopy, noPos,
            "deep nested negative: top-level via copy"))
            << label << ": different platform objects must not be equal";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

// ═══════════════════════════════════════════════════════════════════════
// Group 7: Sibling wrapping interaction
//
// Tests whether forcing siblings in different orders corrupts shared
// Value* objects via sibling wrapping.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, PointerEquality_SiblingWrapping_ForceOrder)
{
    auto expr = R"(
        let shared = { f = y: y; val = 42; };
        in { a = shared; b = shared; c = shared; }
    )";

    // Cold run
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        for (auto name : {"a", "b", "c"}) {
            auto * attr = root.attrs()->get(state.symbols.create(name));
            state.forceValue(*attr->value, noPos);
        }
    }

    // Hot run: force b FIRST, then a, then compare a == c via copies
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*b->value, noPos);

        auto * a = root.attrs()->get(state.symbols.create("a"));
        state.forceValue(*a->value, noPos);

        auto * c = root.attrs()->get(state.symbols.create("c"));
        state.forceValue(*c->value, noPos);

        Value aCopy = *a->value;
        Value cCopy = *c->value;
        EXPECT_TRUE(state.eqValues(aCopy, cCopy, noPos,
            "sibling wrapping: force b first, compare a==c"))
            << "Hot: forcing b first should not corrupt a==c comparison";
    }
}

TEST_F(TracedDataTest, PointerEquality_SiblingWrapping_ReverseForceOrder)
{
    auto expr = R"(
        let shared = { f = y: y; val = 42; };
        in { a = shared; b = shared; c = shared; }
    )";

    // Cold run
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        for (auto name : {"a", "b", "c"}) {
            auto * attr = root.attrs()->get(state.symbols.create(name));
            state.forceValue(*attr->value, noPos);
        }
    }

    // Hot run: force c, b, a (reverse order), then compare a == b
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * c = root.attrs()->get(state.symbols.create("c"));
        state.forceValue(*c->value, noPos);

        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*b->value, noPos);

        auto * a = root.attrs()->get(state.symbols.create("a"));
        state.forceValue(*a->value, noPos);

        Value aCopy = *a->value;
        Value bCopy = *b->value;
        EXPECT_TRUE(state.eqValues(aCopy, bCopy, noPos,
            "sibling wrapping: reverse force order a==b"))
            << "Hot: reverse force order should not break a==b comparison";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Group 8: Value copies for lists (potential bindingsIdentityMap gap)
//
// bindingsIdentityMap only handles attrsets; list copies need
// siblingIdentityMap or Tier 1 (canonicalSiblingIdx).
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, PointerEquality_ViaCopy_ListWithFunction_Aliased)
{
    auto expr = R"(
        let xs = [ (y: y) 1 2 ];
        in { a = xs; b = xs; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        Value aCopy = *a->value;
        Value bCopy = *b->value;
        EXPECT_TRUE(state.eqValues(aCopy, bCopy, noPos,
            "list with function via copy"))
            << label << ": aliased list copies must be equal";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_ViaCopy_SmallListWithFunction_Aliased)
{
    // SmallList (<=2 elements) uses inline storage
    auto expr = R"(
        let xs = [ (y: y) ];
        in { a = xs; b = xs; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        Value aCopy = *a->value;
        Value bCopy = *b->value;
        EXPECT_TRUE(state.eqValues(aCopy, bCopy, noPos,
            "small list with function via copy"))
            << label << ": aliased small list copies must be equal";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

// ═══════════════════════════════════════════════════════════════════════
// Group 9: Nested function indirection
//
// Deeper thunk evaluation chains: g calls f calls identity.
// Tests whether the resolution chain preserves pointer equality.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, PointerEquality_NestedFunctionIndirection)
{
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            f = x: x;
            g = x: f x;
        in { a = g platform; b = g platform; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        EXPECT_TRUE(state.eqValues(*a->value, *b->value, noPos,
            "nested function indirection"))
            << label << ": g(platform) calls should resolve to same object";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_NestedFunctionIndirection_ViaCopy)
{
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            f = x: x;
            g = x: f x;
        in { a = g platform; b = g platform; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        Value aCopy = *a->value;
        Value bCopy = *b->value;
        EXPECT_TRUE(state.eqValues(aCopy, bCopy, noPos,
            "nested function indirection via copy"))
            << label << ": copies of g(platform) must be equal";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

// ═══════════════════════════════════════════════════════════════════════
// Group 10: Multiple nesting levels with function args
//
// Tests deeper nesting (2 levels) through function call boundaries:
// root → pkg → stdenv → buildPlatform/hostPlatform
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, PointerEquality_TwoLevelNesting_FunctionArg)
{
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            mkStdenv = bp: hp: { buildPlatform = bp; hostPlatform = hp; };
        in {
          pkg = {
            stdenv = mkStdenv platform platform;
          };
        }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);

        // Navigate: root → pkg → stdenv
        auto * pkg = root.attrs()->get(state.symbols.create("pkg"));
        ASSERT_NE(pkg, nullptr) << label;
        state.forceValue(*pkg->value, noPos);

        auto * stdenv = pkg->value->attrs()->get(state.symbols.create("stdenv"));
        ASSERT_NE(stdenv, nullptr) << label;
        state.forceValue(*stdenv->value, noPos);

        auto * bp = stdenv->value->attrs()->get(state.symbols.create("buildPlatform"));
        auto * hp = stdenv->value->attrs()->get(state.symbols.create("hostPlatform"));
        ASSERT_NE(bp, nullptr) << label;
        ASSERT_NE(hp, nullptr) << label;
        state.forceValue(*bp->value, noPos);
        state.forceValue(*hp->value, noPos);

        Value bpCopy = *bp->value;
        Value hpCopy = *hp->value;
        EXPECT_TRUE(state.eqValues(bpCopy, hpCopy, noPos,
            "two-level nesting: buildPlatform == hostPlatform via copy"))
            << label << ": nested platform comparison via copy must succeed";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_TwoLevelNesting_FunctionArg_Negative)
{
    auto expr = R"(
        let p1 = { system = "x86_64-linux"; canExecute = other: true; };
            p2 = { system = "aarch64-linux"; canExecute = other: true; };
            mkStdenv = bp: hp: { buildPlatform = bp; hostPlatform = hp; };
        in {
          pkg = {
            stdenv = mkStdenv p1 p2;
          };
        }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);

        auto * pkg = root.attrs()->get(state.symbols.create("pkg"));
        ASSERT_NE(pkg, nullptr) << label;
        state.forceValue(*pkg->value, noPos);

        auto * stdenv = pkg->value->attrs()->get(state.symbols.create("stdenv"));
        ASSERT_NE(stdenv, nullptr) << label;
        state.forceValue(*stdenv->value, noPos);

        auto * bp = stdenv->value->attrs()->get(state.symbols.create("buildPlatform"));
        auto * hp = stdenv->value->attrs()->get(state.symbols.create("hostPlatform"));
        state.forceValue(*bp->value, noPos);
        state.forceValue(*hp->value, noPos);

        Value bpCopy = *bp->value;
        Value hpCopy = *hp->value;
        EXPECT_FALSE(state.eqValues(bpCopy, hpCopy, noPos,
            "two-level nesting: different platforms via copy"))
            << label << ": different platforms must not be equal";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

// ═══════════════════════════════════════════════════════════════════════
// Group 11: `with` pattern
//
// Tests that `with` preserves pointer equality when both sides
// reference the same attribute from the `with` scope.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, PointerEquality_WithPattern)
{
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            platforms = { native = platform; };
        in with platforms; { buildPlatform = native; hostPlatform = native; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * bp = root.attrs()->get(state.symbols.create("buildPlatform"));
        auto * hp = root.attrs()->get(state.symbols.create("hostPlatform"));
        state.forceValue(*bp->value, noPos);
        state.forceValue(*hp->value, noPos);

        EXPECT_TRUE(state.eqValues(*bp->value, *hp->value, noPos,
            "with pattern preserves pointer equality"))
            << label << ": with-scoped attrs should point to same platform";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

TEST_F(TracedDataTest, PointerEquality_WithPattern_ViaCopy)
{
    auto expr = R"(
        let platform = { system = "x86_64-linux"; canExecute = other: true; };
            platforms = { native = platform; };
        in with platforms; { buildPlatform = native; hostPlatform = native; }
    )";

    auto doTest = [&](const char * label, int * loaderCalls) {
        auto cache = makeCache(expr, loaderCalls);
        auto root = forceRoot(*cache);
        auto * bp = root.attrs()->get(state.symbols.create("buildPlatform"));
        auto * hp = root.attrs()->get(state.symbols.create("hostPlatform"));
        state.forceValue(*bp->value, noPos);
        state.forceValue(*hp->value, noPos);

        Value bpCopy = *bp->value;
        Value hpCopy = *hp->value;
        EXPECT_TRUE(state.eqValues(bpCopy, hpCopy, noPos,
            "with pattern via copy"))
            << label << ": copies of with-scoped attrs must be equal";
    };

    doTest("cold run", nullptr);
    int loaderCalls = 0;
    doTest("hot run", &loaderCalls);
}

// ═══════════════════════════════════════════════════════════════════════
// Group 12: Real-tree contamination regression tests
//
// Tests that navigateToReal's sibling wrapping doesn't break pointer
// equality when a cache-miss sibling triggers wrapping of the real tree,
// and later getResolvedTarget navigates through contaminated cells.
//
// CRITICAL DESIGN: The aliased values must be under DIFFERENT parents
// (e.g., a.platform vs b.platform) to bypass the canonicalSiblingIdx
// fast path in haveSameResolvedTarget. Same-parent aliases (like
// stdenv.buildPlatform and stdenv.hostPlatform) are caught by
// detectAliases before getResolvedTarget is ever called.
//
// The contamination manifests only when:
//   1. Cold run records traces for target attrs but NOT for a trigger sibling
//   2. Hot run forces the trigger sibling FIRST (cache miss → navigateToReal
//      → wraps target's cells in the real tree via installChildThunk)
//   3. Hot run then compares aliased values under different parents
//   4. haveSameResolvedTarget → getResolvedTarget navigates through
//      contaminated cells
//
// Without resolveClean(), getResolvedTarget would navigate through
// materialized (contaminated) cells, producing distinct Bindings* for
// values that should share the same underlying data.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, PointerEquality_RealTreeContamination_CrossParent)
{
    // Aliased values under DIFFERENT parents: a.platform and b.platform
    // both reference the same localSystem. `trigger` is not forced on
    // the cold run, so its hot-run eval is a cache miss that triggers
    // navigateToReal → wraps `a` and `b` in the real tree.
    //
    // Different parents → canonicalSiblingIdx fast path does NOT fire →
    // forces getResolvedTarget to navigate through contaminated cells.
    auto expr = R"(
        let localSystem = { system = "x86_64-linux"; canExecute = other: true; };
        in {
          a = { platform = localSystem; };
          b = { platform = localSystem; };
          trigger = { name = "trigger"; };
        }
    )";

    // Cold run: record traces for a.platform and b.platform.
    // Do NOT force `trigger`.
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        ASSERT_EQ(root.type(), nAttrs);

        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        auto * ap = a->value->attrs()->get(state.symbols.create("platform"));
        auto * bp = b->value->attrs()->get(state.symbols.create("platform"));
        ASSERT_NE(ap, nullptr);
        ASSERT_NE(bp, nullptr);
        state.forceValue(*ap->value, noPos);
        state.forceValue(*bp->value, noPos);

        // Baseline: cold run should always work
        Value aCopy = *ap->value;
        Value bCopy = *bp->value;
        EXPECT_TRUE(state.eqValues(aCopy, bCopy, noPos,
            "contamination cross-parent: cold run"))
            << "Cold run: aliased platform attrs under different parents should be equal";
    }

    // Hot run: force `trigger` FIRST to trigger real-tree contamination.
    // trigger has no trace → cache miss → navigateToReal → wraps `a` and `b`
    // in the real tree. Then getResolvedTarget for a.platform and b.platform
    // must navigate through contaminated `a` and `b` cells.
    // Without resolveClean, each yields a materialized value with distinct
    // Bindings* → resolvedTargetsMatch returns false → eqValues returns false.
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        ASSERT_EQ(root.type(), nAttrs);

        // Force trigger FIRST — contamination trigger
        auto * trigger = root.attrs()->get(state.symbols.create("trigger"));
        ASSERT_NE(trigger, nullptr);
        state.forceValue(*trigger->value, noPos);

        // Now access through contaminated paths
        auto * a = root.attrs()->get(state.symbols.create("a"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        auto * ap = a->value->attrs()->get(state.symbols.create("platform"));
        auto * bp = b->value->attrs()->get(state.symbols.create("platform"));
        ASSERT_NE(ap, nullptr);
        ASSERT_NE(bp, nullptr);
        state.forceValue(*ap->value, noPos);
        state.forceValue(*bp->value, noPos);

        // ViaCopy comparison — matches ExprOpEq::eval behavior
        Value aCopy = *ap->value;
        Value bCopy = *bp->value;
        EXPECT_TRUE(state.eqValues(aCopy, bCopy, noPos,
            "contamination cross-parent: hot run via copy"))
            << "Hot run: aliased attrs under different parents must be equal "
               "despite real-tree contamination";
    }
}

TEST_F(TracedDataTest, PointerEquality_RealTreeContamination_CrossParent_ThreeLevel)
{
    // Three-level cross-parent structure matching the nixpkgs pattern more closely:
    // root → {pkg1.stdenv.platform, pkg2.stdenv.platform} share localSystem.
    // `trigger` contaminates at the root level; contamination cascades
    // through pkg1, pkg1.stdenv, pkg2, pkg2.stdenv.
    auto expr = R"(
        let localSystem = { system = "x86_64-linux"; canExecute = other: true; };
            mkStdenv = p: { platform = p; name = "stdenv"; };
        in {
          pkg1 = { stdenv = mkStdenv localSystem; };
          pkg2 = { stdenv = mkStdenv localSystem; };
          trigger = { name = "trigger"; };
        }
    )";

    // Cold run: force through both pkg paths. Do NOT force `trigger`.
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);

        auto * pkg1 = root.attrs()->get(state.symbols.create("pkg1"));
        auto * pkg2 = root.attrs()->get(state.symbols.create("pkg2"));
        ASSERT_NE(pkg1, nullptr);
        ASSERT_NE(pkg2, nullptr);
        state.forceValue(*pkg1->value, noPos);
        state.forceValue(*pkg2->value, noPos);

        auto * s1 = pkg1->value->attrs()->get(state.symbols.create("stdenv"));
        auto * s2 = pkg2->value->attrs()->get(state.symbols.create("stdenv"));
        ASSERT_NE(s1, nullptr);
        ASSERT_NE(s2, nullptr);
        state.forceValue(*s1->value, noPos);
        state.forceValue(*s2->value, noPos);

        auto * p1 = s1->value->attrs()->get(state.symbols.create("platform"));
        auto * p2 = s2->value->attrs()->get(state.symbols.create("platform"));
        ASSERT_NE(p1, nullptr);
        ASSERT_NE(p2, nullptr);
        state.forceValue(*p1->value, noPos);
        state.forceValue(*p2->value, noPos);

        Value c1 = *p1->value;
        Value c2 = *p2->value;
        EXPECT_TRUE(state.eqValues(c1, c2, noPos,
            "3-level cross-parent contamination: cold run"))
            << "Cold run: aliased platform attrs should be equal";
    }

    // Hot run: force `trigger` first → wraps pkg1 and pkg2 in real tree.
    // Navigation through contaminated cells at EVERY intermediate level.
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        // Force trigger FIRST — contamination trigger
        auto * trigger = root.attrs()->get(state.symbols.create("trigger"));
        ASSERT_NE(trigger, nullptr);
        state.forceValue(*trigger->value, noPos);

        // Navigate through contaminated intermediate levels
        auto * pkg1 = root.attrs()->get(state.symbols.create("pkg1"));
        auto * pkg2 = root.attrs()->get(state.symbols.create("pkg2"));
        ASSERT_NE(pkg1, nullptr);
        ASSERT_NE(pkg2, nullptr);
        state.forceValue(*pkg1->value, noPos);
        state.forceValue(*pkg2->value, noPos);

        auto * s1 = pkg1->value->attrs()->get(state.symbols.create("stdenv"));
        auto * s2 = pkg2->value->attrs()->get(state.symbols.create("stdenv"));
        ASSERT_NE(s1, nullptr);
        ASSERT_NE(s2, nullptr);
        state.forceValue(*s1->value, noPos);
        state.forceValue(*s2->value, noPos);

        auto * p1 = s1->value->attrs()->get(state.symbols.create("platform"));
        auto * p2 = s2->value->attrs()->get(state.symbols.create("platform"));
        ASSERT_NE(p1, nullptr);
        ASSERT_NE(p2, nullptr);
        state.forceValue(*p1->value, noPos);
        state.forceValue(*p2->value, noPos);

        Value c1 = *p1->value;
        Value c2 = *p2->value;
        EXPECT_TRUE(state.eqValues(c1, c2, noPos,
            "3-level cross-parent contamination: hot run via copy"))
            << "Hot run: aliased attrs must be equal despite multi-level contamination";
    }
}

} // namespace nix::eval_trace
