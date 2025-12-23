/**
 * HVM4 Closure and Higher-Order Function Tests
 *
 * Tests for closures and higher-order functions in the HVM4 backend:
 * - Closures capturing outer variables
 * - Multiple captures
 * - Nested closures
 * - Integration tests combining features
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Basic Closure Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalLambdaCapturingOuterVariable) {
    // Lambda that captures an outer variable (closure)
    auto* expr = state.parseExprFromString(
        "let x = 10; f = y: x + y; in f 5",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 15);
}

TEST_F(HVM4BackendTest, EvalClosureCapturingMultipleVariables) {
    // Closure that captures multiple outer variables
    auto* expr = state.parseExprFromString(
        "let a = 1; b = 2; f = x: a + b + x; in f 3",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 6);  // 1 + 2 + 3
}

TEST_F(HVM4BackendTest, EvalClosureWithMultiUseCapture) {
    // Closure where the captured variable is used multiple times
    auto* expr = state.parseExprFromString(
        "let x = 3; f = y: x + x + y; in f 1",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 7);  // 3 + 3 + 1
}

// =============================================================================
// Nested Closure Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalNestedClosures) {
    // Nested closures - outer function returns inner function that captures both scopes
    auto* expr = state.parseExprFromString(
        "let outer = 10; f = x: let inner = x + outer; in y: inner + y; in (f 5) 3",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 18);  // (5 + 10) + 3 = 18
}

TEST_F(HVM4BackendTest, EvalDeepClosureNesting) {
    // Deeply nested closures with multiple capture levels
    auto* expr = state.parseExprFromString(
        "let a = 1; in let b = 2; in let f = x: a + b + x; in f 3",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 6);  // 1 + 2 + 3
}

// =============================================================================
// Closure in Conditional Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalClosureInConditionalSingleUse) {
    // Closure used in a conditional - but only one branch executes
    // This works because only one branch uses f
    auto* expr = state.parseExprFromString(
        "let x = 5; f = y: x + y; in if 1 == 1 then f 10 else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 15);  // x + 10 = 5 + 10
}

// =============================================================================
// Mixed Lambdas and Lets
// =============================================================================

TEST_F(HVM4BackendTest, EvalMixedNestedLambdasAndLets) {
    // Mix of lambdas and lets
    auto* expr = state.parseExprFromString(
        "let f = x: let y = x + 1; in y + y; in f 5",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 12);  // (5+1) + (5+1) = 12
}

TEST_F(HVM4BackendTest, ShadowingInLambdaBody) {
    // Let inside lambda body shadows outer binding
    auto* expr = state.parseExprFromString(
        "let x = 100; f = y: let x = y; in x + 1; in f 5",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 6);  // 5 + 1
}

// =============================================================================
// Integration Tests
// =============================================================================

TEST_F(HVM4BackendTest, IntegrationAbsFunction) {
    // Absolute value pattern using conditional (single use)
    auto* expr = state.parseExprFromString(
        "let abs = x: if x == 0 then 0 else x; in abs 5",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 5);
}

TEST_F(HVM4BackendTest, IntegrationComputeWithBindings) {
    // Multiple computations using let bindings
    auto* expr = state.parseExprFromString(
        "let x = 10; y = 20; sum = x + y; diff = y + 0; in "
        "if sum == 30 then diff + 5 else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 25);  // 20 + 5
}

TEST_F(HVM4BackendTest, IntegrationNestedFunctions) {
    // Nested function application
    auto* expr = state.parseExprFromString(
        "let double = x: x + x; addOne = x: x + 1; in "
        "addOne (double 5)",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 11);  // (5 + 5) + 1
}

TEST_F(HVM4BackendTest, IntegrationBooleanLogicComplex) {
    // Complex boolean logic
    auto* expr = state.parseExprFromString(
        "let a = 1; b = 2; c = 3; in "
        "if (a == 1) && ((b == 2) || (c == 4)) then 100 else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 100);
}

TEST_F(HVM4BackendTest, IntegrationCompositeComputation) {
    // Composite computation with multiple steps
    auto* expr = state.parseExprFromString(
        "let step1 = 10 + 5; "
        "    step2 = step1 + step1; "
        "    step3 = if step2 == 30 then step2 + 10 else 0; "
        "in step3",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 40);  // 30 + 10
}

TEST_F(HVM4BackendTest, EvalComplexNestedExpression) {
    // Complex expression combining multiple features (single-use lambda pattern)
    auto* expr = state.parseExprFromString(
        "let a = 1; b = 2; in "
        "let f = x: a + b + x; in "
        "f 3",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 6);  // 1 + 2 + 3 = 6
}

TEST_F(HVM4BackendTest, EvalDeeplyNestedArithmetic) {
    // Deeply nested arithmetic operations
    auto* expr = state.parseExprFromString(
        "((((1 + 2) + 3) + 4) + 5)",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 15);
}

TEST_F(HVM4BackendTest, EvalVariablesUsedInMultipleExpressions) {
    // Variable used across multiple sub-expressions
    auto* expr = state.parseExprFromString(
        "let x = 10; in (x + 1) + (x + 2) + (x + 3)",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 36);  // 11 + 12 + 13 = 36
}

// =============================================================================
// Final Edge Cases
// =============================================================================

TEST_F(HVM4BackendTest, FinalSingleIntegerLiteral) {
    // Simplest possible expression
    auto* expr = state.parseExprFromString("1", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);
}

TEST_F(HVM4BackendTest, FinalNestedParenthesesDeep) {
    // Deeply nested parentheses
    auto* expr = state.parseExprFromString("(((((((42)))))))", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

// =============================================================================
// Statistics Tests
// =============================================================================

TEST_F(HVM4BackendTest, StatsIncrement) {
    auto* expr = state.parseExprFromString("1 + 2", state.rootPath(CanonPath::root));
    Value result;

    auto statsBefore = backend.getStats();
    EXPECT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    auto statsAfter = backend.getStats();

    EXPECT_GT(statsAfter.compilations, statsBefore.compilations);
    EXPECT_GT(statsAfter.evaluations, statsBefore.evaluations);
}

TEST_F(HVM4BackendTest, FallbackStats) {
    // Try an expression that can't be compiled (builtins not supported)
    auto* expr = state.parseExprFromString("builtins.add 1 2", state.rootPath(CanonPath::root));
    Value result;

    auto statsBefore = backend.getStats();
    EXPECT_FALSE(backend.tryEvaluate(expr, state.baseEnv, result));
    auto statsAfter = backend.getStats();

    EXPECT_GT(statsAfter.fallbacks, statsBefore.fallbacks);
    // Compilations and evaluations should not increase
    EXPECT_EQ(statsAfter.compilations, statsBefore.compilations);
    EXPECT_EQ(statsAfter.evaluations, statsBefore.evaluations);
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
