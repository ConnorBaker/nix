/**
 * HVM4 Path Tests
 *
 * Tests for Nix path expressions in the HVM4 backend.
 * Paths in Nix have special semantics:
 * - They reference files via a SourceAccessor (virtual filesystem)
 * - When coerced to strings, they are copied to the store and gain context
 * - Path concatenation with + operator creates new paths
 *
 * Path support IS IMPLEMENTED using the "Pure Path Representation" approach:
 * - Paths are represented as #Pth{accessor_id, path_string_id}
 * - Store operations are deferred to result extraction time
 * - HVM4 evaluation remains pure and deterministic
 *
 * Note: Some operations like path interpolation in strings (store coercion)
 * and path concatenation are not yet implemented.
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Path Compilation Capability Tests
// =============================================================================
// These tests verify that path expressions can be compiled by HVM4.

TEST_F(HVM4BackendTest, CanEvaluateAbsolutePath) {
    // Absolute path literal: /foo/bar
    auto* expr = state.parseExprFromString("/foo/bar", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateRelativePathDot) {
    // Relative path with dot: ./foo
    // This is the most common path form in Nix
    auto* expr = state.parseExprFromString("./foo", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateRelativePathDotDot) {
    // Relative path with parent reference: ../foo
    auto* expr = state.parseExprFromString("../foo", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateCurrentDir) {
    // Current directory path: ./.
    auto* expr = state.parseExprFromString("./.", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluatePathWithExtension) {
    // Path with file extension: ./foo.nix
    auto* expr = state.parseExprFromString("./foo.nix", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateDeepPath) {
    // Deeply nested path: ./foo/bar/baz/qux
    auto* expr = state.parseExprFromString("./foo/bar/baz/qux", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Path in Binding Context Tests
// =============================================================================
// Paths used in let bindings and other binding contexts

TEST_F(HVM4BackendTest, CanEvaluatePathInLet) {
    // Path assigned to a variable
    // let p = ./foo; in p
    auto* expr = state.parseExprFromString("let p = ./foo; in p", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluatePathInNestedLet) {
    // Path in nested let bindings
    auto* expr = state.parseExprFromString(
        "let outer = let inner = ./foo; in inner; in outer",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluatePathPassedToLambda) {
    // Path passed as argument to lambda
    auto* expr = state.parseExprFromString("(p: p) ./foo", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluatePathInLambdaBody) {
    // Path referenced in lambda body
    auto* expr = state.parseExprFromString("let f = x: ./foo; in f 1", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Path Concatenation Tests
// =============================================================================
// Path + string concatenation creates a new path
// NOTE: Path concatenation is NOT YET IMPLEMENTED - these tests expect failure

TEST_F(HVM4BackendTest, CannotEvaluatePathConcatString) {
    // Basic path concatenation: ./foo + "/bar"
    // Result should be a path, not a string
    // Not implemented yet - requires special ExprConcatStrings handling for paths
    auto* expr = state.parseExprFromString("./foo + \"/bar\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluatePathConcatMultiple) {
    // Multiple concatenations: ./foo + "/bar" + "/baz"
    auto* expr = state.parseExprFromString("./foo + \"/bar\" + \"/baz\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluatePathConcatWithVariable) {
    // Path concatenation with variable: let suffix = "/bar"; in ./foo + suffix
    // Not implemented yet - ExprConcatStrings with path first element not supported
    auto* expr = state.parseExprFromString(
        "let suffix = \"/bar\"; in ./foo + suffix",
        state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateAbsolutePathConcat) {
    // Absolute path concatenation
    auto* expr = state.parseExprFromString("/foo + \"/bar\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Path to String Coercion Tests
// =============================================================================
// When paths are coerced to strings, they are copied to the store

TEST_F(HVM4BackendTest, CannotEvaluatePathInterpolation) {
    // Path in string interpolation: "${./foo}"
    // This coerces the path to a string (with store copy)
    auto* expr = state.parseExprFromString("\"${./foo}\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluatePathInterpolationWithPrefix) {
    // Path interpolation with prefix text
    auto* expr = state.parseExprFromString("\"prefix-${./foo}\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluatePathInterpolationWithSuffix) {
    // Path interpolation with suffix text
    auto* expr = state.parseExprFromString("\"${./foo}-suffix\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluatePathInterpolationWithBoth) {
    // Path interpolation with both prefix and suffix
    auto* expr = state.parseExprFromString("\"prefix-${./foo}-suffix\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateMultiplePathInterpolation) {
    // Multiple paths in single interpolation
    auto* expr = state.parseExprFromString("\"${./foo}-${./bar}\"", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, PathInLetInterpolationKnownLimitation) {
    // Path from variable in interpolation - this passes compile-time checks.
    // Known limitation: We can't detect path-to-string coercion via variables.
    // The Nix expression "${p}" where p is a path SHOULD coerce the path to a string,
    // but HVM4 doesn't implement path-to-string coercion yet.
    //
    // Current behavior: The single-element interpolation returns the path directly
    // without any string coercion, which is incorrect but documents current state.
    auto* expr = state.parseExprFromString(
        "let p = ./foo; in \"${p}\"",
        state.rootPath(CanonPath::root));
    // Passes canEvaluate (limitation: can't detect path type via variable)
    EXPECT_TRUE(backend.canEvaluate(*expr));
    // Evaluation succeeds but returns a path instead of a string (incorrect behavior)
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    EXPECT_TRUE(success);
    // BUG: Returns path instead of string - path-to-string coercion not implemented
    EXPECT_EQ(result.type(), nPath);
}

// =============================================================================
// Path Laziness Tests
// =============================================================================
// Paths should be lazy - not accessed until needed.
// With path support implemented, these expressions should compile and evaluate.

TEST_F(HVM4BackendTest, CanEvaluateUnusedPath) {
    // Path in let but not used in body
    // Even though the path is unused, the expression should still compile
    auto* expr = state.parseExprFromString("let p = ./foo; in 42", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluatePathInConditionalFalseBranch) {
    // Path in conditional false branch
    auto* expr = state.parseExprFromString(
        "if true then 42 else ./foo",
        state.rootPath(CanonPath::root));
    // Path exists in AST and should be compilable
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluatePathInConditionalTrueBranch) {
    // Path in conditional true branch
    auto* expr = state.parseExprFromString(
        "if false then ./foo else 42",
        state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Path in Data Structure Tests
// =============================================================================
// Paths within lists and attribute sets
// Both lists and attrs are now implemented along with paths

TEST_F(HVM4BackendTest, CanEvaluatePathInList) {
    // Path as list element
    auto* expr = state.parseExprFromString("[./foo ./bar]", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluatePathInAttrSet) {
    // Path as attribute value
    auto* expr = state.parseExprFromString("{ path = ./foo; }", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

// =============================================================================
// Edge Cases and Special Path Forms
// =============================================================================

TEST_F(HVM4BackendTest, CanEvaluateHomePath) {
    // Home directory path: ~/foo
    // These are parsed as ExprPath with the expanded home directory
    auto* expr = state.parseExprFromString("~/foo", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CanEvaluateStorePath) {
    // Store path literal
    // These are typically written as strings in Nix, but can be paths
    auto* expr = state.parseExprFromString("/nix/store/abc123-foo", state.rootPath(CanonPath::root));
    EXPECT_TRUE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateAngleBracketPath) {
    // Angle bracket path: <nixpkgs>
    // These are resolved through NIX_PATH and NOT ExprPath
    // They require special handling and search path resolution
    auto* expr = state.parseExprFromString("<nixpkgs>", state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Future Implementation Tests (Commented Out)
// =============================================================================
// These tests should be enabled once path support is implemented.
// They test actual evaluation behavior, not just compilation capability.

/*
TEST_F(HVM4BackendTest, EvalPathLiteral) {
    // Basic path evaluation - returns path type
    auto* expr = state.parseExprFromString("./.", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nPath);
}

TEST_F(HVM4BackendTest, EvalPathInLet) {
    auto* expr = state.parseExprFromString("let p = ./.; in p", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nPath);
}

TEST_F(HVM4BackendTest, EvalPathConcat) {
    auto* expr = state.parseExprFromString("./. + \"/foo\"", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nPath);
}

TEST_F(HVM4BackendTest, EvalPathInterpolation) {
    // Path in interpolation returns string with context
    auto* expr = state.parseExprFromString("\"${./.}\"", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nString);
    // Context should be non-empty (contains path reference)
}

TEST_F(HVM4BackendTest, EvalPathPassThrough) {
    // Path passed through identity lambda
    auto* expr = state.parseExprFromString("(p: p) ./.", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nPath);
}

TEST_F(HVM4BackendTest, EvalUnusedPathIsLazy) {
    // Path in unused binding should not cause errors
    auto* expr = state.parseExprFromString("let p = ./nonexistent; in 42", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.integer().value, 42);
}
*/

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
