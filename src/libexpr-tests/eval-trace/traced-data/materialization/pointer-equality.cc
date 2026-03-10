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

} // namespace nix::eval_trace
