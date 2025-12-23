/**
 * HVM4 Conditional (If-Then-Else) Tests
 *
 * Tests for conditional expressions in the HVM4 backend:
 * - Basic if-then-else evaluation
 * - Nested conditionals
 * - Lazy branch evaluation
 * - Complex conditions
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Basic If-Then-Else Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalIfThenElseTrue) {
    auto* expr = state.parseExprFromString("if (1 == 1) then 42 else 0", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, EvalIfThenElseFalse) {
    auto* expr = state.parseExprFromString("if (1 == 2) then 42 else 0", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);
}

TEST_F(HVM4BackendTest, BoundaryMinimalIf) {
    // Minimal if-then-else
    auto* expr = state.parseExprFromString("if 1 == 1 then 1 else 0", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);
}

// =============================================================================
// Nested Conditional Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalNestedIfThenElse) {
    auto* expr = state.parseExprFromString(
        "if (1 == 1) then (if (2 == 2) then 100 else 50) else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 100);
}

TEST_F(HVM4BackendTest, StressNestedConditionals) {
    // Nested if-then-else with computation
    auto* expr = state.parseExprFromString(
        "if 1 == 1 then "
        "  if 2 == 2 then "
        "    if 3 == 3 then "
        "      if 4 == 4 then 100 else 0 "
        "    else 0 "
        "  else 0 "
        "else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 100);
}

TEST_F(HVM4BackendTest, CondNestedDeeply) {
    // Deeply nested conditionals
    auto* expr = state.parseExprFromString(
        "if 1 == 1 then if 2 == 2 then if 3 == 3 then 100 else 0 else 0 else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 100);
}

// =============================================================================
// Branch-Only Evaluation Tests (Lazy Evaluation)
// =============================================================================

TEST_F(HVM4BackendTest, CondTrueBranchOnly) {
    // Only true branch is evaluated when condition is true
    auto* expr = state.parseExprFromString(
        "if 1 == 1 then 42 else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, CondFalseBranchOnly) {
    // Only false branch is evaluated when condition is false
    auto* expr = state.parseExprFromString(
        "if 1 == 2 then 0 else 99",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 99);
}

// =============================================================================
// Conditional with Complex Conditions
// =============================================================================

TEST_F(HVM4BackendTest, EvalConditionalWithComplexCondition) {
    // Conditional with complex boolean condition
    auto* expr = state.parseExprFromString(
        "if (1 == 1) && (2 == 2) then 100 else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 100);
}

TEST_F(HVM4BackendTest, EvalNestedConditionalWithOr) {
    // Nested conditional with || condition
    auto* expr = state.parseExprFromString(
        "if (1 == 2) || (3 == 3) then 50 else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 50);
}

TEST_F(HVM4BackendTest, FinalConditionalWithComputedCondition) {
    // Condition is a computation
    auto* expr = state.parseExprFromString(
        "if (1 + 1) == 2 then 100 else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 100);
}

// =============================================================================
// Conditional with Let Bindings
// =============================================================================

TEST_F(HVM4BackendTest, EvalIfThenElseWithLetBinding) {
    auto* expr = state.parseExprFromString(
        "let x = 5; in if (x == 5) then x + 10 else x + 20",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 15);
}

TEST_F(HVM4BackendTest, CondWithLetInBranches) {
    // Let expressions in conditional branches
    auto* expr = state.parseExprFromString(
        "if 1 == 1 then let x = 10; in x + 5 else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 15);
}

// =============================================================================
// Conditional with Lambda
// =============================================================================

TEST_F(HVM4BackendTest, CondWithLambdaInBranches) {
    // Lambda in conditional branch
    auto* expr = state.parseExprFromString(
        "if 1 == 1 then (x: x + 1) 5 else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 6);
}

TEST_F(HVM4BackendTest, CondAsArgument) {
    // Conditional as function argument
    auto* expr = state.parseExprFromString(
        "(x: x + 1) (if 1 == 1 then 10 else 0)",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 11);
}

// =============================================================================
// Conditional Chain (else-if patterns)
// =============================================================================

TEST_F(HVM4BackendTest, IntegrationConditionalChain) {
    // Chain of conditionals with computation
    auto* expr = state.parseExprFromString(
        "let x = 5; in "
        "if x == 1 then 100 else "
        "if x == 2 then 200 else "
        "if x == 5 then 500 else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 500);
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
