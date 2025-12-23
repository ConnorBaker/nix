/**
 * HVM4 Capability Tests
 *
 * Tests for what expressions the HVM4 backend can and cannot evaluate.
 * These tests verify the canEvaluate() method returns correct results.
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Positive Capability Tests (Can Evaluate)
// =============================================================================

TEST_F(HVM4BackendTest, CanEvaluateIntLiteral) {
    auto* expr = state.parseExprFromString("42", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateSimpleLambda) {
    auto* expr = state.parseExprFromString("x: x", state.rootPath(CanonPath::root));
    // Lambda without application can't be extracted, but can be compiled
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateAddition) {
    auto* expr = state.parseExprFromString("1 + 2", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateLet) {
    auto* expr = state.parseExprFromString("let x = 1; in x", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateEquality) {
    auto* expr = state.parseExprFromString("1 == 1", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateInequality) {
    auto* expr = state.parseExprFromString("1 != 2", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateNot) {
    // Use (1 == 1) instead of true - true/false are builtins
    auto* expr = state.parseExprFromString("!(1 == 2)", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateAnd) {
    auto* expr = state.parseExprFromString("(1 == 1) && (2 == 3)", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateOr) {
    auto* expr = state.parseExprFromString("(1 == 1) || (2 == 3)", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateIfThenElse) {
    // Use (1 == 1) instead of true - true/false are builtins
    auto* expr = state.parseExprFromString("if (1 == 1) then 1 else 2", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateNestedIf) {
    auto* expr = state.parseExprFromString("if (1 == 1) then (if (1 == 2) then 1 else 2) else 3", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Negative Capability Tests (Cannot Evaluate)
// =============================================================================

TEST_F(HVM4BackendTest, CanEvaluateString) {
    auto* expr = state.parseExprFromString("\"hello\"", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello");
}

TEST_F(HVM4BackendTest, CanEvaluateAttrSet) {
    // Attribute sets are now supported
    auto* expr = state.parseExprFromString("{ a = 1; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateList) {
    auto* expr = state.parseExprFromString("[1 2 3]", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 3);
}

TEST_F(HVM4BackendTest, CanEvaluatePatternLambda) {
    // Pattern lambdas are now supported
    auto* expr = state.parseExprFromString("{ a }: a", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateBuiltin) {
    auto* expr = state.parseExprFromString("builtins.add 1 2", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateRecursiveLet) {
    // Acyclic recursive let is now supported
    auto* expr = state.parseExprFromString("rec { x = 1; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateWith) {
    // With expressions are now supported
    auto* expr = state.parseExprFromString("with { a = 1; }; a", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);
}

TEST_F(HVM4BackendTest, CanEvaluateAssert) {
    // Assert expressions are now supported
    auto* expr = state.parseExprFromString("assert true; 1", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);
}

TEST_F(HVM4BackendTest, CanEvaluateFloatLiteral) {
    // Float literals are now supported
    auto* expr = state.parseExprFromString("1.5", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nFloat);
    EXPECT_DOUBLE_EQ(result.fpoint(), 1.5);
}

TEST_F(HVM4BackendTest, CannotEvaluateFloatArithmetic) {
    // Float arithmetic is not yet supported
    auto* expr = state.parseExprFromString("1.5 + 2.5", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluatePath) {
    // Path support is now implemented
    auto* expr = state.parseExprFromString("./foo", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateConstantStringInterpolation) {
    // Interpolation with constant strings is now supported
    auto* expr = state.parseExprFromString("\"hello ${\"world\"}\"", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello world");
}

// Unary negation (-5) is parsed as sub(0, 5) which is now supported via __sub
TEST_F(HVM4BackendTest, CanEvaluateUnaryNegation) {
    auto* expr = state.parseExprFromString("(-5)", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// true and false are now supported as builtin constants
TEST_F(HVM4BackendTest, CanEvaluateBooleanLiterals) {
    auto* exprTrue = state.parseExprFromString("true", state.rootPath(CanonPath::root));
    auto* exprFalse = state.parseExprFromString("false", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*exprTrue));
    EXPECT_TRUE(backend.canEvaluate(*exprFalse));
}

// Note: Subtraction, Multiplication, Division tests are in hvm4-arithmetic-test.cc

// Attribute selection is now supported
TEST_F(HVM4BackendTest, CanEvaluateSelect) {
    auto* expr = state.parseExprFromString("{ a = 1; }.a", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// Has-attr operator is now supported
TEST_F(HVM4BackendTest, CanEvaluateHasAttr) {
    auto* expr = state.parseExprFromString("{ a = 1; } ? a", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// Implication operator is now supported
TEST_F(HVM4BackendTest, CanEvaluateImplicationCapability) {
    auto* expr = state.parseExprFromString("(1 == 1) -> (2 == 2)", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.integer().value, 1);  // true -> true = true
}

// List concatenation is now supported
TEST_F(HVM4BackendTest, CanEvaluateListConcat) {
    auto* expr = state.parseExprFromString("[1] ++ [2]", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 2);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 1);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->integer().value, 2);
}

// Attribute set update is now supported
TEST_F(HVM4BackendTest, CanEvaluateAttrUpdate) {
    auto* expr = state.parseExprFromString("{ a = 1; } // { b = 2; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// Note: Null tests are in hvm4-null-test.cc
// Note: Comparison operator tests (<, <=, >, >=) are in hvm4-comparison-test.cc

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(HVM4BackendTest, NullExprReturnsFailure) {
    Value result;
    EXPECT_FALSE(backend.tryEvaluate(nullptr, state.baseEnv, result));
}

TEST_F(HVM4BackendTest, LambdaWithoutApplicationFallsBack) {
    // A lambda that isn't applied can be compiled but can't be extracted
    auto* expr = state.parseExprFromString("x: x", state.rootPath(CanonPath::root));
    Value result;
    // This should fail at extraction time (returns LAM, not a value)
    EXPECT_FALSE(backend.tryEvaluate(expr, state.baseEnv, result));
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
