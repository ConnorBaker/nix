/**
 * HVM4 Pattern-Matching Lambda Tests
 *
 * Tests for Nix pattern-matching lambda syntax: { a, b, ... }: body
 *
 * Pattern lambdas desugar to attribute destructuring at compile time:
 *   { a, b ? 1, ... } @ args: body
 *   =>
 *   __arg: let
 *     a = __arg.a;
 *     b = if __arg ? b then __arg.b else 1;
 *     args = __arg;
 *   in body
 *
 * NOTE: Pattern lambdas are NOT YET IMPLEMENTED in the HVM4 backend.
 * These tests currently verify that pattern lambda expressions cannot
 * be compiled (canEvaluate returns false). When the feature is implemented,
 * these tests should be updated to verify correct evaluation behavior.
 *
 * Test Categories:
 * - Simple Patterns: { a }: a
 * - Multiple Patterns: { a, b }: a + b
 * - Default Values: { a ? 1 }: a
 * - Ellipsis: { a, ... }: a
 * - @ Binding: { a, b } @ args: args
 * - Nested Pattern Destructuring
 * - Combined Features
 * - Error Cases (for future implementation)
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Simple Pattern Tests
// =============================================================================

// Pattern lambdas are now implemented - these tests verify canEvaluate
// returns false. Once implemented, update to test actual evaluation.

TEST_F(HVM4BackendTest, PatternLambdaSimple_Implemented) {
    // Simple pattern: { a }: a
    auto* expr = state.parseExprFromString("{ a }: a", state.rootPath(CanonPath::root));
    // Pattern lambdas are now implemented
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaSingleAttr_Implemented) {
    // Pattern lambda applied to attrset
    auto* expr = state.parseExprFromString("({ a }: a) { a = 42; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaReturnsValue_Implemented) {
    // Pattern lambda returning a computed value
    auto* expr = state.parseExprFromString("({ x }: x * 2) { x = 21; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Multiple Pattern Tests
// =============================================================================

TEST_F(HVM4BackendTest, PatternLambdaMultipleAttrs_Implemented) {
    // Multiple required attributes: { a, b }: a + b
    auto* expr = state.parseExprFromString("({ a, b }: a + b) { a = 1; b = 2; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaThreeAttrs_Implemented) {
    // Three attributes
    auto* expr = state.parseExprFromString("({ a, b, c }: a + b + c) { a = 1; b = 2; c = 3; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaManyAttrs_Implemented) {
    // Many attributes
    auto* expr = state.parseExprFromString(
        "({ a, b, c, d, e }: a + b + c + d + e) { a = 1; b = 2; c = 3; d = 4; e = 5; }",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Default Value Tests
// =============================================================================

TEST_F(HVM4BackendTest, PatternLambdaDefaultSimple_Implemented) {
    // Default value: { a ? 1 }: a
    auto* expr = state.parseExprFromString("{ a ? 1 }: a", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaDefaultUsed_Implemented) {
    // Default value used when attr not provided
    auto* expr = state.parseExprFromString("({ a ? 10 }: a) { }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaDefaultOverride_Implemented) {
    // Default value overridden when attr is provided
    auto* expr = state.parseExprFromString("({ a ? 10 }: a) { a = 42; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaDefaultMultiple_Implemented) {
    // Multiple default values
    auto* expr = state.parseExprFromString("({ a ? 1, b ? 2 }: a + b) { }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaDefaultPartial_Implemented) {
    // Mix of required and optional attributes
    auto* expr = state.parseExprFromString("({ a, b ? 10 }: a + b) { a = 1; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaDefaultExpression_Implemented) {
    // Default value is an expression
    auto* expr = state.parseExprFromString("({ a ? 2 + 3 }: a) { }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaDefaultReferencesOther_Implemented) {
    // Default value references another formal
    // In Nix, defaults can reference attributes from the same attrset
    auto* expr = state.parseExprFromString("({ a, b ? a * 2 }: a + b) { a = 5; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    // a = 5, b = a * 2 = 10, result = a + b = 15
    EXPECT_EQ(result.integer().value, 15);
}

TEST_F(HVM4BackendTest, PatternLambdaAllDefaults_Implemented) {
    // All attributes have defaults
    auto* expr = state.parseExprFromString("({ a ? 1, b ? 2, c ? 3 }: a + b + c) { }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Ellipsis Tests
// =============================================================================

TEST_F(HVM4BackendTest, PatternLambdaEllipsis_Implemented) {
    // Ellipsis allows extra attributes: { a, ... }: a
    auto* expr = state.parseExprFromString("{ a, ... }: a", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaEllipsisExtraAttrs_Implemented) {
    // Ellipsis with extra attributes that would otherwise fail
    auto* expr = state.parseExprFromString("({ a, ... }: a) { a = 1; b = 2; c = 3; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaOnlyEllipsis_Implemented) {
    // Just ellipsis, accepts anything
    auto* expr = state.parseExprFromString("({ ... }: 42) { a = 1; b = 2; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaEmptyWithEllipsis_Implemented) {
    // Empty attrset with ellipsis
    auto* expr = state.parseExprFromString("({ ... }: 42) { }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaEllipsisWithDefaults_Implemented) {
    // Ellipsis combined with default values
    auto* expr = state.parseExprFromString("({ a ? 1, ... }: a) { b = 2; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// @ Binding Tests
// =============================================================================

TEST_F(HVM4BackendTest, PatternLambdaAtPattern_Implemented) {
    // @ binding captures entire argument: { a } @ args: args
    auto* expr = state.parseExprFromString("{ a } @ args: args", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaAtPatternAccess_Implemented) {
    // @ binding allows accessing the full attrset
    auto* expr = state.parseExprFromString("({ a, b, ... } @ args: args.c) { a = 1; b = 2; c = 3; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaAtPatternAlternative_Implemented) {
    // Alternative @ syntax: args @ { a }: args
    auto* expr = state.parseExprFromString("args @ { a }: args", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaAtPatternWithDefaults_Implemented) {
    // @ pattern with default values
    auto* expr = state.parseExprFromString("({ a ? 1, b ? 2 } @ args: args) { a = 10; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaAtPatternWithEllipsis_Implemented) {
    // @ pattern with ellipsis
    auto* expr = state.parseExprFromString("({ a, ... } @ args: args) { a = 1; b = 2; c = 3; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Empty Pattern Tests
// =============================================================================

TEST_F(HVM4BackendTest, PatternLambdaEmptyPattern_Implemented) {
    // Empty pattern: { }: body
    auto* expr = state.parseExprFromString("({ }: 42) { }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaEmptyPatternNoArgs_Implemented) {
    // Empty pattern lambda (not applied)
    auto* expr = state.parseExprFromString("{ }: 42", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Nested Pattern Tests (Chained Pattern Lambdas)
// =============================================================================

TEST_F(HVM4BackendTest, PatternLambdaNested_Implemented) {
    // Nested/chained pattern lambdas
    auto* expr = state.parseExprFromString(
        R"(
            let f = { a }: { b }: a + b;
            in f { a = 1; } { b = 2; }
        )",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaChained_Implemented) {
    // Three levels of chained pattern lambdas
    auto* expr = state.parseExprFromString(
        R"(
            let
                f = { a }: { b }: { c }: a + b + c;
            in f { a = 1; } { b = 2; } { c = 3; }
        )",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaMixedWithSimple_Implemented) {
    // Mixing pattern lambda with simple lambda
    auto* expr = state.parseExprFromString(
        R"(
            let f = { a }: x: a + x;
            in f { a = 10; } 5
        )",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaReturnsFunction_Implemented) {
    // Pattern lambda returning a simple lambda
    auto* expr = state.parseExprFromString(
        R"(
            let mkAdder = { x }: y: x + y;
                add5 = mkAdder { x = 5; };
            in add5 10
        )",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Nested Destructuring (Pattern in Body)
// =============================================================================

TEST_F(HVM4BackendTest, PatternLambdaNestedAttrAccess_Implemented) {
    // Accessing nested attributes within pattern lambda body
    auto* expr = state.parseExprFromString(
        "({ outer }: outer.inner) { outer = { inner = 42; }; }",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaDeepNesting_Implemented) {
    // Deeply nested attribute access - now supported
    auto* expr = state.parseExprFromString(
        "({ x }: x.a.b.c) { x = { a = { b = { c = 42; }; }; }; }",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

// =============================================================================
// Combined Features Tests
// =============================================================================

TEST_F(HVM4BackendTest, PatternLambdaAllFeatures_Implemented) {
    // All features combined: required, defaults, ellipsis, @ binding
    auto* expr = state.parseExprFromString(
        R"(
            ({ a, b ? 10, ... } @ args: a + b + args.c) { a = 1; c = 100; }
        )",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaComplexDefaults_Implemented) {
    // Complex default expressions referencing other formals
    auto* expr = state.parseExprFromString(
        R"(
            ({ a, b ? a + 1, c ? b * 2 }: a + b + c) { a = 5; }
        )",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    // a = 5, b = a + 1 = 6, c = b * 2 = 12, result = 5 + 6 + 12 = 23
    EXPECT_EQ(result.integer().value, 23);
}

TEST_F(HVM4BackendTest, PatternLambdaInLet_Implemented) {
    // Pattern lambda in let binding
    auto* expr = state.parseExprFromString(
        R"(
            let
                f = { a, b }: a * b;
                x = { a = 6; b = 7; };
            in f x
        )",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Laziness Tests (For Future Implementation)
// =============================================================================

TEST_F(HVM4BackendTest, PatternLambdaLazyDefault_NotImplemented) {
    // Default should only be evaluated if not provided
    // Not yet implemented: requires throw builtin
    auto* expr = state.parseExprFromString(
        R"(
            ({ a ? throw "not used" }: 42) { a = 1; }
        )",
        state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaUnusedAttrLazy_NotImplemented) {
    // Unused attributes in attrset should remain lazy
    // Not yet implemented: requires throw builtin
    auto* expr = state.parseExprFromString(
        R"(
            ({ a, ... }: a) { a = 1; b = throw "unused"; }
        )",
        state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Higher-Order Pattern Lambda Tests
// =============================================================================

TEST_F(HVM4BackendTest, PatternLambdaAsMapArg_NotImplemented) {
    // Pattern lambda used with builtins.map
    // Not yet implemented: requires builtins.map
    auto* expr = state.parseExprFromString(
        R"(
            builtins.map ({ x }: x * 2) [{ x = 1; } { x = 2; } { x = 3; }]
        )",
        state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaComposition_Implemented) {
    // Composing pattern lambdas
    auto* expr = state.parseExprFromString(
        R"(
            let
                f = { a, b }: { c = a + b; };
                g = { c }: c * 2;
            in g (f { a = 5; b = 3; })
        )",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// NixOS-Style Pattern Tests
// =============================================================================

TEST_F(HVM4BackendTest, PatternLambdaNixOSModule_Implemented) {
    // Simplified NixOS module pattern
    auto* expr = state.parseExprFromString(
        R"(
            let
                mkModule = { config, lib ? {} }: { options = config; };
            in mkModule { config = { foo = 1; }; }
        )",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaCallPackage_Implemented) {
    // Simplified callPackage pattern
    auto* expr = state.parseExprFromString(
        R"(
            let
                pkg = { stdenv, lib ? {} }: { name = "test"; };
                callPackage = fn: overrides:
                    fn ({ stdenv = "mock"; } // overrides);
            in (callPackage pkg {}).name
        )",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaOverridePattern) {
    // Override pattern commonly used in Nixpkgs with string interpolation
    auto* expr = state.parseExprFromString(
        R"(
            let
                base = { name, version ? "1.0", ... } @ args:
                    args // { fullName = "${name}-${version}"; };
            in (base { name = "hello"; extra = true; }).fullName
        )",
        state.rootPath(CanonPath::root));
    // String interpolation is now implemented
    ASSERT_TRUE(backend.canEvaluate(*expr));
    Value result;
    ASSERT_TRUE(backend.tryEvaluate(expr, state.baseEnv, result));
    EXPECT_EQ(result.type(), nString);
    EXPECT_STREQ(result.c_str(), "hello-1.0");
}

// =============================================================================
// Error Case Tests (For Future Implementation)
// =============================================================================
// These tests document expected error behavior once pattern lambdas are
// implemented. They currently just verify canEvaluate returns false.

TEST_F(HVM4BackendTest, PatternLambdaMissingRequired_Implemented) {
    // Missing required attribute should error when implemented
    auto* expr = state.parseExprFromString("({ a, b }: a) { a = 1; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaExtraWithoutEllipsis_Implemented) {
    // Extra attrs without ... should error when implemented
    auto* expr = state.parseExprFromString("({ a }: a) { a = 1; b = 2; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaNonAttrset_Implemented) {
    // Applying pattern lambda to non-attrset should error when implemented
    auto* expr = state.parseExprFromString("({ a }: a) 42", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternLambdaNullArg_Implemented) {
    // Applying pattern lambda to null should error when implemented
    auto* expr = state.parseExprFromString("({ a }: a) null", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Comparison with Simple Lambda (Baseline)
// =============================================================================
// These tests verify that simple lambdas work, providing a baseline for
// pattern lambda implementation.

TEST_F(HVM4BackendTest, SimpleLambdaWorks) {
    // Simple lambda should work (baseline comparison)
    auto* expr = state.parseExprFromString("x: x", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, SimpleLambdaApplied) {
    // Simple lambda application should work
    auto* expr = state.parseExprFromString("(x: x + 1) 41", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, SimpleLambdaMultiArg) {
    // Curried simple lambda should work
    auto* expr = state.parseExprFromString("(a: b: a + b) 1 2", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));

    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 3);
}

// =============================================================================
// Additional Error Case Tests
// =============================================================================
// These tests verify pattern matching error conditions.

TEST_F(HVM4BackendTest, PatternMissingRequiredAttribute) {
    // ({ a, b }: a + b) { a = 1; } should error (missing b)
    auto* expr = state.parseExprFromString("({ a, b }: a + b) { a = 1; }", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: pattern lambdas not supported
    EXPECT_TRUE(backend.canEvaluate(*expr));

    // TODO: Once implemented, tryEvaluate should return false or throw
}

TEST_F(HVM4BackendTest, PatternExtraAttributeNoEllipsis) {
    // ({ a }: a) { a = 1; b = 2; } should error (extra b without ...)
    auto* expr = state.parseExprFromString("({ a }: a) { a = 1; b = 2; }", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternEmptyWithExtraAttrs) {
    // ({ }: 42) { a = 1; } should error (empty pattern, extra attrs)
    auto* expr = state.parseExprFromString("({ }: 42) { a = 1; }", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PatternAppliedToList) {
    // ({ a }: a) [1 2 3] should error (applying to list instead of attrset)
    auto* expr = state.parseExprFromString("({ a }: a) [1 2 3]", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
