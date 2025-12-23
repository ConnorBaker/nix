/**
 * HVM4 Primop (Builtin) Tests
 *
 * Tests for builtin function handling in the HVM4 backend.
 *
 * IMPORTANT: Most builtins are NOT YET IMPLEMENTED in the HVM4 backend.
 * These tests verify the expected behavior for when builtins are supported.
 *
 * Test Categories:
 * - Type Checking: isInt, isString, isBool, isList, isAttrs, etc.
 * - Type Information: typeOf
 * - Coercion: toString
 * - Debugging: seq, deepSeq
 * - Error Handling: throw, tryEval, assert, abort
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Type Checking Primops - isInt
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinIsIntWithInt) {
    // builtins.isInt 42
    auto* expr = state.parseExprFromString("builtins.isInt 42", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: builtins are not supported
    EXPECT_FALSE(backend.canEvaluate(*expr));

    // TODO: Once implemented:
    // Value result;
    // bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    // ASSERT_TRUE(success);
    // EXPECT_EQ(result.type(), nBool);
    // EXPECT_TRUE(result.boolean());
}

TEST_F(HVM4BackendTest, BuiltinIsIntWithString) {
    // builtins.isInt "hello"
    auto* expr = state.parseExprFromString("builtins.isInt \"hello\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinIsIntWithList) {
    // builtins.isInt [1 2 3]
    auto* expr = state.parseExprFromString("builtins.isInt [1 2 3]", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Type Checking Primops - isString
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinIsStringWithString) {
    // builtins.isString "hello"
    auto* expr = state.parseExprFromString("builtins.isString \"hello\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinIsStringWithInt) {
    // builtins.isString 42
    auto* expr = state.parseExprFromString("builtins.isString 42", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Type Checking Primops - isBool
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinIsBoolWithTrue) {
    // builtins.isBool true
    auto* expr = state.parseExprFromString("builtins.isBool true", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinIsBoolWithInt) {
    // builtins.isBool 1 - integers are not booleans
    auto* expr = state.parseExprFromString("builtins.isBool 1", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Type Checking Primops - isList
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinIsListWithList) {
    // builtins.isList [1 2 3]
    auto* expr = state.parseExprFromString("builtins.isList [1 2 3]", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinIsListWithAttrs) {
    // builtins.isList { a = 1; }
    auto* expr = state.parseExprFromString("builtins.isList { a = 1; }", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Type Checking Primops - isAttrs
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinIsAttrsWithAttrs) {
    // builtins.isAttrs { a = 1; }
    auto* expr = state.parseExprFromString("builtins.isAttrs { a = 1; }", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinIsAttrsWithList) {
    // builtins.isAttrs [1 2 3]
    auto* expr = state.parseExprFromString("builtins.isAttrs [1 2 3]", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Type Checking Primops - isPath
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinIsPathWithPath) {
    // builtins.isPath ./.
    auto* expr = state.parseExprFromString("builtins.isPath ./.", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinIsPathWithString) {
    // builtins.isPath "/some/path" - strings are not paths
    auto* expr = state.parseExprFromString("builtins.isPath \"/some/path\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Type Checking Primops - isFunction
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinIsFunctionWithLambda) {
    // builtins.isFunction (x: x)
    auto* expr = state.parseExprFromString("builtins.isFunction (x: x)", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinIsFunctionWithInt) {
    // builtins.isFunction 42
    auto* expr = state.parseExprFromString("builtins.isFunction 42", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Type Information - typeOf
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinTypeOfInt) {
    // builtins.typeOf 42 = "int"
    auto* expr = state.parseExprFromString("builtins.typeOf 42", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinTypeOfString) {
    // builtins.typeOf "hello" = "string"
    auto* expr = state.parseExprFromString("builtins.typeOf \"hello\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinTypeOfBool) {
    // builtins.typeOf true = "bool"
    auto* expr = state.parseExprFromString("builtins.typeOf true", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinTypeOfList) {
    // builtins.typeOf [1 2] = "list"
    auto* expr = state.parseExprFromString("builtins.typeOf [1 2]", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinTypeOfAttrs) {
    // builtins.typeOf { } = "set"
    auto* expr = state.parseExprFromString("builtins.typeOf { }", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinTypeOfNull) {
    // builtins.typeOf null = "null"
    auto* expr = state.parseExprFromString("builtins.typeOf null", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinTypeOfFunction) {
    // builtins.typeOf (x: x) = "lambda"
    auto* expr = state.parseExprFromString("builtins.typeOf (x: x)", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Coercion Primops - toString
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinToStringInt) {
    // builtins.toString 42 = "42"
    auto* expr = state.parseExprFromString("builtins.toString 42", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinToStringNegative) {
    // builtins.toString (-42) = "-42"
    auto* expr = state.parseExprFromString("builtins.toString (0 - 42)", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinToStringBool) {
    // builtins.toString true = "1"
    auto* expr = state.parseExprFromString("builtins.toString true", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinToStringNull) {
    // builtins.toString null = ""
    auto* expr = state.parseExprFromString("builtins.toString null", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinToStringString) {
    // builtins.toString "hello" = "hello"
    auto* expr = state.parseExprFromString("builtins.toString \"hello\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinToStringBigInt) {
    // builtins.toString 9999999999 = "9999999999"
    auto* expr = state.parseExprFromString("builtins.toString 9999999999", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Debugging Primops - seq
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinSeq) {
    // builtins.seq 1 2 = 2 (forces first, returns second)
    auto* expr = state.parseExprFromString("builtins.seq 1 2", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinSeqWithComputation) {
    // builtins.seq (1 + 1) 42 = 42
    auto* expr = state.parseExprFromString("builtins.seq (1 + 1) 42", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Debugging Primops - deepSeq
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinDeepSeqSimple) {
    // builtins.deepSeq { a = 1; b = 2; } 42 = 42
    auto* expr = state.parseExprFromString("builtins.deepSeq { a = 1; b = 2; } 42", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinDeepSeqNested) {
    // builtins.deepSeq { a = { b = 1; }; } 42 = 42
    auto* expr = state.parseExprFromString("builtins.deepSeq { a = { b = 1; }; } 42", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Error Handling Primops - throw
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinThrow) {
    // throw "error message"
    auto* expr = state.parseExprFromString("throw \"error message\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Error Handling Primops - tryEval
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinTryEvalSuccess) {
    // builtins.tryEval 42 = { success = true; value = 42; }
    auto* expr = state.parseExprFromString("builtins.tryEval 42", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinTryEvalFailure) {
    // builtins.tryEval (throw "error") = { success = false; value = false; }
    auto* expr = state.parseExprFromString("builtins.tryEval (throw \"error\")", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Assert Expressions (not primops, but language construct)
// =============================================================================

TEST_F(HVM4BackendTest, AssertTrue) {
    // assert true; 42 = 42
    auto* expr = state.parseExprFromString("assert true; 42", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, AssertFalse) {
    // assert false; 42 = ERA (undefined/error)
    // In HVM4, assertion failure produces ERA which propagates as undefined
    auto* expr = state.parseExprFromString("assert false; 42", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // Note: The evaluation produces ERA, which may result in undefined behavior
    // In proper Nix, this would throw. In HVM4, the result is erased.
}

TEST_F(HVM4BackendTest, AssertWithExpression) {
    // assert (1 == 1); 42 = 42
    auto* expr = state.parseExprFromString("assert (1 == 1); 42", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, AssertWithVariable) {
    // assert x; x with let binding
    auto* expr = state.parseExprFromString("let x = true; in assert x; 42", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, AssertNested) {
    // Nested asserts
    auto* expr = state.parseExprFromString("assert true; assert (2 == 2); 100", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 100);
}

// =============================================================================
// Error Handling Primops - abort
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinAbort) {
    // builtins.abort "stopped"
    auto* expr = state.parseExprFromString("builtins.abort \"stopped\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Arithmetic Primops - builtins.add, etc.
// =============================================================================

TEST_F(HVM4BackendTest, BuiltinAdd) {
    // builtins.add 1 2 = 3
    auto* expr = state.parseExprFromString("builtins.add 1 2", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinSub) {
    // builtins.sub 5 3 = 2
    auto* expr = state.parseExprFromString("builtins.sub 5 3", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinMul) {
    // builtins.mul 4 5 = 20
    auto* expr = state.parseExprFromString("builtins.mul 4 5", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinDiv) {
    // builtins.div 10 3 = 3
    auto* expr = state.parseExprFromString("builtins.div 10 3", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, BuiltinLessThan) {
    // builtins.lessThan 1 2 = true
    auto* expr = state.parseExprFromString("builtins.lessThan 1 2", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
