/**
 * HVM4 Boolean Operator Tests
 *
 * Tests for boolean operations in the HVM4 backend:
 * - Logical NOT (!)
 * - Logical AND (&&)
 * - Logical OR (||)
 * - Short-circuit evaluation
 * - Implication (->) - TDD, not yet implemented
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Logical NOT (!) Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalNotTrue) {
    // !(1 == 2) = true
    auto* expr = state.parseExprFromString("!(1 == 2)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalNotFalse) {
    // !(1 == 1) = false
    auto* expr = state.parseExprFromString("!(1 == 1)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false = 0
}

TEST_F(HVM4BackendTest, EvalDoubleNegationBool) {
    // Double negation
    auto* expr = state.parseExprFromString("!!(1 == 1)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_NE(result.integer().value, 0);  // true
}

TEST_F(HVM4BackendTest, BoundaryNotOfEquality) {
    // Negation of equality
    auto* expr = state.parseExprFromString("!(1 == 2)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true
}

TEST_F(HVM4BackendTest, BoundaryNestedNotNot) {
    // Triple negation
    auto* expr = state.parseExprFromString("!!!(1 == 1)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // !!!true = !true = false
}

// =============================================================================
// Logical AND (&&) Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalAndTrueTrue) {
    // true && true = true
    auto* expr = state.parseExprFromString("(1 == 1) && (2 == 2)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalAndTrueFalse) {
    // true && false = false
    auto* expr = state.parseExprFromString("(1 == 1) && (1 == 2)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false = 0
}

TEST_F(HVM4BackendTest, EvalAndFalseFalse) {
    // false && false = false
    auto* expr = state.parseExprFromString("(1 == 2) && (2 == 3)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false = 0
}

TEST_F(HVM4BackendTest, EvalChainedAnd) {
    // Chained && operations
    auto* expr = state.parseExprFromString("(1 == 1) && (2 == 2) && (3 == 3)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_NE(result.integer().value, 0);  // true
}

TEST_F(HVM4BackendTest, BoundaryAndWithFalseFirst) {
    // Short-circuit && with false first
    auto* expr = state.parseExprFromString("(1 == 2) && (3 == 3)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false
}

// =============================================================================
// Logical OR (||) Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalOrTrueTrue) {
    // true || true = true
    auto* expr = state.parseExprFromString("(1 == 1) || (2 == 2)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalOrTrueFalse) {
    // true || false = true
    auto* expr = state.parseExprFromString("(1 == 1) || (1 == 2)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalOrFalseFalse) {
    // false || false = false
    auto* expr = state.parseExprFromString("(1 == 2) || (2 == 3)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false = 0
}

TEST_F(HVM4BackendTest, EvalChainedOr) {
    // Chained || operations
    auto* expr = state.parseExprFromString("(1 == 2) || (2 == 3) || (3 == 3)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_NE(result.integer().value, 0);  // true
}

TEST_F(HVM4BackendTest, BoundaryOrWithTrueFirst) {
    // Short-circuit || with true first
    auto* expr = state.parseExprFromString("(1 == 1) || (2 == 3)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true
}

// =============================================================================
// Mixed Boolean Operations
// =============================================================================

TEST_F(HVM4BackendTest, EvalMixedBooleanOps) {
    // Mixed && and || with not
    auto* expr = state.parseExprFromString("!(1 == 2) && ((2 == 2) || (3 == 4))", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_NE(result.integer().value, 0);  // !false && (true || false) = true
}

// =============================================================================
// Short-Circuit Evaluation Tests
// =============================================================================
// These tests verify that && and || do not evaluate their second operand
// when the result can be determined from the first operand.

TEST_F(HVM4BackendTest, EvalAndShortCircuit) {
    // false && (throw "should not be evaluated")
    // The second operand should NOT be evaluated since first is false
    // Note: Since throw is a builtin, we use a pattern that would fail if evaluated
    // For now, we test with an expression that would cause issues if evaluated
    // but the short-circuit should prevent it
    auto* expr = state.parseExprFromString("(1 == 2) && (1 / 0 == 0)", state.rootPath(CanonPath::root));
    Value result;
    // If short-circuit works, division by zero is never evaluated
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false = 0
}

TEST_F(HVM4BackendTest, EvalOrShortCircuit) {
    // true || (throw "should not be evaluated")
    // The second operand should NOT be evaluated since first is true
    auto* expr = state.parseExprFromString("(1 == 1) || (1 / 0 == 0)", state.rootPath(CanonPath::root));
    Value result;
    // If short-circuit works, division by zero is never evaluated
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

// =============================================================================
// Implication Operator (->) Tests
// =============================================================================

TEST_F(HVM4BackendTest, CanEvaluateImplication) {
    auto* expr = state.parseExprFromString("(1 == 1) -> (2 == 2)", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, EvalImplicationTrueTrue) {
    // true -> true = true
    auto* expr = state.parseExprFromString("(1 == 1) -> (2 == 2)", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalImplicationTrueFalse) {
    // true -> false = false
    auto* expr = state.parseExprFromString("(1 == 1) -> (1 == 2)", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false = 0
}

TEST_F(HVM4BackendTest, EvalImplicationFalseTrue) {
    // false -> true = true (ex falso quodlibet)
    auto* expr = state.parseExprFromString("(1 == 2) -> (2 == 2)", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalImplicationFalseFalse) {
    // false -> false = true (vacuous truth)
    auto* expr = state.parseExprFromString("(1 == 2) -> (2 == 3)", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalImplicationShortCircuit) {
    // false -> (error) should not evaluate the second operand
    // Short-circuit: when left side is false, right side is never evaluated
    auto* expr = state.parseExprFromString("(1 == 2) -> (1 / 0 == 0)", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    // If short-circuit works, division by zero is never evaluated
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

// =============================================================================
// Precedence Tests
// =============================================================================

TEST_F(HVM4BackendTest, PrecedenceAndOverOr) {
    // && has higher precedence than ||
    // (1==1) || (1==2) && (1==2) should be (1==1) || ((1==2) && (1==2)) = true || false = true
    auto* expr = state.parseExprFromString("(1==1) || (1==2) && (1==2)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true
}

TEST_F(HVM4BackendTest, PrecedenceNotHighest) {
    // ! has highest precedence
    // !(1==2) && (1==1) should be (!(1==2)) && (1==1) = true && true = true
    auto* expr = state.parseExprFromString("!(1==2) && (1==1)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true
}

// =============================================================================
// Complex Boolean Expression Tests
// =============================================================================

TEST_F(HVM4BackendTest, StressComplexBooleanExpression) {
    // Complex boolean expression with multiple operators
    auto* expr = state.parseExprFromString(
        "if ((1 == 1) && (2 == 2)) && ((3 == 3) || (4 == 5)) then 42 else 0",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
