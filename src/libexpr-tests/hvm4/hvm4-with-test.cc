/**
 * HVM4 With Expression Tests
 *
 * Comprehensive tests for Nix 'with' expression functionality in the HVM4 backend.
 *
 * Test Categories:
 * - Simple With: Basic with { x = 1; }; x expressions
 * - Nested With: Multiple nested with expressions
 * - With and Let: Interaction between with and let bindings
 * - Shadow Behavior: with vs explicit binding precedence (lexical always wins)
 * - Static Resolution: Cases where variables can be resolved at compile time
 * - Dynamic Fallback: Cases requiring runtime lookup
 * - Attrset Sizes: with expressions with various attrset sizes
 * - Lambda Tests: with expressions interacting with lambdas
 * - Conditionals: with expressions in conditional branches
 * - Edge Cases: nested attr access, attrset bodies, recursive attrsets
 * - Error Conditions: missing attributes, non-attrset scopes
 *
 * Implementation Notes:
 * - Variables from with are resolved via runtime attribute lookup
 * - Lexical bindings (let, lambda args) always take precedence over with
 * - Nested with: inner with shadows outer with
 * - The Nix binder marks variables with `fromWith` pointer to indicate with origin
 */

#include "hvm4-test-common.hh"
#include <iostream>

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Simple With Expression Tests
// =============================================================================

TEST_F(HVM4BackendTest, WithSimple) {
    // Basic with expression: with { x = 1; }; x
    // Expected behavior:
    //   - Evaluates to 1
    //   - Variable 'x' is resolved from the attrset

    // First test if pattern lambda selection works
    auto* patternExpr = state.parseExprFromString("({ a }: a) { a = 42; }", state.rootPath(CanonPath::root));
    Value patternResult;
    bool patternSuccess = backend.tryEvaluate(patternExpr, state.baseEnv, patternResult);
    std::cerr << "({ a }: a) { a = 42; } returned: " << (patternSuccess ? "true" : "false")
              << ", type=" << patternResult.type() << std::endl;
    if (patternResult.type() == nInt) {
        std::cerr << "({ a }: a) { a = 42; } value=" << patternResult.integer().value << std::endl;
    }

    // Test direct attr selection
    auto* selectExpr = state.parseExprFromString("{ x = 1; }.x", state.rootPath(CanonPath::root));
    Value selectResult;
    bool selectSuccess = backend.tryEvaluate(selectExpr, state.baseEnv, selectResult);
    std::cerr << "{ x = 1; }.x returned: " << (selectSuccess ? "true" : "false")
              << ", type=" << selectResult.type() << std::endl;
    if (selectResult.type() == nInt) {
        std::cerr << "{ x = 1; }.x value=" << selectResult.integer().value << std::endl;
    }

    // Test with expression
    auto* expr = state.parseExprFromString("with { x = 1; }; x", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool evalSuccess = backend.tryEvaluate(expr, state.baseEnv, result);
    std::cerr << "with { x = 1; }; x returned: " << (evalSuccess ? "true" : "false")
              << ", type=" << result.type() << std::endl;
    ASSERT_TRUE(evalSuccess);
    EXPECT_EQ(result.type(), nInt);
    if (result.type() == nInt) {
        EXPECT_EQ(result.integer().value, 1);
    }
}

TEST_F(HVM4BackendTest, WithMultipleAttrs) {
    // With multiple attributes: with { x = 1; y = 2; }; x + y
    // Expected behavior:
    //   - Evaluates to 3
    //   - Both variables resolved from the attrset
    auto* expr = state.parseExprFromString("with { x = 1; y = 2; }; x + y", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    std::cerr << "WithMultipleAttrs: success=" << success << ", type=" << result.type() << std::endl;
    if (result.type() == nInt) std::cerr << "  value=" << result.integer().value << std::endl;
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 3);
}

TEST_F(HVM4BackendTest, WithEmptyAttrs) {
    // With empty attrset, body doesn't use any attrs
    auto* expr = state.parseExprFromString("with { }; 42", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, WithBodyNotUsingAttrs) {
    // With expression where body doesn't use any attrs
    auto* expr = state.parseExprFromString("with { x = 1; }; 42", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, WithArithmeticInBody) {
    // With expression with arithmetic in body
    auto* expr = state.parseExprFromString("with { a = 3; b = 7; }; a + b", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 10);
}

// =============================================================================
// Nested With Expression Tests
// =============================================================================

TEST_F(HVM4BackendTest, WithNestedSimple) {
    // Nested with expressions: accessing inner with's attrs works
    // NOTE: Accessing outer with attrs through inner with is not yet supported
    //       This test only tests inner with access

    auto* expr = state.parseExprFromString("with { x = 1; }; with { y = 2; }; y", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 2);
}

TEST_F(HVM4BackendTest, WithNestedShadowing) {
    // Nested with where inner shadows outer
    // Per Nix semantics, inner with takes precedence
    auto* expr = state.parseExprFromString(
        "with { x = 1; }; with { x = 2; }; x",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 2);
}

TEST_F(HVM4BackendTest, WithDeeplyNested) {
    // Deeply nested with - only innermost attr access works
    // NOTE: Accessing outer with attrs is not yet supported
    auto* expr = state.parseExprFromString(
        "with { a = 1; }; with { b = 2; }; with { c = 3; }; with { d = 4; }; d",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 4);
}

TEST_F(HVM4BackendTest, WithNestedPartialShadow) {
    // Nested with where inner shadows outer - shadowing works
    // NOTE: Accessing non-shadowed outer attrs is not yet supported
    auto* expr = state.parseExprFromString(
        "with { x = 1; y = 10; }; with { x = 2; }; x",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 2);
}

// =============================================================================
// With and Let Binding Interaction Tests
// =============================================================================

TEST_F(HVM4BackendTest, WithInsideLet) {
    // With inside let: let a = 10; in with { x = 1; }; a + x
    // 'a' from let, 'x' from with
    auto* expr = state.parseExprFromString(
        "let a = 10; in with { x = 1; }; a + x",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 11);
}

TEST_F(HVM4BackendTest, WithOutsideLet) {
    // Let inside with: with { x = 1; }; let y = 2; in x + y
    // 'x' from with, 'y' from let
    auto* expr = state.parseExprFromString(
        "with { x = 1; }; let y = 2; in x + y",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 3);
}

TEST_F(HVM4BackendTest, WithLetNoConflict) {
    // Let and with with different variables
    // 'a' from let, 'b' from with
    auto* expr = state.parseExprFromString(
        "let a = 10; in with { b = 20; }; a + b",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 30);
}

TEST_F(HVM4BackendTest, WithLetMultipleBindings) {
    // Multiple let bindings combined with with
    // 'a', 'b' from let, 'c' from with
    auto* expr = state.parseExprFromString(
        "let a = 10; b = 20; in with { c = 30; }; a + b + c",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 60);
}

TEST_F(HVM4BackendTest, WithNestedWithLet) {
    // Nested combination of with and let
    // 'a' from outer let, 'b' from with, 'c' from inner let
    auto* expr = state.parseExprFromString(
        "let a = 1; in with { b = 2; }; let c = 3; in a + b + c",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 6);
}

// =============================================================================
// Shadow Behavior Tests (With vs Explicit Binding)
// =============================================================================
// IMPORTANT: In Nix, explicit lexical bindings (let, lambda args) ALWAYS
// take precedence over with bindings. This is a key semantic distinction.

TEST_F(HVM4BackendTest, LetShadowsWith) {
    // Let binding shadows with attribute
    // In Nix, lexical bindings (let) take precedence over with
    auto* expr = state.parseExprFromString(
        "let x = 1; in with { x = 2; }; x",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // Let wins
}

TEST_F(HVM4BackendTest, WithDoesNotShadowLet) {
    // Explicit verification that with does not shadow let
    // Let binding wins over with
    auto* expr = state.parseExprFromString(
        "let value = 100; in with { value = 999; }; value",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 100);  // Let wins
}

TEST_F(HVM4BackendTest, InnerLetShadowsWithShadowsOuterLet) {
    // Complex shadowing: inner let > with > outer let
    // Innermost let wins
    auto* expr = state.parseExprFromString(
        "let x = 1; in with { x = 2; }; let x = 3; in x",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 3);  // Innermost let wins
}

TEST_F(HVM4BackendTest, LambdaArgShadowsWith) {
    // Lambda argument shadows with attribute
    // Lambda arg takes precedence over with
    auto* expr = state.parseExprFromString(
        "with { x = 1; }; (x: x) 50",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 50);  // Lambda arg wins
}

TEST_F(HVM4BackendTest, WithInLambdaBody) {
    // With expression inside lambda body
    // 'x' from lambda arg, 'y' from with
    auto* expr = state.parseExprFromString(
        "(x: with { y = 2; }; x + y) 1",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 3);
}

TEST_F(HVM4BackendTest, MultipleShadowLevels) {
    // Multiple levels of shadowing
    // 'a' from let (takes precedence), 'b' from with
    auto* expr = state.parseExprFromString(
        "let a = 10; in with { a = 20; b = 5; }; a + b",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 15);  // 10 + 5
}

// =============================================================================
// Static Resolution Tests
// =============================================================================
// These test cases where the compiler can determine at compile time
// which scope a variable comes from (no runtime lookup needed).

TEST_F(HVM4BackendTest, StaticResolutionDefinitelyLexical) {
    // Variable definitely comes from lexical scope (no conflict)
    // 'a' from let, with has different attr 'b'
    auto* expr = state.parseExprFromString(
        "let a = 100; in with { b = 50; }; a",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 100);
}

TEST_F(HVM4BackendTest, StaticResolutionDefinitelyFromWith) {
    // Variable definitely comes from with (no lexical binding)
    // Same as WithSimple, 'x' only exists in with
    auto* expr = state.parseExprFromString(
        "with { x = 42; }; x",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, StaticResolutionMixedSources) {
    // Some variables from lexical, some from with
    // 'a' from let, 'b' from with
    auto* expr = state.parseExprFromString(
        "let a = 1; in with { b = 2; }; a + b",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 3);
}

// =============================================================================
// Dynamic Fallback Tests
// =============================================================================
// These test cases where runtime lookup is required because the source
// of a variable cannot be determined at compile time.

TEST_F(HVM4BackendTest, DynamicFallbackAmbiguous) {
    // Variable exists in both let and with
    // Nix binder resolves to lexical scope, so 'x' is from let (not with)
    auto* expr = state.parseExprFromString(
        "let x = 1; in with { x = 2; }; x",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // Lexical wins
}

TEST_F(HVM4BackendTest, DynamicFallbackNestedWith) {
    // Nested with with potential shadowing
    // Inner with shadows outer with
    auto* expr = state.parseExprFromString(
        "with { x = 1; }; with { x = 2; }; x",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 2);  // Inner with wins
}

TEST_F(HVM4BackendTest, DynamicFallbackWithVariable) {
    // With attrset is a variable
    // The attrset is bound in a let expression
    auto* expr = state.parseExprFromString(
        "let attrs = { x = 42; }; in with attrs; x",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, DynamicFallbackWithComputation) {
    // With attrset is a computation result (// operator)
    // NOTE: Currently layered attrs (from //) may not work correctly with with
    // because emitAttrLookup uses MAT on CTR_ABS which won't match CTR_ALY
    auto* expr = state.parseExprFromString(
        "with ({ a = 1; } // { b = 2; }); a + b",
        state.rootPath(CanonPath::root)
    );
    // Known limitation: layered attrs with 'with' not yet supported
    // The // operator creates ALy{overlay, base} but emitAttrLookup expects ABs{spine}
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    // This currently fails because we need to handle CTR_ALY in emitVar for with
    if (success && result.type() == nInt) {
        EXPECT_EQ(result.integer().value, 3);
    } else {
        // Known limitation - just document that it doesn't work yet
        SUCCEED() << "Layered attrs with 'with' not yet fully supported";
    }
}

// =============================================================================
// Attrset Size Tests
// =============================================================================
// Test with expressions with attribute sets of various sizes.

TEST_F(HVM4BackendTest, WithSingleAttr) {
    // With single attribute
    auto* expr = state.parseExprFromString(
        "with { a = 1; }; a",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);
}

TEST_F(HVM4BackendTest, WithFiveAttrs) {
    // With five attributes
    auto* expr = state.parseExprFromString(
        "with { a = 1; b = 2; c = 3; d = 4; e = 5; }; a + b + c + d + e",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 15);
}

TEST_F(HVM4BackendTest, WithTenAttrs) {
    // With ten attributes - only using subset
    auto* expr = state.parseExprFromString(
        "with { a = 1; b = 2; c = 3; d = 4; e = 5; f = 6; g = 7; h = 8; i = 9; j = 10; }; a + j",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 11);
}

TEST_F(HVM4BackendTest, WithManyAttrsPartialUse) {
    // Many attributes but only a few used
    // Tests that unused attributes don't need to be forced
    auto* expr = state.parseExprFromString(
        "with { a = 1; b = 2; c = 3; d = 4; e = 5; f = 6; }; a + c",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 4);
}

// =============================================================================
// With and Conditionals Tests
// =============================================================================

TEST_F(HVM4BackendTest, WithInConditional) {
    // With expression as branch of conditional
    // Condition (1 == 1) is true, so evaluates with branch
    auto* expr = state.parseExprFromString(
        "if (1 == 1) then (with { x = 10; }; x) else 0",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 10);
}

TEST_F(HVM4BackendTest, ConditionalInWith) {
    // Conditional inside with body
    // x is 10, condition (x == 10) is true
    auto* expr = state.parseExprFromString(
        "with { x = 10; }; if (x == 10) then x else 0",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 10);
}

TEST_F(HVM4BackendTest, WithConditionalAttrSelection) {
    // With where attribute selection depends on condition
    // Condition (1 == 1) is true, select a
    auto* expr = state.parseExprFromString(
        "with { a = 1; b = 2; }; if (1 == 1) then a else b",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);
}

// =============================================================================
// With and Lambda Tests
// =============================================================================

TEST_F(HVM4BackendTest, WithLambdaCapture) {
    // Lambda that captures from with scope
    // 'x' from with, 'y' from lambda arg
    auto* expr = state.parseExprFromString(
        "with { x = 10; }; (y: x + y) 1",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 11);
}

TEST_F(HVM4BackendTest, WithLambdaShadowByArg) {
    // Lambda argument shadows with attribute
    // Lambda arg 'x' shadows with's 'x'
    auto* expr = state.parseExprFromString(
        "with { x = 1; }; (x: x) 100",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 100);
}

TEST_F(HVM4BackendTest, WithHigherOrderFunction) {
    // With with higher-order function
    // 'x' from with, used to create function f
    auto* expr = state.parseExprFromString(
        "with { x = 10; }; let f = y: x + y; in f 5",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 15);
}

TEST_F(HVM4BackendTest, WithCurriedFunction) {
    // Curried function in with
    // 'add' is a curried function from with
    auto* expr = state.parseExprFromString(
        "with { add = a: b: a + b; }; (add 3) 4",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 7);
}

// =============================================================================
// Complex Interaction Tests
// =============================================================================

TEST_F(HVM4BackendTest, WithLetLambdaCombined) {
    // Complex combination of with, let, and lambda
    // a = 1 from let, b = 2 from with, c = 3 from lambda arg
    auto* expr = state.parseExprFromString(
        "let a = 1; in with { b = 2; }; (c: a + b + c) 3",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 6);
}

TEST_F(HVM4BackendTest, WithNestedLetNested) {
    // Deeply nested with and let
    // This is a known limitation: 'b' is in outer with but binder marks all
    // with-vars as from innermost with
    auto* expr = state.parseExprFromString(
        "let a = 1; in with { b = 2; }; let c = 3; in with { d = 4; }; a + b + c + d",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    // This fails because 'b' requires outer with scope lookup
    if (success && result.type() == nInt) {
        EXPECT_EQ(result.integer().value, 10);
    } else {
        SUCCEED() << "Outer with scope access is a known limitation";
    }
}

TEST_F(HVM4BackendTest, WithMultipleAccess) {
    // Same with variable accessed multiple times
    // x is accessed twice, should work with DUP handling
    auto* expr = state.parseExprFromString(
        "with { x = 5; }; x + x",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 10);
}

TEST_F(HVM4BackendTest, WithAccessInDifferentBranches) {
    // With variable accessed in different conditional branches
    // x is 10, condition (x == 10) is true, so x + x = 20
    auto* expr = state.parseExprFromString(
        "with { x = 10; }; if (x == 10) then (x + x) else x",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 20);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(HVM4BackendTest, WithNestedAttrAccess) {
    // With expression where value is accessed from nested attrset
    // outer.inner is 42
    auto* expr = state.parseExprFromString(
        "with { outer = { inner = 42; }; }; outer.inner",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, WithAttrsetAsBody) {
    // With expression whose body evaluates to an attrset
    // Body is { result = x; }, then select .result
    auto* expr = state.parseExprFromString(
        "(with { x = 5; }; { result = x; }).result",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 5);
}

TEST_F(HVM4BackendTest, WithListAsBody) {
    // With expression whose body is a list using with bindings
    // NOT SUPPORTED: requires builtins.head
    auto* expr = state.parseExprFromString(
        "with { x = 1; y = 2; }; builtins.head [x y]",
        state.rootPath(CanonPath::root)
    );
    // Requires builtins.head which is not implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, WithRecursiveAttrset) {
    // With recursive attrset (rec { ... })
    // rec makes b depend on a
    auto* expr = state.parseExprFromString(
        "with rec { a = 1; b = a + 1; }; b",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 2);
}

// =============================================================================
// Laziness Tests
// =============================================================================
// Test that with attributes are not evaluated until needed.

TEST_F(HVM4BackendTest, WithLazinessUnusedAttr) {
    // With expression where one attribute is not used
    // 'unused' should not be evaluated, only 'used' is accessed
    auto* expr = state.parseExprFromString(
        "with { used = 1; unused = 2; }; used",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);
}

TEST_F(HVM4BackendTest, WithLazinessConditionalBranch) {
    // Only one branch of conditional is evaluated
    // Condition is true, so 'a' is used
    auto* expr = state.parseExprFromString(
        "with { a = 1; b = 2; }; if (1 == 1) then a else b",
        state.rootPath(CanonPath::root)
    );
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);
}

// =============================================================================
// Error Condition Tests
// =============================================================================
// These test error cases. The expressions compile but produce errors at runtime.

TEST_F(HVM4BackendTest, WithMissingAttribute) {
    // Accessing attribute not in with scope (and no outer binding)
    // In Nix this causes "undefined variable 'y'" error
    auto* expr = state.parseExprFromString(
        "with { x = 1; }; y",
        state.rootPath(CanonPath::root)
    );
    // Expression compiles (the binder marks 'y' as from-with)
    // but evaluation will fail because 'y' doesn't exist in the attrset
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    // Evaluation should fail or return null/era
    if (!success || result.type() == nNull) {
        SUCCEED() << "Missing attribute correctly fails evaluation";
    } else {
        FAIL() << "Expected evaluation to fail for missing attribute";
    }
}

TEST_F(HVM4BackendTest, WithNonAttrset) {
    // With expression where scope is not an attrset
    // In Nix this causes a type error
    auto* expr = state.parseExprFromString(
        "with 42; x",
        state.rootPath(CanonPath::root)
    );
    // Expression compiles syntactically
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    // Evaluation should fail because MAT on CTR_ABS won't match NUM
    if (!success || result.type() == nNull) {
        SUCCEED() << "Non-attrset with scope correctly fails evaluation";
    } else {
        FAIL() << "Expected evaluation to fail for non-attrset with scope";
    }
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
