/**
 * HVM4 Float Tests
 *
 * Tests for floating-point number handling in the HVM4 backend.
 *
 * IMPORTANT: Float LITERALS are supported, but float ARITHMETIC is not.
 * Expressions containing float arithmetic should fall back to the standard
 * Nix evaluator. These tests verify both working float literals and the
 * fallback behavior for unsupported operations.
 *
 * Test Categories:
 * - Float Literals: Verify float expressions can be compiled and evaluated
 * - Float Arithmetic: Verify float operations fall back
 * - Float Builtins: Verify float builtins fall back
 * - Mixed Int/Float: Verify mixed expressions fall back
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Float Literal Tests - These should now work
// =============================================================================

TEST_F(HVM4BackendTest, FloatLiteralSimple) {
    // 3.14 - float literal should be compilable and evaluate correctly
    auto* expr = state.parseExprFromString("3.14", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nFloat);
    EXPECT_DOUBLE_EQ(result.fpoint(), 3.14);
}

TEST_F(HVM4BackendTest, FloatLiteralZero) {
    // 0.0 - float literal should be compilable and evaluate correctly
    auto* expr = state.parseExprFromString("0.0", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nFloat);
    EXPECT_DOUBLE_EQ(result.fpoint(), 0.0);
}

TEST_F(HVM4BackendTest, FloatLiteralScientific) {
    // 1.5e10 - scientific notation float should be compilable
    auto* expr = state.parseExprFromString("1.5e10", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nFloat);
    EXPECT_DOUBLE_EQ(result.fpoint(), 1.5e10);
}

TEST_F(HVM4BackendTest, FloatLiteralNegative) {
    // 0.0 - 3.14 is subtraction, which uses a primop - should not be compilable
    auto* expr = state.parseExprFromString("0.0 - 3.14", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Float Arithmetic Tests - These should fall back
// =============================================================================
// Arithmetic involving floats should fall back

TEST_F(HVM4BackendTest, FloatAddition) {
    // 1.0 + 2.0 should not be compilable
    auto* expr = state.parseExprFromString("1.0 + 2.0", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, FloatSubtraction) {
    // 5.0 - 3.0 should not be compilable
    auto* expr = state.parseExprFromString("5.0 - 3.0", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, FloatMultiplication) {
    // 2.0 * 3.0 should not be compilable
    auto* expr = state.parseExprFromString("2.0 * 3.0", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, FloatDivision) {
    // 10.0 / 3.0 should not be compilable
    auto* expr = state.parseExprFromString("10.0 / 3.0", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Mixed Integer/Float Tests - These should fall back
// =============================================================================
// Mixing integers and floats in arithmetic should also fall back

TEST_F(HVM4BackendTest, IntPlusFloat) {
    // 1 + 2.0 should not be compilable
    auto* expr = state.parseExprFromString("1 + 2.0", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, FloatPlusInt) {
    // 1.0 + 2 should not be compilable
    auto* expr = state.parseExprFromString("1.0 + 2", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, IntDivFloat) {
    // 10 / 3.0 should not be compilable
    auto* expr = state.parseExprFromString("10 / 3.0", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Float in Let Bindings Tests
// =============================================================================

TEST_F(HVM4BackendTest, FloatInLetBinding) {
    // let x = 3.14; in x should be compilable and evaluate correctly
    auto* expr = state.parseExprFromString("let x = 3.14; in x", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nFloat);
    EXPECT_DOUBLE_EQ(result.fpoint(), 3.14);
}

TEST_F(HVM4BackendTest, FloatInLetBody) {
    // let x = 1; in x + 2.0 should not be compilable (mixed arithmetic)
    auto* expr = state.parseExprFromString("let x = 1; in x + 2.0", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Float Builtin Tests - These should fall back
// =============================================================================
// Float-related builtins should fall back

TEST_F(HVM4BackendTest, BuiltinCeil) {
    // builtins.ceil 3.2 should not be compilable
    auto* expr = state.parseExprFromString("builtins.ceil 3.2", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinFloor) {
    // builtins.floor 3.8 should not be compilable
    auto* expr = state.parseExprFromString("builtins.floor 3.8", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Float Comparison Tests - These should fall back
// =============================================================================
// Float comparisons should fall back

TEST_F(HVM4BackendTest, FloatEquality) {
    // 1.0 == 1.0 should not be compilable
    auto* expr = state.parseExprFromString("1.0 == 1.0", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, FloatLessThan) {
    // 1.0 < 2.0 should not be compilable
    auto* expr = state.parseExprFromString("1.0 < 2.0", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, IntFloatComparison) {
    // 1 < 2.0 should not be compilable
    auto* expr = state.parseExprFromString("1 < 2.0", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Float in Lambda Tests
// =============================================================================

TEST_F(HVM4BackendTest, FloatAsLambdaResult) {
    // (x: 3.14) 1 should be compilable and evaluate correctly
    auto* expr = state.parseExprFromString("(x: 3.14) 1", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nFloat);
    EXPECT_DOUBLE_EQ(result.fpoint(), 3.14);
}

TEST_F(HVM4BackendTest, FloatInLambdaBody) {
    // (x: x + 1.0) 1 should not be compilable (mixed arithmetic)
    auto* expr = state.parseExprFromString("(x: x + 1.0) 1", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Float in Conditional Tests
// =============================================================================

TEST_F(HVM4BackendTest, FloatInThenBranch) {
    // if (1 == 1) then 3.14 else 0 should be compilable
    auto* expr = state.parseExprFromString("if (1 == 1) then 3.14 else 0", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nFloat);
    EXPECT_DOUBLE_EQ(result.fpoint(), 3.14);
}

TEST_F(HVM4BackendTest, FloatInElseBranch) {
    // if (1 == 2) then 0 else 3.14 should be compilable
    auto* expr = state.parseExprFromString("if (1 == 2) then 0 else 3.14", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nFloat);
    EXPECT_DOUBLE_EQ(result.fpoint(), 3.14);
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
