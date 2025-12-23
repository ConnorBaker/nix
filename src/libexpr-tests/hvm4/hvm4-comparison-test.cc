/**
 * HVM4 Comparison Operator Tests
 *
 * Tests for comparison operators in the HVM4 backend:
 * - Equality (==)
 * - Inequality (!=)
 * - Less than (<) - TDD, not yet implemented
 * - Less than or equal (<=) - TDD, not yet implemented
 * - Greater than (>) - TDD, not yet implemented
 * - Greater than or equal (>=) - TDD, not yet implemented
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Equality Tests (==)
// =============================================================================

TEST_F(HVM4BackendTest, EvalEqualityTrue) {
    auto* expr = state.parseExprFromString("5 == 5", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalEqualityFalse) {
    auto* expr = state.parseExprFromString("5 == 6", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false = 0
}

TEST_F(HVM4BackendTest, EvalNestedComparison) {
    // (1 + 2) == 3
    auto* expr = state.parseExprFromString("(1 + 2) == 3", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);
}

TEST_F(HVM4BackendTest, EvalZeroInComparison) {
    // Zero in equality comparisons
    auto* expr = state.parseExprFromString("0 == 0", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true
}

TEST_F(HVM4BackendTest, BoundarySameValueEquality) {
    // Equality of same literal values
    auto* expr = state.parseExprFromString("42 == 42", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true
}

TEST_F(HVM4BackendTest, BoundaryZeroEquality) {
    // Zero equals zero
    auto* expr = state.parseExprFromString("0 == 0", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true
}

TEST_F(HVM4BackendTest, FinalEqualitySameExpression) {
    // Same complex expression on both sides
    auto* expr = state.parseExprFromString("(1 + 2) == (2 + 1)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // 3 == 3
}

// =============================================================================
// Inequality Tests (!=)
// =============================================================================

TEST_F(HVM4BackendTest, EvalInequalityTrue) {
    auto* expr = state.parseExprFromString("5 != 6", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalInequalityFalse) {
    auto* expr = state.parseExprFromString("5 != 5", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false = 0
}

TEST_F(HVM4BackendTest, EvalZeroNotEqualNonZero) {
    // Zero not equal to non-zero
    auto* expr = state.parseExprFromString("0 != 1", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true
}

TEST_F(HVM4BackendTest, FinalInequalityDifferentValues) {
    // Different values should be not equal
    auto* expr = state.parseExprFromString("1 != 2", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true
}

// =============================================================================
// Less Than (<) Tests (Now Implemented via __lessThan primop)
// =============================================================================

TEST_F(HVM4BackendTest, CanEvaluateLessThan) {
    auto* expr = state.parseExprFromString("1 < 2", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, EvalLessThanTrue) {
    // TDD: 1 < 2 = true
    auto* expr = state.parseExprFromString("1 < 2", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalLessThanFalse) {
    // TDD: 2 < 1 = false
    auto* expr = state.parseExprFromString("2 < 1", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false = 0
}

TEST_F(HVM4BackendTest, EvalLessThanEqual) {
    // TDD: 2 < 2 = false (equal values)
    auto* expr = state.parseExprFromString("2 < 2", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false = 0
}

// =============================================================================
// Less Than or Equal (<=) Tests (Now Implemented)
// =============================================================================

TEST_F(HVM4BackendTest, CanEvaluateLessEqual) {
    auto* expr = state.parseExprFromString("1 <= 2", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, EvalLessEqualLess) {
    // TDD: 1 <= 2 = true (less)
    auto* expr = state.parseExprFromString("1 <= 2", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalLessEqualEqual) {
    // TDD: 2 <= 2 = true (equal)
    auto* expr = state.parseExprFromString("2 <= 2", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalLessEqualFalse) {
    // TDD: 3 <= 2 = false (greater)
    auto* expr = state.parseExprFromString("3 <= 2", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false = 0
}

// =============================================================================
// Greater Than (>) Tests (Now Implemented)
// =============================================================================

TEST_F(HVM4BackendTest, CanEvaluateGreaterThan) {
    auto* expr = state.parseExprFromString("5 > 3", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, EvalGreaterThanTrue) {
    // TDD: 5 > 3 = true
    auto* expr = state.parseExprFromString("5 > 3", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalGreaterThanFalse) {
    // TDD: 3 > 5 = false
    auto* expr = state.parseExprFromString("3 > 5", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false = 0
}

TEST_F(HVM4BackendTest, EvalGreaterThanEqual) {
    // TDD: 5 > 5 = false (equal values)
    auto* expr = state.parseExprFromString("5 > 5", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false = 0
}

// =============================================================================
// Greater Than or Equal (>=) Tests (Now Implemented)
// =============================================================================

TEST_F(HVM4BackendTest, CanEvaluateGreaterEqual) {
    auto* expr = state.parseExprFromString("5 >= 3", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, EvalGreaterEqualGreater) {
    // TDD: 5 >= 3 = true (greater)
    auto* expr = state.parseExprFromString("5 >= 3", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalGreaterEqualEqual) {
    // TDD: 5 >= 5 = true (equal)
    auto* expr = state.parseExprFromString("5 >= 5", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalGreaterEqualFalse) {
    // TDD: 3 >= 5 = false (less)
    auto* expr = state.parseExprFromString("3 >= 5", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false = 0
}

// =============================================================================
// Comparison with Negative Numbers
// =============================================================================
// Signed comparisons use XOR with sign bit trick to correctly compare negative
// and positive numbers: signed_lt(a, b) = unsigned_lt(a^0x80000000, b^0x80000000)

TEST_F(HVM4BackendTest, EvalCompareNegatives) {
    // (-5) < (-3) = true
    auto* expr = state.parseExprFromString("(0 - 5) < (0 - 3)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalCompareNegativeToPositive) {
    // (-1) < 1 = true
    auto* expr = state.parseExprFromString("(0 - 1) < 1", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

TEST_F(HVM4BackendTest, EvalCompareZeroToNegative) {
    // 0 > (-5) = true
    auto* expr = state.parseExprFromString("0 > (0 - 5)", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true = 1
}

// =============================================================================
// BigInt Comparison Tests
// =============================================================================
// BigInt comparison tests
// BigInt values (> 2^31-1) are represented as #Pos{lo, hi} or #Neg{lo, hi} constructors.
// The emitBigIntLessThan function uses MAT pattern matching to dispatch to the
// appropriate comparison logic based on the operand types.

TEST_F(HVM4BackendTest, EvalCompareBigIntLess) {
    // 2147483648 < 2147483649 = true
    // Both operands are BigInt constructors (#Pos{lo, hi})
    auto* expr = state.parseExprFromString("2147483648 < 2147483649", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // 2147483648 < 2147483649 is true
}

TEST_F(HVM4BackendTest, EvalCompareBigIntEqual) {
    // 2147483648 == 2147483648 = true
    // Both operands are BigInt constructors (#Pos{lo, hi})
    // EQL operator handles structural comparison of BigInt constructors
    auto* expr = state.parseExprFromString("2147483648 == 2147483648", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // Same BigInt values are equal
}

TEST_F(HVM4BackendTest, EvalCompareBigIntNotEqual) {
    // 2147483648 != 2147483649 = true
    // Both operands are BigInt constructors (#Pos{lo, hi})
    // Inequality uses 1 - EQL to handle BigInt comparison
    auto* expr = state.parseExprFromString("2147483648 != 2147483649", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // Different BigInt values are not equal
}

TEST_F(HVM4BackendTest, EvalCompareBigIntNotEqualFalse) {
    // 2147483648 != 2147483648 = false
    // Same BigInt values should return false for !=
    auto* expr = state.parseExprFromString("2147483648 != 2147483648", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // Same BigInt values are equal, so != is false
}

TEST_F(HVM4BackendTest, EvalCompareBigIntGreater) {
    // 2147483650 > 2147483648 = true
    // Both operands are BigInt constructors (#Pos{lo, hi})
    auto* expr = state.parseExprFromString("2147483650 > 2147483648", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // 2147483650 > 2147483648 is true
}

// =============================================================================
// Precedence Tests
// =============================================================================

TEST_F(HVM4BackendTest, PrecedenceComparisonInConditional) {
    // Comparison in if condition
    auto* expr = state.parseExprFromString("if 1 + 1 == 2 then 100 else 0", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 100);
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
