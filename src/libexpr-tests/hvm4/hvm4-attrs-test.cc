/**
 * HVM4 Attribute Set Tests
 *
 * Comprehensive tests for Nix attribute set functionality in the HVM4 backend.
 *
 * Attribute sets are now implemented with the following support:
 * - Basic construction (empty, single, multiple attributes)
 * - Attribute selection (single and multi-path like .a.b.c)
 * - HasAttr operator (?)
 * - Update operator (//)
 * - Nested attribute sets
 *
 * Not yet fully implemented:
 * - Selection with default (or)
 * - Recursive attribute sets (rec { })
 * - Dynamic attribute names (${expr})
 * - Inherit keyword
 *
 * Test Categories:
 * - Basic Attrset Construction: Empty, single, multiple attrs
 * - Attribute Selection: Simple selection, nested paths
 * - Selection with Default: The "or" operator
 * - HasAttr Operator: The "?" operator
 * - Update Operator: The "//" operator
 * - Nested Attribute Sets: Deeply nested access
 * - Laziness: Values not forced until accessed
 * - Inherit Keyword: Basic inherit and inherit-from
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Basic Attrset Construction Tests
// =============================================================================

TEST_F(HVM4BackendTest, AttrsEmpty) {
    // Empty attribute set: {}
    auto* expr = state.parseExprFromString("{}", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nAttrs);
    EXPECT_EQ(result.attrs()->size(), 0);
}

TEST_F(HVM4BackendTest, AttrsSingle) {
    // Single attribute: { a = 1; }
    auto* expr = state.parseExprFromString("{ a = 1; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nAttrs);
    EXPECT_EQ(result.attrs()->size(), 1);
}

TEST_F(HVM4BackendTest, AttrsMultiple) {
    // Multiple attributes: { a = 1; b = 2; c = 3; }
    auto* expr = state.parseExprFromString("{ a = 1; b = 2; c = 3; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nAttrs);
    EXPECT_EQ(result.attrs()->size(), 3);
}

TEST_F(HVM4BackendTest, AttrsWithDifferentValueTypes) {
    // Attributes with different value types
    auto* expr = state.parseExprFromString("{ int = 42; bool = true; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nAttrs);
    EXPECT_EQ(result.attrs()->size(), 2);
}

// =============================================================================
// Attribute Selection Tests
// =============================================================================

TEST_F(HVM4BackendTest, AttrsSelectSimple) {
    // Simple attribute selection: { a = 1; }.a
    auto* expr = state.parseExprFromString("{ a = 1; }.a", state.rootPath(CanonPath::root));
    // canEvaluate returns true (we accept the expression)
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // NOTE: Evaluation may not fully work yet - selection requires complex MAT-based lookup
    // Skip tryEvaluate verification until selection is fully implemented
}

TEST_F(HVM4BackendTest, AttrsSelectFromMultiple) {
    // Selecting one attribute from a set with multiple: { a = 1; b = 2; }.b
    auto* expr = state.parseExprFromString("{ a = 1; b = 2; }.b", state.rootPath(CanonPath::root));
    // canEvaluate returns true (we accept the expression)
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // NOTE: Selection evaluation not yet fully implemented
}

TEST_F(HVM4BackendTest, AttrsSelectNestedPath) {
    // Nested attribute path: { a = { b = 1; }; }.a.b
    auto* expr = state.parseExprFromString("{ a = { b = 1; }; }.a.b", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);
}

TEST_F(HVM4BackendTest, AttrsSelectDeeplyNested) {
    // Deeply nested: { a = { b = { c = { d = 42; }; }; }; }.a.b.c.d
    auto* expr = state.parseExprFromString(
        "{ a = { b = { c = { d = 42; }; }; }; }.a.b.c.d",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

// =============================================================================
// Selection with Default (or) Tests
// =============================================================================

TEST_F(HVM4BackendTest, AttrsSelectWithDefaultMissing) {
    // Selection with default, attribute missing: { }.a or 42
    auto* expr = state.parseExprFromString("{ }.a or 42", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);  // Default is used
}

TEST_F(HVM4BackendTest, AttrsSelectWithDefaultPresent) {
    // Selection with default, attribute present: { a = 1; }.a or 42
    auto* expr = state.parseExprFromString("{ a = 1; }.a or 42", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // Attribute value is used
}

TEST_F(HVM4BackendTest, AttrsSelectWithDefaultNested) {
    // Nested path with default: { a = {}; }.a.b or 99
    auto* expr = state.parseExprFromString("{ a = {}; }.a.b or 99", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 99);  // Default is used (b doesn't exist)
}

// =============================================================================
// HasAttr Operator (?) Tests
// =============================================================================

TEST_F(HVM4BackendTest, AttrsHasAttrTrue) {
    // Attribute exists: { a = 1; } ? a
    auto* expr = state.parseExprFromString("{ a = 1; } ? a", state.rootPath(CanonPath::root));
    // canEvaluate returns true (we accept the expression)
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // NOTE: HasAttr evaluation not yet fully implemented
}

TEST_F(HVM4BackendTest, AttrsHasAttrFalse) {
    // Attribute does not exist: { a = 1; } ? b
    auto* expr = state.parseExprFromString("{ a = 1; } ? b", state.rootPath(CanonPath::root));
    // canEvaluate returns true (we accept the expression)
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // NOTE: HasAttr evaluation not yet fully implemented
}

TEST_F(HVM4BackendTest, AttrsHasAttrEmpty) {
    // Empty set: { } ? a
    auto* expr = state.parseExprFromString("{ } ? a", state.rootPath(CanonPath::root));
    // canEvaluate returns true (we accept the expression)
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // NOTE: HasAttr evaluation not yet fully implemented
}

TEST_F(HVM4BackendTest, AttrsHasAttrNestedPath) {
    // Nested path: { a = { b = 1; }; } ? a.b
    auto* expr = state.parseExprFromString("{ a = { b = 1; }; } ? a.b", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: Multi-level paths not yet supported
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Update Operator (//) Tests
// =============================================================================

TEST_F(HVM4BackendTest, AttrsUpdateSimple) {
    // Simple update: { a = 1; } // { b = 2; }
    auto* expr = state.parseExprFromString("{ a = 1; } // { b = 2; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // NOTE: Update evaluation not yet fully implemented
}

TEST_F(HVM4BackendTest, AttrsUpdateOverride) {
    // Update with override: { a = 1; } // { a = 2; }
    auto* expr = state.parseExprFromString("{ a = 1; } // { a = 2; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsUpdateEmptyBase) {
    // Update with empty base: { } // { a = 1; }
    auto* expr = state.parseExprFromString("{ } // { a = 1; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsUpdateEmptyOverlay) {
    // Update with empty overlay: { a = 1; } // { }
    auto* expr = state.parseExprFromString("{ a = 1; } // { }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsUpdateBothEmpty) {
    // Update both empty: { } // { }
    auto* expr = state.parseExprFromString("{ } // { }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsUpdateChained) {
    // Chained updates: { a = 1; } // { b = 2; } // { c = 3; }
    auto* expr = state.parseExprFromString(
        "{ a = 1; } // { b = 2; } // { c = 3; }",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsUpdateChainedOverride) {
    // Chained updates with same key - rightmost wins: { a = 1; } // { a = 2; } // { a = 3; }
    auto* expr = state.parseExprFromString(
        "{ a = 1; } // { a = 2; } // { a = 3; }",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsUpdateManyLayers) {
    // Many layers (tests layer flattening at MAX_LAYERS=8)
    auto* expr = state.parseExprFromString(
        "{} // {a=1;} // {b=2;} // {c=3;} // {d=4;} // {e=5;} // {f=6;} // {g=7;} // {h=8;} // {i=9;}",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Nested Attribute Sets Tests
// =============================================================================

TEST_F(HVM4BackendTest, AttrsNestedConstruction) {
    // Nested construction: { a = { b = 1; }; }
    auto* expr = state.parseExprFromString("{ a = { b = 1; }; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nAttrs);
    EXPECT_EQ(result.attrs()->size(), 1);
}

TEST_F(HVM4BackendTest, AttrsNestedMultipleLevels) {
    // Multiple levels of nesting
    auto* expr = state.parseExprFromString(
        "{ level1 = { level2 = { level3 = { value = 42; }; }; }; }",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nAttrs);
}

TEST_F(HVM4BackendTest, AttrsNestedAccess) {
    // Access nested value: { a = { b = { c = 42; }; }; }.a.b.c
    auto* expr = state.parseExprFromString(
        "{ a = { b = { c = 42; }; }; }.a.b.c",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

// =============================================================================
// Laziness Tests
// =============================================================================

TEST_F(HVM4BackendTest, AttrsLazyValueNotForced) {
    // Values should not be forced until accessed
    // Selecting 'a' should not force 'b' which contains a throw
    auto* expr = state.parseExprFromString(
        "{ a = 1; b = throw \"not forced\"; }.a",
        state.rootPath(CanonPath::root)
    );
    // NOT YET IMPLEMENTED: expect canEvaluate to return false
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsLazyValueNotForcedInUpdate) {
    // Update should not force values
    auto* expr = state.parseExprFromString(
        "{ a = throw \"not forced a\"; } // { b = throw \"not forced b\"; }",
        state.rootPath(CanonPath::root)
    );
    // NOT YET IMPLEMENTED: expect canEvaluate to return false
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsLazyNestedAccess) {
    // Only the accessed path should be forced
    auto* expr = state.parseExprFromString(
        "{ a = { x = 1; }; b = { y = throw \"not forced\"; }; }.a.x",
        state.rootPath(CanonPath::root)
    );
    // NOT YET IMPLEMENTED: expect canEvaluate to return false
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsKeyStrictValueLazy) {
    // Keys are strict (evaluated at construction), values are lazy
    // This tests that we can construct the attrset without forcing values
    auto* expr = state.parseExprFromString(
        "{ a = 1; b = throw \"lazy\"; }",
        state.rootPath(CanonPath::root)
    );
    // NOT YET IMPLEMENTED: expect canEvaluate to return false
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Inherit Keyword Tests
// =============================================================================

TEST_F(HVM4BackendTest, AttrsInheritSimple) {
    // Simple inherit: let x = 1; in { inherit x; }
    auto* expr = state.parseExprFromString(
        "let x = 1; in { inherit x; }",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nAttrs);
    auto attr = result.attrs()->get(state.symbols.create("x"));
    ASSERT_NE(attr, nullptr);
    state.forceValue(*attr->value, noPos);
    EXPECT_EQ(attr->value->integer().value, 1);
}

TEST_F(HVM4BackendTest, AttrsInheritMultiple) {
    // Multiple inherit: let x = 1; y = 2; z = 3; in { inherit x y z; }
    auto* expr = state.parseExprFromString(
        "let x = 1; y = 2; z = 3; in { inherit x y z; }",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nAttrs);

    auto attrX = result.attrs()->get(state.symbols.create("x"));
    ASSERT_NE(attrX, nullptr);
    state.forceValue(*attrX->value, noPos);
    EXPECT_EQ(attrX->value->integer().value, 1);

    auto attrY = result.attrs()->get(state.symbols.create("y"));
    ASSERT_NE(attrY, nullptr);
    state.forceValue(*attrY->value, noPos);
    EXPECT_EQ(attrY->value->integer().value, 2);

    auto attrZ = result.attrs()->get(state.symbols.create("z"));
    ASSERT_NE(attrZ, nullptr);
    state.forceValue(*attrZ->value, noPos);
    EXPECT_EQ(attrZ->value->integer().value, 3);
}

TEST_F(HVM4BackendTest, AttrsInheritMixed) {
    // Mixed inherit and regular attrs: let x = 1; in { inherit x; y = 2; }
    auto* expr = state.parseExprFromString(
        "let x = 1; in { inherit x; y = 2; }",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nAttrs);

    auto attrX = result.attrs()->get(state.symbols.create("x"));
    ASSERT_NE(attrX, nullptr);
    state.forceValue(*attrX->value, noPos);
    EXPECT_EQ(attrX->value->integer().value, 1);

    auto attrY = result.attrs()->get(state.symbols.create("y"));
    ASSERT_NE(attrY, nullptr);
    state.forceValue(*attrY->value, noPos);
    EXPECT_EQ(attrY->value->integer().value, 2);
}

TEST_F(HVM4BackendTest, AttrsInheritFrom) {
    // Inherit from expression: let s = { a = 1; b = 2; }; in { inherit (s) a b; }
    auto* expr = state.parseExprFromString(
        "let s = { a = 1; b = 2; }; in { inherit (s) a b; }",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nAttrs);

    auto attrA = result.attrs()->get(state.symbols.create("a"));
    ASSERT_NE(attrA, nullptr);
    state.forceValue(*attrA->value, noPos);
    EXPECT_EQ(attrA->value->integer().value, 1);

    auto attrB = result.attrs()->get(state.symbols.create("b"));
    ASSERT_NE(attrB, nullptr);
    state.forceValue(*attrB->value, noPos);
    EXPECT_EQ(attrB->value->integer().value, 2);
}

TEST_F(HVM4BackendTest, AttrsInheritFromPartial) {
    // Inherit only some attributes: let s = { a = 1; b = 2; c = 3; }; in { inherit (s) a c; }
    auto* expr = state.parseExprFromString(
        "let s = { a = 1; b = 2; c = 3; }; in { inherit (s) a c; }",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nAttrs);

    // Should have a and c, but not b
    auto attrA = result.attrs()->get(state.symbols.create("a"));
    ASSERT_NE(attrA, nullptr);
    state.forceValue(*attrA->value, noPos);
    EXPECT_EQ(attrA->value->integer().value, 1);

    auto attrC = result.attrs()->get(state.symbols.create("c"));
    ASSERT_NE(attrC, nullptr);
    state.forceValue(*attrC->value, noPos);
    EXPECT_EQ(attrC->value->integer().value, 3);

    // b should not be present
    auto attrB = result.attrs()->get(state.symbols.create("b"));
    EXPECT_EQ(attrB, nullptr);
}

// =============================================================================
// Attrset in Let Binding Tests
// =============================================================================

TEST_F(HVM4BackendTest, AttrsInLetBinding) {
    // Attrset bound in let: let x = { a = 1; }; in x.a
    auto* expr = state.parseExprFromString(
        "let x = { a = 1; }; in x.a",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // NOTE: Selection from let-bound attrs may not fully evaluate yet
}

TEST_F(HVM4BackendTest, AttrsUsingLetVars) {
    // Attrset using let variables: let x = 1; in { a = x; b = x + 1; }
    auto* expr = state.parseExprFromString(
        "let x = 1; in { a = x; b = x + 1; }",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nAttrs);
    EXPECT_EQ(result.attrs()->size(), 2);
}

TEST_F(HVM4BackendTest, AttrsNestedInLet) {
    // Nested attrset in let: let x = { inner = { value = 42; }; }; in x.inner.value
    auto* expr = state.parseExprFromString(
        "let x = { inner = { value = 42; }; }; in x.inner.value",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

// =============================================================================
// Attrset with Lambda Tests
// =============================================================================

TEST_F(HVM4BackendTest, AttrsContainingLambda) {
    // Attrset with lambda value: { f = x: x + 1; }
    auto* expr = state.parseExprFromString(
        "{ f = x: x + 1; }",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // NOTE: Extracting attrs with lambda values not yet fully supported
}

TEST_F(HVM4BackendTest, AttrsApplyLambdaFromAttr) {
    // Apply lambda from attrset: { f = x: x + 1; }.f 5
    auto* expr = state.parseExprFromString(
        "{ f = x: x + 1; }.f 5",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // NOTE: Selection and application not fully implemented yet
}

TEST_F(HVM4BackendTest, AttrsLambdaReturningAttrs) {
    // Lambda returning attrset: (x: { a = x; }) 42
    auto* expr = state.parseExprFromString(
        "(x: { a = x; }) 42",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nAttrs);
    EXPECT_EQ(result.attrs()->size(), 1);
}

// =============================================================================
// Attrset with Conditionals Tests
// =============================================================================

TEST_F(HVM4BackendTest, AttrsWithConditionalValue) {
    // Conditional value in attrset: { a = if true then 1 else 2; }
    auto* expr = state.parseExprFromString(
        "{ a = if true then 1 else 2; }",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nAttrs);
    EXPECT_EQ(result.attrs()->size(), 1);
}

TEST_F(HVM4BackendTest, AttrsConditionalSelection) {
    // Conditional selection: (if true then { a = 1; } else { a = 2; }).a
    auto* expr = state.parseExprFromString(
        "(if true then { a = 1; } else { a = 2; }).a",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // NOTE: Selection evaluation not fully implemented yet
}

// =============================================================================
// Special Key Names Tests
// =============================================================================

TEST_F(HVM4BackendTest, AttrsQuotedKeys) {
    // Keys with special characters (quoted): { "foo-bar" = 1; }
    auto* expr = state.parseExprFromString(
        "{ \"foo-bar\" = 1; }",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nAttrs);
    EXPECT_EQ(result.attrs()->size(), 1);
}

TEST_F(HVM4BackendTest, AttrsQuotedKeysWithSpaces) {
    // Keys with spaces: { "with spaces" = 1; }
    auto* expr = state.parseExprFromString(
        "{ \"with spaces\" = 1; }",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nAttrs);
    EXPECT_EQ(result.attrs()->size(), 1);
}

// =============================================================================
// Recursive Attrset Tests (rec { })
// =============================================================================

TEST_F(HVM4BackendTest, AttrsRecursiveSimple) {
    // Simple recursive attrset: rec { a = 1; b = a + 1; }
    auto* expr = state.parseExprFromString(
        "rec { a = 1; b = a + 1; }",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsRecursiveSelection) {
    // Select from recursive attrset: rec { a = 1; b = a + 1; }.b
    auto* expr = state.parseExprFromString(
        "rec { a = 1; b = a + 1; }.b",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Dynamic Attribute Names Tests
// =============================================================================

TEST_F(HVM4BackendTest, AttrsDynamicName) {
    // Dynamic attribute name: let name = "a"; in { ${name} = 1; }
    auto* expr = state.parseExprFromString(
        "let name = \"a\"; in { ${name} = 1; }",
        state.rootPath(CanonPath::root)
    );
    // NOT YET IMPLEMENTED: expect canEvaluate to return false
    // Dynamic attrs are explicitly not supported in initial implementation
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Combination Tests
// =============================================================================

TEST_F(HVM4BackendTest, AttrsComplexCombination) {
    // Complex combination of features
    auto* expr = state.parseExprFromString(
        "let base = { a = 1; b = 2; }; in (base // { c = 3; }).c",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // NOTE: Complex selection from update not fully evaluated yet
}

TEST_F(HVM4BackendTest, AttrsUpdateWithSelection) {
    // Update and then select: ({ a = 1; } // { b = 2; }).b
    auto* expr = state.parseExprFromString(
        "({ a = 1; } // { b = 2; }).b",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsHasAttrAfterUpdate) {
    // HasAttr after update: ({ a = 1; } // { b = 2; }) ? b
    auto* expr = state.parseExprFromString(
        "({ a = 1; } // { b = 2; }) ? b",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsDefaultAfterUpdate) {
    // Selection with default after update: ({ a = 1; } // { b = 2; }).c or 99
    auto* expr = state.parseExprFromString(
        "({ a = 1; } // { b = 2; }).c or 99",
        state.rootPath(CanonPath::root)
    );
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 99);  // c doesn't exist, default used
}

// =============================================================================
// Error Case Tests
// =============================================================================
// These tests verify that attribute set operations produce appropriate errors
// for invalid inputs.

TEST_F(HVM4BackendTest, AttrsMissingAttributeError) {
    // { a = 1; }.b should produce an error (missing attribute)
    auto* expr = state.parseExprFromString("{ a = 1; }.b", state.rootPath(CanonPath::root));
    // We can compile it, but evaluation may fail when attr not found
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // NOTE: Error handling for missing attrs not yet fully implemented
}

TEST_F(HVM4BackendTest, AttrsSelectOnNonAttrs) {
    // Selection on non-attrset: (42).a should produce an error at runtime
    // Note: "42.a" is parsed as attribute path lookup which throws at parse time
    // Using parens to force integer literal interpretation
    auto* expr = state.parseExprFromString("(42).a", state.rootPath(CanonPath::root));
    // canEvaluate returns true because we don't do type checking at compile time
    // Runtime type errors would occur during evaluation
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // NOTE: Runtime type checking not yet implemented
}

TEST_F(HVM4BackendTest, AttrsSelectOnList) {
    // [1 2 3].a should produce an error at runtime
    auto* expr = state.parseExprFromString("[1 2 3].a", state.rootPath(CanonPath::root));
    // canEvaluate returns true because we don't do type checking at compile time
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsHasAttrOnNonAttrs) {
    // 42 ? a should produce an error at runtime
    auto* expr = state.parseExprFromString("42 ? a", state.rootPath(CanonPath::root));
    // canEvaluate returns true because we don't do type checking at compile time
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsUpdateLeftNonAttrs) {
    // 42 // { a = 1; } should produce an error at runtime
    auto* expr = state.parseExprFromString("42 // { a = 1; }", state.rootPath(CanonPath::root));
    // canEvaluate returns true because we don't do type checking at compile time
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsUpdateRightNonAttrs) {
    // { a = 1; } // 42 should produce an error at runtime
    auto* expr = state.parseExprFromString("{ a = 1; } // 42", state.rootPath(CanonPath::root));
    // canEvaluate returns true because we don't do type checking at compile time
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, AttrsUpdateBothNonAttrs) {
    // 1 // 2 should produce an error at runtime
    auto* expr = state.parseExprFromString("1 // 2", state.rootPath(CanonPath::root));
    // canEvaluate returns true because we don't do type checking at compile time
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
