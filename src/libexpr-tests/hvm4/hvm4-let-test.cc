/**
 * HVM4 Let Binding Tests
 *
 * Tests for let expressions in the HVM4 backend:
 * - Simple bindings
 * - Multiple bindings
 * - Nested let expressions
 * - Variable shadowing
 * - Dependencies between bindings
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Basic Let Binding Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalIntLiteral) {
    auto* expr = state.parseExprFromString("42", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, EvalLetSimple) {
    auto* expr = state.parseExprFromString("let x = 5; in x", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 5);
}

TEST_F(HVM4BackendTest, EvalLetWithAddition) {
    auto* expr = state.parseExprFromString("let x = 3; in x + 7", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 10);
}

TEST_F(HVM4BackendTest, BoundaryMinimalLet) {
    // Minimal let with single binding
    auto* expr = state.parseExprFromString("let x = 1; in x", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);
}

// =============================================================================
// Multiple Binding Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalLetMultipleBindings) {
    auto* expr = state.parseExprFromString("let x = 1; y = 2; in x + y", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 3);
}

TEST_F(HVM4BackendTest, EvalManyBindingsInLet) {
    // Let with many bindings
    auto* expr = state.parseExprFromString(
        "let a = 1; b = 2; c = 3; d = 4; e = 5; in a + b + c + d + e",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 15);
}

TEST_F(HVM4BackendTest, EvalMultipleBindingsWithDependencies) {
    // Let with multiple bindings where later bindings depend on earlier ones
    auto* expr = state.parseExprFromString(
        "let a = 1; b = a + 1; c = b + 1; in c",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 3);
}

// =============================================================================
// Nested Let Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalNestedLet) {
    auto* expr = state.parseExprFromString("let x = 1; in let y = 2; in x + y", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 3);
}

TEST_F(HVM4BackendTest, EvalThreeNestedLetsSimpleBody) {
    // Three nested let bindings with simple body
    auto* expr = state.parseExprFromString(
        "let a = 1; in let b = 2; in let c = 3; in c",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 3);
}

TEST_F(HVM4BackendTest, EvalThreeNestedLetsTwoVarAdd) {
    // Three nested let bindings but only use 2 vars
    auto* expr = state.parseExprFromString(
        "let a = 1; in let b = 2; in let c = 3; in a + c",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 4);
}

TEST_F(HVM4BackendTest, EvalThreeNestedLets) {
    // Three nested let bindings
    auto* expr = state.parseExprFromString(
        "let a = 1; in let b = 2; in let c = 3; in a + b + c",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 6);
}

TEST_F(HVM4BackendTest, EvalDeeplyNestedLets) {
    // Deeply nested let bindings
    auto* expr = state.parseExprFromString(
        "let a = 1; in let b = 2; in let c = 3; in let d = 4; in a + b + c + d",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 10);
}

// =============================================================================
// Variable Shadowing Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalLetShadowing) {
    auto* expr = state.parseExprFromString("let x = 1; in let x = 2; in x", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 2);
}

TEST_F(HVM4BackendTest, ShadowingInNestedLet) {
    // Inner let shadows outer let binding
    auto* expr = state.parseExprFromString(
        "let x = 10; in let x = 20; in x",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 20);
}

TEST_F(HVM4BackendTest, ShadowingOuterStillAccessible) {
    // Outer binding accessible before shadowing
    auto* expr = state.parseExprFromString(
        "let x = 10; in x + (let x = 5; in x)",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 15);  // 10 + 5
}

TEST_F(HVM4BackendTest, ShadowingMultipleLevels) {
    // Multiple levels of shadowing
    auto* expr = state.parseExprFromString(
        "let x = 1; in let x = 2; in let x = 3; in x",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 3);
}

TEST_F(HVM4BackendTest, ShadowingDifferentVariables) {
    // Different variables don't shadow each other
    auto* expr = state.parseExprFromString(
        "let x = 1; y = 2; in let z = 3; in x + y + z",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 6);
}

// =============================================================================
// Unused Binding Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalUnusedBinding) {
    // Binding that's never used
    auto* expr = state.parseExprFromString("let unused = 999; in 42", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, LetWithUnusedBindings) {
    // Unused bindings should not affect result
    auto* expr = state.parseExprFromString(
        "let unused1 = 999; x = 42; unused2 = 888; in x",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

// =============================================================================
// Binding Order and Dependencies
// =============================================================================

TEST_F(HVM4BackendTest, LetBindingOrder) {
    // Later bindings can reference earlier ones
    auto* expr = state.parseExprFromString(
        "let a = 1; b = a + 1; c = b + 1; in c",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 3);
}

TEST_F(HVM4BackendTest, LetNestedWithSameNames) {
    // Inner let can use same name as outer
    auto* expr = state.parseExprFromString(
        "let x = 1; in let y = x + 1; in let x = y + 1; in x",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 3);  // 1 + 1 + 1
}

// =============================================================================
// Let with Complex Values
// =============================================================================

TEST_F(HVM4BackendTest, LetBindingInCondition) {
    // Let binding used in condition
    auto* expr = state.parseExprFromString(
        "let x = 5; in if x == 5 then 100 else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 100);
}

TEST_F(HVM4BackendTest, LetBindingWithLambda) {
    // Let binding containing a lambda (single use)
    auto* expr = state.parseExprFromString(
        "let f = x: x + 10; in f 5",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 15);
}

TEST_F(HVM4BackendTest, LetBindingComplexExpression) {
    // Complex expression as binding value
    auto* expr = state.parseExprFromString(
        "let x = if 1 == 1 then 10 + 20 else 0; in x + 5",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 35);  // 30 + 5
}

TEST_F(HVM4BackendTest, EvalSingleBindingWithComputation) {
    // Let with single binding that is a computation
    auto* expr = state.parseExprFromString(
        "let x = 10 + 20; in x + x",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 60);  // 30 + 30
}

// =============================================================================
// Stress Tests
// =============================================================================

TEST_F(HVM4BackendTest, StressManyLetBindings) {
    // 10 let bindings forming a dependency chain
    auto* expr = state.parseExprFromString(
        "let a = 1; b = a + 1; c = b + 1; d = c + 1; e = d + 1; "
        "f = e + 1; g = f + 1; h = g + 1; i = h + 1; j = i + 1; in j",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 10);  // 1 + 9 increments
}

TEST_F(HVM4BackendTest, StressDeeplyNestedLets) {
    // 5 levels of nested let expressions
    auto* expr = state.parseExprFromString(
        "let a = 1; in "
        "let b = a + 1; in "
        "let c = b + 1; in "
        "let d = c + 1; in "
        "let e = d + 1; in e",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 5);
}

// =============================================================================
// Final Let Tests
// =============================================================================

TEST_F(HVM4BackendTest, FinalLetWithAllFeatures) {
    // Comprehensive let expression
    auto* expr = state.parseExprFromString(
        "let a = 1; b = 2; c = a + b; f = x: x + c; in f 10",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 13);  // 10 + (1 + 2)
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
