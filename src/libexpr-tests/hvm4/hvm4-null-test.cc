/**
 * HVM4 Null Value Tests
 *
 * Tests for null value handling in the HVM4 backend.
 *
 * IMPORTANT: Null support is NOT YET IMPLEMENTED in the HVM4 backend.
 * These tests verify the expected behavior once null is supported.
 *
 * Test Categories:
 * - Null Literal: Basic null value parsing and evaluation
 * - Null in Expressions: Null in let bindings, conditionals, etc.
 * - Null Comparisons: Equality with null values
 * - Null Type Checking: builtins.isNull tests
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Null Literal Tests
// =============================================================================

TEST_F(HVM4BackendTest, CanEvaluateNull) {
    auto* expr = state.parseExprFromString("null", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, EvalNullLiteral) {
    // TDD: null should evaluate to null type
    auto* expr = state.parseExprFromString("null", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nNull);
}

// =============================================================================
// Null in Expressions Tests
// =============================================================================

TEST_F(HVM4BackendTest, EvalNullInLet) {
    // TDD: let x = null; in x
    auto* expr = state.parseExprFromString("let x = null; in x", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nNull);
}

TEST_F(HVM4BackendTest, EvalNullInConditionalThen) {
    // TDD: if (1 == 1) then null else 42
    auto* expr = state.parseExprFromString("if (1 == 1) then null else 42", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nNull);
}

TEST_F(HVM4BackendTest, EvalNullInConditionalElse) {
    // TDD: if (1 == 2) then 42 else null
    auto* expr = state.parseExprFromString("if (1 == 2) then 42 else null", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nNull);
}

TEST_F(HVM4BackendTest, EvalNullAsLambdaArg) {
    // TDD: (x: x) null
    auto* expr = state.parseExprFromString("(x: x) null", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nNull);
}

// =============================================================================
// Null Comparison Tests
// =============================================================================
// Null is represented as a constructor #Nul{} and comparisons use MAT-based
// pattern matching to correctly handle null values.

TEST_F(HVM4BackendTest, EvalNullEqualityTrue) {
    // null == null = true (1)
    auto* expr = state.parseExprFromString("null == null", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true
}

TEST_F(HVM4BackendTest, EvalNullNotEqualToInt) {
    // null != 0 = true (1)
    auto* expr = state.parseExprFromString("null != 0", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true
}

TEST_F(HVM4BackendTest, EvalNullEqualToIntFalse) {
    // null == 0 = false (0)
    auto* expr = state.parseExprFromString("null == 0", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 0);  // false
}

// =============================================================================
// Null Type Checking Tests (builtins.isNull)
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinIsNullTrue) {
    // TDD: builtins.isNull null = true
    auto* expr = state.parseExprFromString("builtins.isNull null", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: builtins are not supported
    EXPECT_FALSE(backend.canEvaluate(*expr));

    // TODO: Once implemented:
    // Value result;
    // bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    // ASSERT_TRUE(success);
    // EXPECT_EQ(result.type(), nBool);
    // EXPECT_TRUE(result.boolean());
}

TEST_F(HVM4BackendTest, BuiltinIsNullFalseInt) {
    // TDD: builtins.isNull 0 = false
    auto* expr = state.parseExprFromString("builtins.isNull 0", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: builtins are not supported
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinIsNullFalseString) {
    // TDD: builtins.isNull "" = false
    auto* expr = state.parseExprFromString("builtins.isNull \"\"", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: builtins are not supported
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
