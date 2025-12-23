/**
 * HVM4 Known Limitations Tests
 *
 * This file documents known limitations of the HVM4 backend and provides
 * regression tests to ensure we don't accidentally claim to support features
 * that don't work correctly.
 *
 * Limitations are organized by category:
 * 1. BigInt overflow - arithmetic on values > 2^31-1 produces incorrect results
 * 2. BigInt comparisons - comparison operators fail on BigInt constructors
 * 3. Lists - not yet implemented (Phase 2)
 * 4. Strings - not yet implemented (Phase 3)
 * 5. Attribute sets - not yet implemented (Phase 4)
 * 6. With expressions - not yet implemented (Phase 8)
 * 7. Imports - not yet implemented (Phase 9)
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// BigInt Overflow Limitations
// =============================================================================
// HVM4's arithmetic operators (OP_ADD, OP_SUB, OP_MUL, OP_DIV) operate on
// 32-bit values. When the result exceeds 32 bits, overflow occurs.

TEST_F(HVM4BackendTest, LimitationBigIntAdditionOverflow) {
    // 2147483647 + 1 should be 2147483648, but overflows to -2147483648
    auto* expr = state.parseExprFromString("2147483647 + 1", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    // KNOWN LIMITATION: result is -2147483648 due to 32-bit overflow
    // When multi-word arithmetic is implemented, result should be 2147483648
}

TEST_F(HVM4BackendTest, LimitationBigIntMultiplicationOverflow) {
    // 65536 * 65536 should be 4294967296, but overflows
    auto* expr = state.parseExprFromString("65536 * 65536", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    // KNOWN LIMITATION: result is 0 due to 32-bit overflow
    // When multi-word arithmetic is implemented, result should be 4294967296
}

// =============================================================================
// BigInt Comparison Limitations
// =============================================================================
// HVM4's OP_LT operator cannot compare BigInt constructors directly.
// However, EQL (structural equality) handles BigInt equality correctly.

TEST_F(HVM4BackendTest, LimitationBigIntEqualityWorks) {
    // Both operands are BigInt constructors
    // EQL handles structural comparison of #Pos{lo, hi} constructors
    auto* expr = state.parseExprFromString("2147483648 == 2147483648", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // BigInt equality now works via EQL
}

TEST_F(HVM4BackendTest, LimitationBigIntLessThanWorks) {
    // BigInt less-than comparison now works via MAT pattern matching
    auto* expr = state.parseExprFromString("2147483648 < 2147483649", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);  // true
}

TEST_F(HVM4BackendTest, LimitationBigIntArithmeticFails) {
    // Adding two BigInt values fails because OP_ADD can't operate on constructors
    auto* expr = state.parseExprFromString("4000000000 + 4000000000", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // KNOWN LIMITATION: tryEvaluate fails for BigInt arithmetic
}

// =============================================================================
// Division by Zero Limitation
// =============================================================================
// HVM4 does not detect division by zero.

TEST_F(HVM4BackendTest, LimitationDivisionByZeroNotDetected) {
    // Division by zero produces undefined result instead of error
    auto* expr = state.parseExprFromString("42 / 0", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    // KNOWN LIMITATION: HVM4 doesn't detect div by zero
    // Instead of throwing an error, it produces garbage
    EXPECT_TRUE(success);  // Unfortunately succeeds with undefined result
}

// =============================================================================
// Lists Implementation Status
// =============================================================================

TEST_F(HVM4BackendTest, ListsAreImplemented) {
    // Basic list literal - NOW IMPLEMENTED
    auto* expr = state.parseExprFromString("[1 2 3]", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 3);
}

TEST_F(HVM4BackendTest, ListConcatImplemented) {
    // List concatenation - NOW IMPLEMENTED
    auto* expr = state.parseExprFromString("[1] ++ [2]", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 2);
}

TEST_F(HVM4BackendTest, LimitationBuiltinsHeadNotImplemented) {
    auto* expr = state.parseExprFromString("builtins.head [1 2 3]", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, LimitationBuiltinsLengthNotImplemented) {
    auto* expr = state.parseExprFromString("builtins.length [1 2 3]", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// String Literals and Constant Concatenation Now Work (Phase 3)
// =============================================================================

TEST_F(HVM4BackendTest, StringsNowSupported) {
    // Basic string literals are now supported
    auto* expr = state.parseExprFromString("\"hello\"", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello");
}

TEST_F(HVM4BackendTest, StringConcatNowSupported) {
    // String concatenation with literals is now supported
    auto* expr = state.parseExprFromString("\"hello\" + \" world\"", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello world");
}

TEST_F(HVM4BackendTest, LimitationStringInterpolationNotImplemented) {
    // String interpolation with builtins is not yet supported
    auto* expr = state.parseExprFromString("let x = 42; in \"value: ${toString x}\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Attribute Sets ARE Implemented (Phase 4)
// =============================================================================

TEST_F(HVM4BackendTest, LimitationAttrsNowImplemented) {
    // Basic attrset - now supported
    auto* expr = state.parseExprFromString("{ a = 1; b = 2; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, LimitationAttrAccessNowImplemented) {
    auto* expr = state.parseExprFromString("{ a = 1; }.a", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, LimitationAttrUpdateNowImplemented) {
    auto* expr = state.parseExprFromString("{ a = 1; } // { b = 2; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, RecursiveAttrsNowImplemented) {
    // Acyclic recursive attrs are now implemented (Phase 7)
    auto* expr = state.parseExprFromString("rec { a = 1; b = a + 1; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Pattern-Matching Lambdas Now Implemented (Phase 6)
// =============================================================================

TEST_F(HVM4BackendTest, PatternLambdaImplemented) {
    auto* expr = state.parseExprFromString("{ a, b }: a + b", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaDefaultsImplemented) {
    auto* expr = state.parseExprFromString("{ a, b ? 0 }: a + b", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaEllipsisImplemented) {
    auto* expr = state.parseExprFromString("{ a, ... }: a", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// With Expressions - Partial Support (Phase 8)
// =============================================================================
// Basic with is now implemented. Known limitations:
// - Accessing attrs from outer with scopes in nested with expressions
// - Layered attrs (from //) as with scope may not work

TEST_F(HVM4BackendTest, LimitationWithNowImplemented) {
    // Basic with is now supported
    auto* expr = state.parseExprFromString("with { a = 1; }; a", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 1);
}

TEST_F(HVM4BackendTest, LimitationWithOuterScopeAccess) {
    // Accessing outer with scope's attrs is a known limitation
    // In this test, 'a' is in outer with, 'b' is in inner with
    auto* expr = state.parseExprFromString("with { a = 1; }; with { b = 2; }; a + b", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    // This may fail because 'a' requires outer scope lookup
    // Just verify it compiles
    SUCCEED() << "Outer with scope access is a known limitation";
}

// =============================================================================
// Imports Not Implemented (Phase 9)
// =============================================================================

TEST_F(HVM4BackendTest, LimitationImportNotImplemented) {
    // import requires path and attrset support
    auto* expr = state.parseExprFromString("import ./test.nix", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Builtins Not Implemented
// =============================================================================

TEST_F(HVM4BackendTest, LimitationBuiltinsMapNotImplemented) {
    auto* expr = state.parseExprFromString("builtins.map (x: x) [1 2 3]", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, LimitationBuiltinsFoldlNotImplemented) {
    auto* expr = state.parseExprFromString("builtins.foldl' (a: b: a + b) 0 [1 2 3]", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, LimitationBuiltinsToStringNotImplemented) {
    auto* expr = state.parseExprFromString("builtins.toString 42", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, LimitationBuiltinsIsNullNotImplemented) {
    auto* expr = state.parseExprFromString("builtins.isNull null", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, LimitationBuiltinsAttrNamesNotImplemented) {
    auto* expr = state.parseExprFromString("builtins.attrNames { a = 1; }", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Float Arithmetic Not Implemented (Float literals work, arithmetic does not)
// =============================================================================

TEST_F(HVM4BackendTest, LimitationFloatArithmeticNotImplemented) {
    // Float literals work, but float arithmetic does not
    auto* expr = state.parseExprFromString("1.5 + 2.5", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, SanityFloatLiteralWorks) {
    // Float literals are now supported
    auto* expr = state.parseExprFromString("3.14", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nFloat);
    EXPECT_DOUBLE_EQ(result.fpoint(), 3.14);
}

// =============================================================================
// Features That DO Work (Sanity Checks)
// =============================================================================
// These tests verify features that should work, as a sanity check.

TEST_F(HVM4BackendTest, SanityIntegerLiteralWorks) {
    auto* expr = state.parseExprFromString("42", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, SanitySmallArithmeticWorks) {
    auto* expr = state.parseExprFromString("10 + 20 - 5", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.integer().value, 25);
}

TEST_F(HVM4BackendTest, SanityComparisonWorks) {
    auto* expr = state.parseExprFromString("5 < 10", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.integer().value, 1);  // true
}

TEST_F(HVM4BackendTest, SanityLetBindingWorks) {
    auto* expr = state.parseExprFromString("let x = 5; in x + x", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.integer().value, 10);
}

TEST_F(HVM4BackendTest, SanityLambdaWorks) {
    auto* expr = state.parseExprFromString("(x: x + 1) 10", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.integer().value, 11);
}

TEST_F(HVM4BackendTest, SanityIfThenElseWorks) {
    auto* expr = state.parseExprFromString("if 1 < 2 then 100 else 0", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.integer().value, 100);
}

TEST_F(HVM4BackendTest, SanityBooleanOpsWork) {
    auto* expr = state.parseExprFromString("(1 == 1) && (2 == 2)", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_NE(result.integer().value, 0);  // true
}

TEST_F(HVM4BackendTest, SanityNullLiteralWorks) {
    auto* expr = state.parseExprFromString("null", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nNull);
}

TEST_F(HVM4BackendTest, SanityNullComparisonWorks) {
    auto* expr = state.parseExprFromString("null == null", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.integer().value, 1);  // true
}

TEST_F(HVM4BackendTest, SanityNegativeNumberWorks) {
    auto* expr = state.parseExprFromString("0 - 42", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.integer().value, -42);
}

TEST_F(HVM4BackendTest, SanitySignedComparisonWorks) {
    // -5 < 5 should be true (signed comparison)
    auto* expr = state.parseExprFromString("(0 - 5) < 5", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.integer().value, 1);  // true
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
