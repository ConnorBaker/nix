/**
 * HVM4 Import Tests
 *
 * Comprehensive tests for Nix import expressions in the HVM4 backend.
 *
 * IMPORTANT: Import support is NOT YET IMPLEMENTED in the HVM4 backend.
 * These tests currently verify that import expressions are correctly identified
 * as unsupported (canEvaluate returns false). Once import support is implemented
 * per docs/hvm4-plan/07-imports.md, these tests should be updated to verify
 * correct evaluation behavior.
 *
 * The chosen implementation approach is "Pre-Import Resolution" which:
 * - Resolves all imports before HVM4 compilation
 * - Parses main expression and collects all static import paths
 * - Recursively parses and compiles imported files
 * - Builds combined AST with imports resolved
 * - Keeps HVM4 evaluation pure and deterministic
 *
 * Limitations of this approach:
 * - Dynamic import paths not supported (e.g., `import (./. + filename)`)
 * - Import From Derivation (IFD) not supported in Phase 1
 * - Expressions with dynamic imports must fall back to standard evaluator
 *
 * Test Categories:
 * - Basic Import Expressions: import ./file.nix
 * - Import in Let Bindings: let pkg = import ./pkg.nix; in ...
 * - Nested Imports: Files that import other files
 * - Import with Arguments: import ./f.nix { a = 1; }
 * - Dynamic Import Detection: Expressions that cannot be pre-resolved
 * - Import Path Forms: Relative, absolute, and search paths
 * - Circular Import Detection: Should be detected and rejected
 * - Memoization: Same file imported multiple times
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Basic Import Expression Tests
// =============================================================================
// These tests verify that basic import expressions are correctly identified
// as not yet supported. When import support is implemented, change EXPECT_FALSE
// to EXPECT_TRUE and add evaluation tests.

TEST_F(HVM4BackendTest, CannotEvaluateImportRelativePath) {
    // Basic import with relative path: import ./foo.nix
    // This is the most common import form in Nix
    auto* expr = state.parseExprFromString("import ./foo.nix", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportCurrentDir) {
    // Import current directory (typically has default.nix): import ./.
    auto* expr = state.parseExprFromString("import ./.", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportParentDir) {
    // Import parent directory: import ../.
    auto* expr = state.parseExprFromString("import ../.", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportAbsolutePath) {
    // Import with absolute path: import /etc/nix/foo.nix
    // Absolute paths are less common but valid
    auto* expr = state.parseExprFromString("import /etc/nix/foo.nix", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportDeepPath) {
    // Import with deeply nested relative path
    auto* expr = state.parseExprFromString("import ./foo/bar/baz/qux.nix", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportAngleBracket) {
    // Import with angle bracket (search path): import <nixpkgs>
    // These are resolved through NIX_PATH
    auto* expr = state.parseExprFromString("import <nixpkgs>", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportAngleBracketWithPath) {
    // Import with angle bracket and subpath: import <nixpkgs/lib>
    auto* expr = state.parseExprFromString("import <nixpkgs/lib>", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Import in Let Binding Tests
// =============================================================================
// Common pattern: binding imported value to a variable

TEST_F(HVM4BackendTest, CannotEvaluateImportInLetBinding) {
    // Import bound to a variable: let pkg = import ./pkg.nix; in pkg
    auto* expr = state.parseExprFromString(
        "let pkg = import ./pkg.nix; in pkg",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateMultipleImportsInLet) {
    // Multiple imports in let bindings
    auto* expr = state.parseExprFromString(
        "let a = import ./a.nix; b = import ./b.nix; in a",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateNestedLetWithImport) {
    // Nested let bindings with import
    auto* expr = state.parseExprFromString(
        "let outer = let inner = import ./foo.nix; in inner; in outer",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportWithOtherBindings) {
    // Import alongside regular bindings
    auto* expr = state.parseExprFromString(
        "let x = 42; pkg = import ./pkg.nix; y = 10; in x",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    // Note: Even though the import result is not used, it exists in the AST
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Import with Arguments Tests
// =============================================================================
// Pattern: import ./f.nix { arg = value; }
// The imported file is a function, and we pass arguments to it

TEST_F(HVM4BackendTest, CannotEvaluateImportWithEmptyAttrArg) {
    // Import applied to empty attribute set
    auto* expr = state.parseExprFromString(
        "(import ./f.nix) {}",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportWithSingleArg) {
    // Import applied to single-attribute set
    auto* expr = state.parseExprFromString(
        "(import ./f.nix) { a = 1; }",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportWithMultipleArgs) {
    // Import applied to multiple-attribute set
    auto* expr = state.parseExprFromString(
        "(import ./f.nix) { a = 1; b = 2; c = 3; }",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportWithNestedArgs) {
    // Import applied to nested attribute set
    auto* expr = state.parseExprFromString(
        "(import ./f.nix) { config = { enable = true; }; }",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportWithVariableArg) {
    // Import applied to a variable holding an attribute set
    auto* expr = state.parseExprFromString(
        "let args = { a = 1; }; in (import ./f.nix) args",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportChainedApplication) {
    // Import applied multiple times (curried function)
    auto* expr = state.parseExprFromString(
        "((import ./f.nix) 1) 2",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Nested Import Tests
// =============================================================================
// Files that import other files, creating a dependency graph

TEST_F(HVM4BackendTest, CannotEvaluateImportOfImportingFile) {
    // Conceptually: foo.nix contains `import ./bar.nix`
    // This tests that the compiler must handle transitive imports
    auto* expr = state.parseExprFromString(
        "import ./foo.nix",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    // The pre-resolution strategy must recursively resolve imports
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDiamondImportPattern) {
    // Diamond pattern: A imports B and C, both B and C import D
    // This tests import memoization - D should only be compiled once
    auto* expr = state.parseExprFromString(
        "let b = import ./b.nix; c = import ./c.nix; in b",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Dynamic Import Path Tests
// =============================================================================
// These expressions have dynamically computed import paths and cannot
// be handled by the pre-import resolution strategy. They must fall back
// to the standard evaluator.

TEST_F(HVM4BackendTest, CannotEvaluateDynamicImportConcat) {
    // Dynamic import path via concatenation: import (./. + "/foo.nix")
    // This cannot be statically resolved
    auto* expr = state.parseExprFromString(
        "import (./. + \"/foo.nix\")",
        state.rootPath(CanonPath::root));
    // This should NEVER be supported by HVM4 pre-resolution
    // Must fall back to standard evaluator
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDynamicImportVariable) {
    // Dynamic import path from variable: let p = ./foo.nix; in import p
    // Even though the path is known at parse time, this pattern is dynamic
    auto* expr = state.parseExprFromString(
        "let p = ./foo.nix; in import p",
        state.rootPath(CanonPath::root));
    // This is a dynamic import pattern - the import argument is a variable
    // Pre-resolution requires the path to be a literal ExprPath
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDynamicImportInterpolation) {
    // Dynamic import with string interpolation in path
    auto* expr = state.parseExprFromString(
        "let name = \"foo\"; in import (./. + \"/${name}.nix\")",
        state.rootPath(CanonPath::root));
    // Dynamic - cannot be pre-resolved
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDynamicImportConditional) {
    // Conditional import path: import (if cond then ./a.nix else ./b.nix)
    auto* expr = state.parseExprFromString(
        "import (if (1 == 1) then ./a.nix else ./b.nix)",
        state.rootPath(CanonPath::root));
    // Dynamic - the path depends on a condition
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDynamicImportFunctionResult) {
    // Import path from function application
    auto* expr = state.parseExprFromString(
        "let f = x: ./foo.nix; in import (f 1)",
        state.rootPath(CanonPath::root));
    // Dynamic - path comes from function result
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Import in Lambda Tests
// =============================================================================
// Import expressions within function bodies

TEST_F(HVM4BackendTest, CannotEvaluateImportInLambdaBody) {
    // Import inside a lambda body: (x: import ./foo.nix) 1
    auto* expr = state.parseExprFromString(
        "(x: import ./foo.nix) 1",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateLambdaReturningImport) {
    // Lambda that returns an imported value
    auto* expr = state.parseExprFromString(
        "let f = x: import ./foo.nix; in f 1",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportPassedToLambda) {
    // Import result passed to a lambda
    auto* expr = state.parseExprFromString(
        "(x: x) (import ./foo.nix)",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Import in Conditional Tests
// =============================================================================
// Import expressions within if-then-else branches

TEST_F(HVM4BackendTest, CannotEvaluateImportInTrueBranch) {
    // Import in the true branch of a conditional
    auto* expr = state.parseExprFromString(
        "if (1 == 1) then import ./foo.nix else 42",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    // Note: Pre-resolution must collect imports from ALL branches
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportInFalseBranch) {
    // Import in the false branch of a conditional
    auto* expr = state.parseExprFromString(
        "if (1 == 2) then 42 else import ./foo.nix",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportInBothBranches) {
    // Import in both branches of a conditional
    auto* expr = state.parseExprFromString(
        "if (1 == 1) then import ./a.nix else import ./b.nix",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    // Pre-resolution must collect BOTH imports
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Import in Data Structure Tests
// =============================================================================
// Imports within lists and attribute sets

TEST_F(HVM4BackendTest, CannotEvaluateImportInList) {
    // Import as list element
    auto* expr = state.parseExprFromString(
        "[1 (import ./foo.nix) 3]",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: both lists and imports need support
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportInAttrSet) {
    // Import as attribute value
    auto* expr = state.parseExprFromString(
        "{ foo = import ./foo.nix; }",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: both attrsets and imports need support
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportInNestedAttrSet) {
    // Import deeply nested in attribute set
    auto* expr = state.parseExprFromString(
        "{ outer = { inner = import ./foo.nix; }; }",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: both attrsets and imports need support
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// scopedImport Tests
// =============================================================================
// scopedImport is NOT memoized (unlike import) and takes an additional
// scope argument

TEST_F(HVM4BackendTest, CannotEvaluateScopedImport) {
    // Basic scopedImport: builtins.scopedImport {} ./foo.nix
    auto* expr = state.parseExprFromString(
        "builtins.scopedImport {} ./foo.nix",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: scopedImport is more complex than import
    // Note: scopedImport is NOT memoized
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateScopedImportWithScope) {
    // scopedImport with custom scope
    auto* expr = state.parseExprFromString(
        "builtins.scopedImport { x = 42; } ./foo.nix",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: scopedImport is more complex than import
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Import From Derivation (IFD) Tests
// =============================================================================
// IFD requires building derivations during evaluation - explicitly not
// supported in Phase 1 of HVM4

TEST_F(HVM4BackendTest, CannotEvaluateImportFromDerivation) {
    // Import From Derivation pattern (simplified example)
    // In practice, the path would be a derivation output path
    auto* expr = state.parseExprFromString(
        "import /nix/store/abc123-foo/default.nix",
        state.rootPath(CanonPath::root));
    // IFD is explicitly NOT supported in Phase 1
    // Would need effect-based approach (Option C from plan)
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Import Memoization Tests
// =============================================================================
// Same file imported multiple times should be deduplicated

TEST_F(HVM4BackendTest, CannotEvaluateSameImportTwice) {
    // Same file imported twice in let bindings
    auto* expr = state.parseExprFromString(
        "let a = import ./foo.nix; b = import ./foo.nix; in a",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    // When implemented, ./foo.nix should only be compiled once (memoization)
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateSameImportMultipleTimes) {
    // Same file imported multiple times
    auto* expr = state.parseExprFromString(
        "let a = import ./x.nix; b = import ./x.nix; c = import ./x.nix; in a",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    // Memoization via AST deduplication
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Unused Import Tests
// =============================================================================
// Imports that exist in the AST but are not used in the final result

TEST_F(HVM4BackendTest, CannotEvaluateUnusedImport) {
    // Import in unused binding
    auto* expr = state.parseExprFromString(
        "let unused = import ./foo.nix; in 42",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    // The import exists in AST and must be resolvable, even if unused
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportInUnusedConditionalBranch) {
    // Import in branch that won't be taken
    // Note: This is different from dynamic import - the path is still static
    auto* expr = state.parseExprFromString(
        "if (1 == 2) then import ./never-used.nix else 42",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    // Pre-resolution must still resolve this import (for correctness)
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Import Error Cases (for future implementation)
// =============================================================================
// These tests document expected error behaviors when imports are implemented

TEST_F(HVM4BackendTest, CannotEvaluateImportNonexistent) {
    // Import of nonexistent file should error (when implemented)
    auto* expr = state.parseExprFromString(
        "import ./this-file-does-not-exist-12345.nix",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    // When implemented, this should produce a clear error about missing file
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Relative Import Base Path Tests
// =============================================================================
// Import resolution depends on the base path of the importing file

TEST_F(HVM4BackendTest, CannotEvaluateImportRelativeToParent) {
    // Import that goes up then down: import ../sibling/foo.nix
    auto* expr = state.parseExprFromString(
        "import ../sibling/foo.nix",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportWithComplexRelativePath) {
    // Complex relative path with multiple parent references
    auto* expr = state.parseExprFromString(
        "import ../../foo/bar/../baz/qux.nix",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Combination Tests
// =============================================================================
// Tests combining import with other features

TEST_F(HVM4BackendTest, CannotEvaluateImportWithArithmeticAfter) {
    // Import followed by arithmetic on result (assuming result is int)
    auto* expr = state.parseExprFromString(
        "(import ./num.nix) + 1",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportWithAttrAccess) {
    // Import followed by attribute access
    auto* expr = state.parseExprFromString(
        "(import ./attrs.nix).foo",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: both imports and attrset access need support
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportThenApply) {
    // Import a function and apply it
    auto* expr = state.parseExprFromString(
        "(import ./func.nix) 42",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateImportInRecAttrset) {
    // Import inside recursive attrset
    auto* expr = state.parseExprFromString(
        "rec { lib = import ./lib.nix; app = lib.mkApp {}; }",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: imports, rec attrsets all need support
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Home Path Import Tests
// =============================================================================
// Import with home directory path expansion

TEST_F(HVM4BackendTest, CannotEvaluateImportHomePath) {
    // Import with home path: import ~/nixpkgs/default.nix
    auto* expr = state.parseExprFromString(
        "import ~/nixpkgs/default.nix",
        state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: import requires pre-resolution strategy
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Future Implementation Tests (Commented Out)
// =============================================================================
// These tests should be enabled once import support is implemented.
// They test actual evaluation behavior, not just compilation capability.

/*
// Basic import evaluation (requires test file setup)
TEST_F(HVM4BackendTest, EvalImportSimple) {
    // Create a test file that returns 42
    // Then verify: import ./test.nix evaluates to 42
    auto* expr = state.parseExprFromString("import ./test.nix", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, EvalImportWithArgs) {
    // Import a function and pass arguments
    // file.nix contains: { x }: x + 1
    auto* expr = state.parseExprFromString("(import ./file.nix) { x = 41; }", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nInt);
    EXPECT_EQ(result.integer().value, 42);
}

TEST_F(HVM4BackendTest, EvalImportMemoized) {
    // Same import should return same value (memoized)
    auto* expr = state.parseExprFromString(
        "let a = import ./file.nix; b = import ./file.nix; in a == b",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nBool);
    EXPECT_TRUE(result.boolean());
}

TEST_F(HVM4BackendTest, EvalNestedImports) {
    // a.nix imports b.nix which imports c.nix
    auto* expr = state.parseExprFromString("import ./a.nix", state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    // Verify the chain resolved correctly
}

TEST_F(HVM4BackendTest, EvalCircularImportError) {
    // a.nix imports b.nix which imports a.nix
    // Should produce a clear error about circular import
    auto* expr = state.parseExprFromString("import ./circular-a.nix", state.rootPath(CanonPath::root));
    Value result;
    // This should fail with a specific error about circular imports
    EXPECT_THROW(backend.tryEvaluate(expr, state.baseEnv, result), ...);
}

TEST_F(HVM4BackendTest, EvalDynamicImportFallback) {
    // Dynamic import should gracefully fall back to standard evaluator
    auto* expr = state.parseExprFromString(
        "import (./. + \"/foo.nix\")",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    // Should return false (fallback) rather than error
    EXPECT_FALSE(success);
    // Verify fallback counter was incremented
    auto stats = backend.getStats();
    EXPECT_GT(stats.fallbacks, 0);
}
*/

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
