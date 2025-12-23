/**
 * HVM4 Lambda and Application Tests
 *
 * Tests for lambda expressions and function application in the HVM4 backend:
 * - Identity functions
 * - Constant functions
 * - Curried functions
 * - Nested lambdas
 * - Higher-order functions
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Basic Lambda Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalIdentityLambda) {
    auto* expr = state.parseExprFromString("(x: x) 42", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, EvalConstLambda) {
    auto* expr = state.parseExprFromString("(x: 100) 42", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 100);
}

TEST_F(HVM4BackendTest, EvalAdditionLambda) {
    auto* expr = state.parseExprFromString("(x: x + 1) 5", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 6);
}

TEST_F(HVM4BackendTest, BoundaryEmptyBodyLambda) {
    // Lambda that just returns its argument
    auto* expr = state.parseExprFromString("(x: x) 99", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 99);
}

// =============================================================================
// Curried Lambda Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalNestedLambda) {
    // ((x: y: x + y) 3) 4 -> 7
    auto* expr = state.parseExprFromString("((x: y: x + y) 3) 4", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 7);
}

TEST_F(HVM4BackendTest, EvalDeeplyNestedLambdas) {
    // (a: b: c: d: a + b + c + d) 1 2 3 4 -> 10
    auto* expr = state.parseExprFromString("(a: b: c: d: a + b + c + d) 1 2 3 4", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 10);
}

TEST_F(HVM4BackendTest, StressDeeplyNestedLambdas) {
    // 5 levels of nested curried function application
    auto* expr = state.parseExprFromString(
        "let f = a: b: c: d: e: a + b + c + d + e; in f 1 2 3 4 5",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 15);
}

// =============================================================================
// Multi-Use Variable Tests (DUP Insertion)
// =============================================================================

TEST_F(HVM4BackendTest, EvalMultiUseVariable) {
    // Variable used twice should trigger DUP insertion
    auto* expr = state.parseExprFromString("let x = 5; in x + x", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 10);
}

TEST_F(HVM4BackendTest, EvalTripleUseVariable) {
    // Variable used three times
    auto* expr = state.parseExprFromString("let x = 3; in x + x + x", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 9);
}

TEST_F(HVM4BackendTest, EvalMultiUseLambdaArg) {
    // Lambda argument used multiple times
    auto* expr = state.parseExprFromString("(x: x + x) 7", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 14);
}

TEST_F(HVM4BackendTest, StressMultiUseVariableInLargeExpression) {
    // Variable used many times in a complex expression
    auto* expr = state.parseExprFromString(
        "let x = 5; in x + x + x + x + x",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 25);
}

// =============================================================================
// Higher-Order Function Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalFunctionReturningFunction) {
    // (x: y: x + y) returns a function
    auto* expr = state.parseExprFromString("let add = x: y: x + y; in (add 3) 4", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 7);
}

TEST_F(HVM4BackendTest, EvalLambdaReturningLambda) {
    // Higher-order function: lambda returning lambda
    auto* expr = state.parseExprFromString(
        "let makeAdder = x: y: x + y; in (makeAdder 10) 5",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 15);
}

TEST_F(HVM4BackendTest, EvalPartialApplicationInLet) {
    // Partial application stored in let binding
    auto* expr = state.parseExprFromString(
        "let add = x: y: x + y; add5 = add 5; in add5 3",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 8);
}

TEST_F(HVM4BackendTest, EvalIdentityFunctionSingleUse) {
    // Single identity function application
    auto* expr = state.parseExprFromString(
        "let id = x: x; in id 42",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

// =============================================================================
// Lambda Application Edge Cases
// =============================================================================

TEST_F(HVM4BackendTest, AppDirectLambda) {
    // Direct lambda application without let binding
    auto* expr = state.parseExprFromString("(x: x + 1) 5", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 6);
}

TEST_F(HVM4BackendTest, AppNestedDirectLambdas) {
    // Nested direct lambda applications
    auto* expr = state.parseExprFromString("(x: y: x + y) 3 4", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 7);
}

TEST_F(HVM4BackendTest, AppLambdaToLambda) {
    // Apply a lambda that returns a lambda
    auto* expr = state.parseExprFromString("((x: y: x) 10) 20", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 10);  // const function returns first arg
}

TEST_F(HVM4BackendTest, AppWithComputedArgument) {
    // Apply lambda with computed argument
    auto* expr = state.parseExprFromString("(x: x + x) (1 + 2)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 6);  // 3 + 3
}

TEST_F(HVM4BackendTest, AppResultInCondition) {
    // Use function application result in condition
    auto* expr = state.parseExprFromString(
        "if (x: x) (1 == 1) then 100 else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 100);
}

TEST_F(HVM4BackendTest, AppSingleUseInLet) {
    // Single use of lambda in let (should work)
    auto* expr = state.parseExprFromString(
        "let inc = x: x + 1; in inc 5",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 6);
}

// =============================================================================
// Shadowing in Lambda
// =============================================================================

TEST_F(HVM4BackendTest, EvalShadowingInNestedLambda) {
    // Shadow variable in nested lambda
    auto* expr = state.parseExprFromString("(x: (x: x + 1) 100) 5", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 101);  // Inner x = 100, result = 101
}

TEST_F(HVM4BackendTest, ShadowingLambdaParameter) {
    // Lambda parameter shadows let binding
    auto* expr = state.parseExprFromString(
        "let x = 100; in (x: x) 42",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

// =============================================================================
// Curried Function Stress Tests
// =============================================================================

TEST_F(HVM4BackendTest, StressCurriedFunctionDirect) {
    // Curried function applied directly (not stored in let)
    auto* expr = state.parseExprFromString(
        "let base = 100; in (x: y: base + x + y) 10 5",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 115);
}

TEST_F(HVM4BackendTest, IntegrationCurriedApplication) {
    // Curried function with multiple applications
    auto* expr = state.parseExprFromString(
        "let add3 = a: b: c: a + b + c; in add3 1 2 3",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 6);
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
