/**
 * HVM4 Backend Session Tests (Sessions 14-19)
 *
 * Integration and edge case tests for the HVM4 backend.
 * These tests combine multiple features and verify edge cases across
 * all supported expression types.
 *
 * For individual feature tests, see:
 * - hvm4-capability-test.cc: CanEvaluate/CannotEvaluate tests
 * - hvm4-arithmetic-test.cc: Arithmetic operator tests
 * - hvm4-comparison-test.cc: Comparison operator tests
 * - hvm4-boolean-test.cc: Boolean operator tests
 * - hvm4-conditional-test.cc: If-then-else tests
 * - hvm4-lambda-test.cc: Lambda and application tests
 * - hvm4-let-test.cc: Let binding tests
 * - hvm4-closure-test.cc: Closure and higher-order function tests
 *
 * For stress/extended tests, see:
 * - hvm4-stress-test.cc: Stress and performance tests
 * - hvm4-runtime-test.cc: Runtime behavior tests
 * - hvm4-bigint-test.cc: BigInt encoding tests
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Session 14: Refinement Tests
// =============================================================================

// --- Backend Combination Refinements ---

// Multiple additions in a single let binding value
TEST_F(HVM4BackendTest, RefinementLetWithChainedAddition) {
    auto* expr = state.parseExprFromString(
        "let x = 1 + 2 + 3 + 4; in x",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 10);
}

// Nested equality checks
TEST_F(HVM4BackendTest, RefinementNestedEqualityChecks) {
    auto* expr = state.parseExprFromString(
        "let a = 1 == 1; b = 2 == 2; in if a == b then 100 else 0",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 100);
}

// Boolean operations with computed values
TEST_F(HVM4BackendTest, RefinementBooleanWithComputed) {
    auto* expr = state.parseExprFromString(
        "let x = 5; y = 10; in (x == 5) && (y == 10)",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1);
}

// Deeply nested function application
TEST_F(HVM4BackendTest, RefinementDeepFunctionNesting) {
    auto* expr = state.parseExprFromString(
        "(a: b: c: d: a + b + c + d) 1 2 3 4",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 10);
}

// Let binding with conditional value
TEST_F(HVM4BackendTest, RefinementLetWithConditionalValue) {
    auto* expr = state.parseExprFromString(
        "let x = if 1 == 1 then 42 else 0; in x + 8",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 50);
}

// Multiple independent let bindings used together
TEST_F(HVM4BackendTest, RefinementMultipleIndependentBindings) {
    auto* expr = state.parseExprFromString(
        "let a = 100; b = 200; c = 300; d = 400; in a + b + c + d",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1000);
}

// Conditional returning function result
TEST_F(HVM4BackendTest, RefinementConditionalWithFunctionResult) {
    auto* expr = state.parseExprFromString(
        "if (x: x + 10) 5 == 15 then 1 else 0",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1);
}

// Not of inequality
TEST_F(HVM4BackendTest, RefinementNotOfInequality) {
    auto* expr = state.parseExprFromString(
        "!(1 != 1)",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1);
}

// Or with both sides false
TEST_F(HVM4BackendTest, RefinementOrBothFalse) {
    auto* expr = state.parseExprFromString(
        "(1 == 2) || (3 == 4)",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 0);
}

// And with both sides true
TEST_F(HVM4BackendTest, RefinementAndBothTrue) {
    auto* expr = state.parseExprFromString(
        "(1 == 1) && (2 == 2)",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1);
}

// Lambda returning conditional
TEST_F(HVM4BackendTest, RefinementLambdaReturningConditional) {
    auto* expr = state.parseExprFromString(
        "(x: if x == 0 then 1 else x) 0",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1);
}

// Lambda with computed body
TEST_F(HVM4BackendTest, RefinementLambdaWithComputedBody) {
    auto* expr = state.parseExprFromString(
        "(x: (1 + 2 + 3) + x) 4",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 10);
}

// =============================================================================
// Session 15: Additional Refinement Tests
// =============================================================================

// --- Zero Edge Cases ---

// Zero in all positions of addition chain
TEST_F(HVM4BackendTest, Session15ZeroChain) {
    auto* expr = state.parseExprFromString(
        "0 + 0 + 0 + 0",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 0);
}

// Zero as lambda argument
TEST_F(HVM4BackendTest, Session15ZeroAsArgument) {
    auto* expr = state.parseExprFromString(
        "(x: x + x + x) 0",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 0);
}

// Zero in conditional comparison
TEST_F(HVM4BackendTest, Session15ZeroInConditional) {
    auto* expr = state.parseExprFromString(
        "if 0 == 0 then 1 else 2",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1);
}

// --- Nested Boolean Operations ---

// Triple AND
TEST_F(HVM4BackendTest, Session15TripleAnd) {
    auto* expr = state.parseExprFromString(
        "(1 == 1) && (2 == 2) && (3 == 3)",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1);
}

// Triple OR
TEST_F(HVM4BackendTest, Session15TripleOr) {
    auto* expr = state.parseExprFromString(
        "(1 == 2) || (2 == 3) || (3 == 3)",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1);
}

// Mixed AND/OR with parentheses
TEST_F(HVM4BackendTest, Session15MixedBooleanWithParens) {
    auto* expr = state.parseExprFromString(
        "((1 == 1) && (2 == 3)) || ((3 == 3) && (4 == 4))",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1);
}

// --- Complex Lambda Patterns ---

// Lambda with all parameters used
TEST_F(HVM4BackendTest, Session15AllParametersUsed) {
    auto* expr = state.parseExprFromString(
        "(a: b: c: a + b + c) 10 20 30",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 60);
}

// Lambda ignoring some parameters
TEST_F(HVM4BackendTest, Session15IgnoredParameters) {
    auto* expr = state.parseExprFromString(
        "(a: b: c: b) 10 20 30",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 20);
}

// Lambda returning first parameter
TEST_F(HVM4BackendTest, Session15FirstParameter) {
    auto* expr = state.parseExprFromString(
        "(a: b: c: a) 10 20 30",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 10);
}

// Lambda returning last parameter
TEST_F(HVM4BackendTest, Session15LastParameter) {
    auto* expr = state.parseExprFromString(
        "(a: b: c: c) 10 20 30",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 30);
}

// --- Nested Let with Computation ---

// Let with addition in body referencing multiple bindings
TEST_F(HVM4BackendTest, Session15LetMultipleRefs) {
    auto* expr = state.parseExprFromString(
        "let a = 5; b = 10; c = 15; in a + b + c + a + b + c",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 60);
}

// Let with conditional in binding
TEST_F(HVM4BackendTest, Session15LetConditionalBinding) {
    auto* expr = state.parseExprFromString(
        "let x = if 1 == 1 then 100 else 0; y = if 1 == 2 then 100 else 50; in x + y",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 150);
}


// =============================================================================
// Session 17: Final Edge Cases and Documentation Tests
// =============================================================================

// --- Comprehensive Integration Tests ---

// Full expression combining all supported features
TEST_F(HVM4BackendTest, Session17FullIntegration) {
    auto* expr = state.parseExprFromString(
        "let a = 10; b = 20; in (x: y: if (x == a) && (y == b) then x + y else 0) 10 20",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 30);
}

// Deeply nested boolean expression
TEST_F(HVM4BackendTest, Session17DeepBooleanNesting) {
    auto* expr = state.parseExprFromString(
        "!(!(!(!( 1 == 1 ))))",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1);
}

// --- Specific Value Tests ---

// Maximum 32-bit value in expression
TEST_F(HVM4BackendTest, Session17MaxInt32InExpr) {
    auto* expr = state.parseExprFromString(
        "2147483647",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 2147483647);
}

// Large computed value
TEST_F(HVM4BackendTest, Session17LargeComputedValue) {
    auto* expr = state.parseExprFromString(
        "1000000000 + 1000000000",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 2000000000);
}

// --- Identity and Constant Functions ---

// K combinator pattern
TEST_F(HVM4BackendTest, Session17KCombinator) {
    auto* expr = state.parseExprFromString(
        "(x: y: x) 42 99",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 42);
}

// KI combinator pattern (flip of K)
TEST_F(HVM4BackendTest, Session17KICombinator) {
    auto* expr = state.parseExprFromString(
        "(x: y: y) 42 99",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 99);
}

// --- Short-Circuit Evaluation ---

// AND short-circuit (first false)
TEST_F(HVM4BackendTest, Session17AndShortCircuit) {
    auto* expr = state.parseExprFromString(
        "(1 == 2) && (3 == 3)",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 0);
}

// OR short-circuit (first true)
TEST_F(HVM4BackendTest, Session17OrShortCircuit) {
    auto* expr = state.parseExprFromString(
        "(1 == 1) || (2 == 3)",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1);
}

// --- Expression in Various Positions ---

// Lambda as conditional result
TEST_F(HVM4BackendTest, Session17LambdaInCondResult) {
    auto* expr = state.parseExprFromString(
        "(if 1 == 1 then (x: x + 1) else (x: x)) 10",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 11);
}

// Conditional as lambda body
TEST_F(HVM4BackendTest, Session17CondAsLambdaBody) {
    auto* expr = state.parseExprFromString(
        "(x: if x == 0 then 100 else x + 50) 0",
        state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 100);
}

// =============================================================================
// Session 18: Documentation and Completeness Tests
// =============================================================================

// --- Verify Core Functionality ---

// Simplest possible integer
TEST_F(HVM4BackendTest, Session18SimpleInteger) {
    auto* expr = state.parseExprFromString("0", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 0);
}

// Simplest possible addition
TEST_F(HVM4BackendTest, Session18SimpleAddition) {
    auto* expr = state.parseExprFromString("0 + 0", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 0);
}

// Simplest possible lambda
TEST_F(HVM4BackendTest, Session18SimpleLambda) {
    auto* expr = state.parseExprFromString("(x: x) 1", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1);
}

// Simplest possible let
TEST_F(HVM4BackendTest, Session18SimpleLet) {
    auto* expr = state.parseExprFromString("let x = 1; in x", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1);
}

// Simplest possible conditional
TEST_F(HVM4BackendTest, Session18SimpleConditional) {
    auto* expr = state.parseExprFromString("if 1 == 1 then 1 else 0", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 1);
}

// --- Final Edge Cases ---

// Empty-ish expression: just return bound value
TEST_F(HVM4BackendTest, Session18JustBoundValue) {
    auto* expr = state.parseExprFromString("let result = 42; in result", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 42);
}

// Verify false branch works
TEST_F(HVM4BackendTest, Session18FalseBranchTaken) {
    auto* expr = state.parseExprFromString("if 1 == 2 then 100 else 200", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 200);
}

// Double application
TEST_F(HVM4BackendTest, Session18DoubleApplication) {
    auto* expr = state.parseExprFromString("(f: x: f x) (y: y + 1) 5", state.rootPath(CanonPath::root));
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.type(), nInt);
    EXPECT_EQ(v.integer().value, 6);
}

// =============================================================================
// Session 19: Additional Edge Case and Stress Tests
// =============================================================================

// --- Arithmetic Edge Cases ---

TEST_F(HVM4BackendTest, Session19ZeroAddition) {
    auto* expr = state.parseExprFromString("0 + 0", state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 0);
}

TEST_F(HVM4BackendTest, Session19IdentityAddZero) {
    auto* expr = state.parseExprFromString("42 + 0", state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, Session19AdditionCommutativity) {
    // a + b should equal b + a
    auto* expr1 = state.parseExprFromString("3 + 7", state.rootPath(CanonPath::root));
    auto* expr2 = state.parseExprFromString("7 + 3", state.rootPath(CanonPath::root));
    Value v1, v2;
    ASSERT_TRUE(backend.tryEvaluate(expr1, state.baseEnv, v1));
    ASSERT_TRUE(backend.tryEvaluate(expr2, state.baseEnv, v2));
    EXPECT_EQ(v1.integer().value, v2.integer().value);
}

TEST_F(HVM4BackendTest, Session19AdditionAssociativity) {
    // (a + b) + c should equal a + (b + c)
    auto* expr1 = state.parseExprFromString("(1 + 2) + 3", state.rootPath(CanonPath::root));
    auto* expr2 = state.parseExprFromString("1 + (2 + 3)", state.rootPath(CanonPath::root));
    Value v1, v2;
    ASSERT_TRUE(backend.tryEvaluate(expr1, state.baseEnv, v1));
    ASSERT_TRUE(backend.tryEvaluate(expr2, state.baseEnv, v2));
    EXPECT_EQ(v1.integer().value, 6);
    EXPECT_EQ(v2.integer().value, 6);
}

TEST_F(HVM4BackendTest, Session19NegativeResult) {
    // Results in negative (needs proper signed handling)
    auto* expr = state.parseExprFromString("let a = 5; b = 10; in a + (0 + (0 + (0 + (0 + 0))))", state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 5);
}

// --- Boolean Edge Cases ---

TEST_F(HVM4BackendTest, Session19DoubleNegation) {
    // !!x should equal x
    auto* expr = state.parseExprFromString("!!(1 == 1)", state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, Session19TripleNegation) {
    auto* expr = state.parseExprFromString("!!!(1 == 1)", state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 0);
}

TEST_F(HVM4BackendTest, Session19DeMorganAnd) {
    // !(a && b) should behave like !a || !b
    // Test: !(true && false) = true
    auto* expr = state.parseExprFromString("!((1 == 1) && (1 == 2))", state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, Session19DeMorganOr) {
    // !(a || b) should behave like !a && !b
    // Test: !(false || false) = true
    auto* expr = state.parseExprFromString("!((1 == 2) || (3 == 4))", state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, Session19ComplexBooleanChain) {
    // Complex boolean expression
    auto* expr = state.parseExprFromString(
        "((1 == 1) && (2 == 2)) || ((3 == 4) && (5 == 5))",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 1);  // (true && true) || (false && true) = true
}

// --- Lambda Edge Cases ---

TEST_F(HVM4BackendTest, Session19NestedIdentity) {
    // ((x: x) (y: y)) z = z
    auto* expr = state.parseExprFromString("((x: x) (y: y)) 42", state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, Session19SCombinatorfLike) {
    // S-like combinator: (x: y: z: (x z) (y z))
    // Applied: (f: g: x: f x + g x) (a: a) (b: b + 1) 5 = 5 + 6 = 11
    auto* expr = state.parseExprFromString(
        "(f: g: x: (f x) + (g x)) (a: a) (b: b + 1) 5",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 11);
}

TEST_F(HVM4BackendTest, Session19HigherOrderSelect) {
    // Apply a selector function
    auto* expr = state.parseExprFromString(
        "(sel: a: b: if sel == 1 then a else b) 1 100 200",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 100);
}

TEST_F(HVM4BackendTest, Session19HigherOrderSelectAlt) {
    auto* expr = state.parseExprFromString(
        "(sel: a: b: if sel == 1 then a else b) 0 100 200",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 200);
}

// --- Let Binding Edge Cases ---

TEST_F(HVM4BackendTest, Session19LetShadowing) {
    // Inner binding shadows outer
    auto* expr = state.parseExprFromString(
        "let x = 1; in let x = 2; in x",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 2);
}

TEST_F(HVM4BackendTest, Session19LetShadowingWithOuter) {
    // Use outer after inner scope ends
    auto* expr = state.parseExprFromString(
        "let x = 1; y = (let x = 10; in x); in x + y",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 11);  // 1 + 10
}

TEST_F(HVM4BackendTest, Session19LetChainedDependency) {
    // Each binding depends on previous
    auto* expr = state.parseExprFromString(
        "let a = 1; b = a + 1; c = b + 1; d = c + 1; in d",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 4);
}

TEST_F(HVM4BackendTest, Session19LetMultipleUse) {
    // Same binding used multiple times
    auto* expr = state.parseExprFromString(
        "let x = 5; in x + x + x",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 15);
}

TEST_F(HVM4BackendTest, Session19LetUnusedBinding) {
    // Unused binding should not affect result
    auto* expr = state.parseExprFromString(
        "let unused = 999; used = 42; in used",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 42);
}

// --- Conditional Edge Cases ---

TEST_F(HVM4BackendTest, Session19NestedConditionals) {
    auto* expr = state.parseExprFromString(
        "if 1 == 1 then (if 2 == 2 then (if 3 == 3 then 100 else 0) else 0) else 0",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 100);
}

TEST_F(HVM4BackendTest, Session19ConditionalInAddition) {
    auto* expr = state.parseExprFromString(
        "(if 1 == 1 then 10 else 0) + (if 1 == 2 then 0 else 5)",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 15);
}

TEST_F(HVM4BackendTest, Session19ConditionalWithComputation) {
    auto* expr = state.parseExprFromString(
        "let a = 5; b = 10; in if a + b == 15 then a + b + 1 else 0",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 16);
}

// --- Comparison Edge Cases ---

TEST_F(HVM4BackendTest, Session19EqualityReflexive) {
    // x == x should always be true
    auto* expr = state.parseExprFromString(
        "let x = 42; in x == x",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, Session19EqualitySymmetric) {
    // (a == b) should equal (b == a)
    auto* expr1 = state.parseExprFromString("3 == 5", state.rootPath(CanonPath::root));
    auto* expr2 = state.parseExprFromString("5 == 3", state.rootPath(CanonPath::root));
    Value v1, v2;
    ASSERT_TRUE(backend.tryEvaluate(expr1, state.baseEnv, v1));
    ASSERT_TRUE(backend.tryEvaluate(expr2, state.baseEnv, v2));
    EXPECT_EQ(v1.integer().value, v2.integer().value);
}

TEST_F(HVM4BackendTest, Session19InequalitySymmetric) {
    auto* expr1 = state.parseExprFromString("3 != 5", state.rootPath(CanonPath::root));
    auto* expr2 = state.parseExprFromString("5 != 3", state.rootPath(CanonPath::root));
    Value v1, v2;
    ASSERT_TRUE(backend.tryEvaluate(expr1, state.baseEnv, v1));
    ASSERT_TRUE(backend.tryEvaluate(expr2, state.baseEnv, v2));
    EXPECT_EQ(v1.integer().value, v2.integer().value);
}

TEST_F(HVM4BackendTest, Session19CompareZero) {
    auto* expr = state.parseExprFromString("0 == 0", state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 1);
}

// --- Stress Tests ---

TEST_F(HVM4BackendTest, Session19DeepNesting) {
    // Deeply nested additions
    auto* expr = state.parseExprFromString(
        "((((((((1 + 1) + 1) + 1) + 1) + 1) + 1) + 1) + 1) + 1",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 10);
}

TEST_F(HVM4BackendTest, Session19DeepLambdaNesting) {
    // Deeply nested lambdas
    auto* expr = state.parseExprFromString(
        "(a: b: c: d: e: a + b + c + d + e) 1 2 3 4 5",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 15);
}

TEST_F(HVM4BackendTest, Session19DeepLetNesting) {
    // Deeply nested lets
    auto* expr = state.parseExprFromString(
        "let a = 1; in let b = a + 1; in let c = b + 1; in let d = c + 1; in let e = d + 1; in e",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 5);
}

TEST_F(HVM4BackendTest, Session19ManyBindings) {
    // Many bindings in single let
    auto* expr = state.parseExprFromString(
        "let a = 1; b = 2; c = 3; d = 4; e = 5; f = 6; g = 7; h = 8; i = 9; j = 10; in a + b + c + d + e + f + g + h + i + j",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 55);
}

// --- Closure Tests ---

TEST_F(HVM4BackendTest, Session19ClosureCapturesValue) {
    // Closure captures value from outer scope
    auto* expr = state.parseExprFromString(
        "let x = 10; f = y: x + y; in f 5",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 15);
}

TEST_F(HVM4BackendTest, Session19ClosureMultipleCaptures) {
    auto* expr = state.parseExprFromString(
        "let a = 1; b = 2; f = x: a + b + x; in f 3",
        state.rootPath(CanonPath::root));
    Value v;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, v));
    EXPECT_EQ(v.integer().value, 6);
}

// --- String Tests (previously rejection, now acceptance) ---

TEST_F(HVM4BackendTest, Session19AcceptString) {
    // String literals are now supported
    auto* expr = state.parseExprFromString("\"hello\"", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello");
}

TEST_F(HVM4BackendTest, Session19AcceptList) {
    auto* expr = state.parseExprFromString("[1 2 3]", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 3);
}

TEST_F(HVM4BackendTest, Session19AcceptAttrset) {
    // Attribute sets are now supported
    auto* expr = state.parseExprFromString("{ a = 1; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, Session19AcceptPatternLambda) {
    // Pattern lambdas are now supported
    auto* expr = state.parseExprFromString("{ a, b }: a + b", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, Session19RejectRecursiveLet) {
    auto* expr = state.parseExprFromString("let x = x; in x", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, Session19RejectBuiltinCall) {
    auto* expr = state.parseExprFromString("builtins.add 1 2", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// Note: RejectFreeVariable test removed - Nix parser validates variable
// references during parsing, so we can't test canEvaluate rejection of
// free variables (they throw during parse, not during canEvaluate).

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
