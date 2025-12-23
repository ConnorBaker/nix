/**
 * HVM4 Derivation Tests
 *
 * Tests for Nix derivation expressions in the HVM4 backend.
 *
 * Derivations are the core of Nix - they define build actions. In Nix:
 *   derivation { name = "hello"; builder = "/bin/sh"; system = "x86_64-linux"; }
 * creates a derivation that can be built to produce outputs.
 *
 * NOTE: Derivation support is NOT YET IMPLEMENTED in the HVM4 backend.
 * These tests currently verify that derivation expressions cannot be
 * compiled (canEvaluate returns false). When derivation support is
 * implemented per docs/hvm4-plan/08-derivations.md, these tests should
 * be updated to verify correct evaluation behavior.
 *
 * Implementation Strategy (from plan document):
 * - Phase 1: Pure Derivation Records (Option A - CHOSEN)
 * - Derivations compile to pure #Drv{...} records
 * - HVM4 evaluates without side effects
 * - Post-evaluation phase collects Drv records and writes to store
 *
 * HVM4 Derivation Encoding:
 *   #Drv{
 *     #Str{"hello", #NoC{}},           // name
 *     #Str{"x86_64-linux", #NoC{}},    // system
 *     #Str{"/bin/sh", #NoC{}},         // builder
 *     #Lst{2, #Con{"-c", #Con{"echo hello", #Nil{}}}},  // args
 *     #ABs{...},                        // env
 *     #Lst{1, #Con{"out", #Nil{}}}     // outputs
 *   }
 *
 * Test Categories:
 * - Basic Derivation: derivation { ... } and derivationStrict { ... }
 * - Derivation Attribute Access: drv.outPath, drv.drvPath, drv.name, etc.
 * - Derivation Outputs: Single and multiple outputs
 * - Derivation Arguments: args list handling
 * - Derivation Environment: Environment variable passing
 * - Context Propagation: String context from derivation references
 * - Pure Derivation Records: Testing the pure representation
 * - Derivation in Expressions: Using derivations in let, lambda, etc.
 * - builtins.derivation vs derivationStrict: Distinction between the two
 * - Edge Cases: Error handling and boundary conditions
 *
 * See docs/hvm4-plan/08-derivations.md for implementation details.
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Basic Derivation Tests
// =============================================================================
// These tests verify that derivation expressions are correctly identified
// as not yet supported. When derivation support is implemented, change
// EXPECT_FALSE to EXPECT_TRUE and add evaluation tests.

TEST_F(HVM4BackendTest, CannotEvaluateDerivationMinimal) {
    // Minimal derivation with required attributes
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "minimal";
                builder = "/bin/sh";
                system = "x86_64-linux";
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationStrictMinimal) {
    // derivationStrict is the lower-level primitive
    auto* expr = state.parseExprFromString(
        R"(
            builtins.derivationStrict {
                name = "minimal";
                builder = "/bin/sh";
                system = "x86_64-linux";
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationWithArgs) {
    // Derivation with builder arguments
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "with-args";
                builder = "/bin/sh";
                system = "x86_64-linux";
                args = ["-c" "echo hello"];
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationWithEnv) {
    // Derivation with custom environment variables
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "with-env";
                builder = "/bin/sh";
                system = "x86_64-linux";
                FOO = "bar";
                BAZ = "qux";
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationComplete) {
    // Complete derivation with multiple features
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "complete";
                builder = "/bin/sh";
                system = "x86_64-linux";
                args = ["-c" "echo $message > $out"];
                message = "Hello, World!";
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Derivation Attribute Access Tests
// =============================================================================
// Accessing attributes of a derivation result

TEST_F(HVM4BackendTest, CannotEvaluateDerivationName) {
    // Access the name attribute of a derivation
    auto* expr = state.parseExprFromString(
        R"(
            (derivation {
                name = "test-name";
                builder = "/bin/sh";
                system = "x86_64-linux";
            }).name
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: Returns "test-name" as a string
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationSystem) {
    // Access the system attribute of a derivation
    auto* expr = state.parseExprFromString(
        R"(
            (derivation {
                name = "test";
                builder = "/bin/sh";
                system = "x86_64-linux";
            }).system
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: Returns "x86_64-linux" as a string
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationBuilder) {
    // Access the builder attribute of a derivation
    auto* expr = state.parseExprFromString(
        R"(
            (derivation {
                name = "test";
                builder = "/bin/sh";
                system = "x86_64-linux";
            }).builder
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: Returns "/bin/sh" as a string (or path)
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationDrvPath) {
    // Access the drvPath attribute - path to the .drv file
    auto* expr = state.parseExprFromString(
        R"(
            (derivation {
                name = "test";
                builder = "/bin/sh";
                system = "x86_64-linux";
            }).drvPath
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: Returns store path like "/nix/store/...-test.drv"
    // Note: In pure derivation records, this may be computed lazily
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationOutPath) {
    // Access the outPath attribute - path to the default output
    auto* expr = state.parseExprFromString(
        R"(
            (derivation {
                name = "test";
                builder = "/bin/sh";
                system = "x86_64-linux";
            }).outPath
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: Returns store path like "/nix/store/...-test"
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationOut) {
    // Access the default 'out' output
    auto* expr = state.parseExprFromString(
        R"(
            (derivation {
                name = "test";
                builder = "/bin/sh";
                system = "x86_64-linux";
            }).out
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: Returns the derivation itself (for single-output)
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationType) {
    // Access the type attribute
    auto* expr = state.parseExprFromString(
        R"(
            (derivation {
                name = "test";
                builder = "/bin/sh";
                system = "x86_64-linux";
            }).type
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: Returns "derivation"
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationOutputs) {
    // Access the outputs attribute
    auto* expr = state.parseExprFromString(
        R"(
            (derivation {
                name = "test";
                builder = "/bin/sh";
                system = "x86_64-linux";
            }).outputs
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: Returns ["out"] for single-output derivation
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Multiple Output Derivation Tests
// =============================================================================

TEST_F(HVM4BackendTest, CannotEvaluateDerivationMultipleOutputs) {
    // Derivation with multiple outputs
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "multi-output";
                builder = "/bin/sh";
                system = "x86_64-linux";
                outputs = ["out" "dev" "doc"];
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationDevOutput) {
    // Access the dev output of a multi-output derivation
    auto* expr = state.parseExprFromString(
        R"(
            (derivation {
                name = "multi-output";
                builder = "/bin/sh";
                system = "x86_64-linux";
                outputs = ["out" "dev" "doc"];
            }).dev
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: Returns the dev output derivation
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationDocOutput) {
    // Access the doc output of a multi-output derivation
    auto* expr = state.parseExprFromString(
        R"(
            (derivation {
                name = "multi-output";
                builder = "/bin/sh";
                system = "x86_64-linux";
                outputs = ["out" "dev" "doc"];
            }).doc
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationOutputsAttr) {
    // Access the outputs list of a multi-output derivation
    auto* expr = state.parseExprFromString(
        R"(
            (derivation {
                name = "multi-output";
                builder = "/bin/sh";
                system = "x86_64-linux";
                outputs = ["out" "dev" "doc"];
            }).outputs
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: Returns ["out" "dev" "doc"]
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Derivation String Coercion Tests
// =============================================================================
// When derivations are coerced to strings, they produce their outPath

TEST_F(HVM4BackendTest, CannotEvaluateDerivationToString) {
    // Derivation coerced to string in interpolation
    auto* expr = state.parseExprFromString(
        R"(
            let drv = derivation {
                name = "test";
                builder = "/bin/sh";
                system = "x86_64-linux";
            };
            in "${drv}"
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: Returns the outPath as a string with context
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationToStringConcat) {
    // Derivation in string concatenation
    auto* expr = state.parseExprFromString(
        R"(
            let drv = derivation {
                name = "test";
                builder = "/bin/sh";
                system = "x86_64-linux";
            };
            in "${drv}/bin/hello"
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: Returns outPath + "/bin/hello" with context
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationOutputToString) {
    // Specific output coerced to string
    auto* expr = state.parseExprFromString(
        R"(
            let drv = derivation {
                name = "test";
                builder = "/bin/sh";
                system = "x86_64-linux";
                outputs = ["out" "dev"];
            };
            in "${drv.dev}"
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: Returns the dev output path as string with context
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Context Propagation Tests
// =============================================================================
// String context tracks derivation dependencies

TEST_F(HVM4BackendTest, CannotEvaluateDerivationContext) {
    // Context propagates through string operations
    auto* expr = state.parseExprFromString(
        R"(
            let
                drv = derivation {
                    name = "base";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
                path = "${drv}/lib";
            in path
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: String with context referencing the derivation
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationContextMerge) {
    // Context from multiple derivations merges
    auto* expr = state.parseExprFromString(
        R"(
            let
                drv1 = derivation {
                    name = "dep1";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
                drv2 = derivation {
                    name = "dep2";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
            in "${drv1}:${drv2}"
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: String with context referencing both derivations
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationDependency) {
    // One derivation depending on another
    auto* expr = state.parseExprFromString(
        R"(
            let
                dep = derivation {
                    name = "dependency";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
                main = derivation {
                    name = "main";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                    depPath = "${dep}";
                };
            in main
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: main derivation with dep as input derivation
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationMultipleDeps) {
    // Derivation with multiple dependencies
    auto* expr = state.parseExprFromString(
        R"(
            let
                dep1 = derivation {
                    name = "dep1";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
                dep2 = derivation {
                    name = "dep2";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
                main = derivation {
                    name = "main";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                    PATH = "${dep1}/bin:${dep2}/bin";
                };
            in main
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Pure Derivation Record Tests
// =============================================================================
// Testing the pure representation of derivations (Phase 1 approach)

TEST_F(HVM4BackendTest, CannotEvaluateDerivationIsPure) {
    // Derivation creation should not write to store during eval
    // This tests that eval is side-effect free
    auto* expr = state.parseExprFromString(
        R"(
            let
                drv = derivation {
                    name = "pure-test";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
            in drv.name
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // When implemented, this should NOT write a .drv file during eval
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationRecordFields) {
    // Pure derivation record contains all necessary fields
    auto* expr = state.parseExprFromString(
        R"(
            let
                drv = derivation {
                    name = "record-test";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                    args = ["-c" "echo test"];
                    MY_VAR = "value";
                };
            in {
                inherit (drv) name system builder;
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Derivation in Expression Context Tests
// =============================================================================

TEST_F(HVM4BackendTest, CannotEvaluateDerivationInLet) {
    // Derivation bound in let
    auto* expr = state.parseExprFromString(
        R"(
            let
                myDrv = derivation {
                    name = "let-test";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
            in myDrv.name
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationFromLambda) {
    // Lambda returning a derivation
    auto* expr = state.parseExprFromString(
        R"(
            let
                mkDrv = name: derivation {
                    inherit name;
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
            in (mkDrv "lambda-test").name
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationFromPatternLambda) {
    // Pattern lambda returning a derivation (nixpkgs style)
    auto* expr = state.parseExprFromString(
        R"(
            let
                mkDrv = { name, system ? "x86_64-linux" }: derivation {
                    inherit name system;
                    builder = "/bin/sh";
                };
            in (mkDrv { name = "pattern-test"; }).name
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationInConditional) {
    // Derivation in conditional expression
    auto* expr = state.parseExprFromString(
        R"(
            let
                useDebug = true;
                drv = if useDebug
                    then derivation {
                        name = "debug";
                        builder = "/bin/sh";
                        system = "x86_64-linux";
                    }
                    else derivation {
                        name = "release";
                        builder = "/bin/sh";
                        system = "x86_64-linux";
                    };
            in drv.name
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationInList) {
    // Derivation in list
    auto* expr = state.parseExprFromString(
        R"(
            let
                drvs = [
                    (derivation { name = "a"; builder = "/bin/sh"; system = "x86_64-linux"; })
                    (derivation { name = "b"; builder = "/bin/sh"; system = "x86_64-linux"; })
                ];
            in builtins.length drvs
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationInAttrSet) {
    // Derivation in attribute set
    auto* expr = state.parseExprFromString(
        R"(
            let
                packages = {
                    hello = derivation {
                        name = "hello";
                        builder = "/bin/sh";
                        system = "x86_64-linux";
                    };
                    world = derivation {
                        name = "world";
                        builder = "/bin/sh";
                        system = "x86_64-linux";
                    };
                };
            in packages.hello.name
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Special Derivation Attributes Tests
// =============================================================================

TEST_F(HVM4BackendTest, CannotEvaluateDerivationPassAsFile) {
    // passAsFile attribute for large data
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "pass-as-file";
                builder = "/bin/sh";
                system = "x86_64-linux";
                passAsFile = ["largeData"];
                largeData = "This is a large string that will be passed as a file";
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationOutputHashMode) {
    // Fixed-output derivation with hash
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "fixed-output";
                builder = "/bin/sh";
                system = "x86_64-linux";
                outputHashMode = "flat";
                outputHashAlgo = "sha256";
                outputHash = "0000000000000000000000000000000000000000000000000000000000000000";
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationAllowedReferences) {
    // Derivation with allowedReferences
    auto* expr = state.parseExprFromString(
        R"(
            let
                dep = derivation {
                    name = "dep";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
            in derivation {
                name = "with-allowed-refs";
                builder = "/bin/sh";
                system = "x86_64-linux";
                allowedReferences = [dep];
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationPreferLocalBuild) {
    // Derivation with preferLocalBuild
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "local-build";
                builder = "/bin/sh";
                system = "x86_64-linux";
                preferLocalBuild = true;
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationAllowSubstitutes) {
    // Derivation with allowSubstitutes = false
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "no-substitutes";
                builder = "/bin/sh";
                system = "x86_64-linux";
                allowSubstitutes = false;
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Recursive Derivation Attribute Tests
// =============================================================================

TEST_F(HVM4BackendTest, CannotEvaluateDerivationRecursive) {
    // Recursive attribute set with derivation
    auto* expr = state.parseExprFromString(
        R"(
            let
                pkgs = rec {
                    hello = derivation {
                        name = "hello";
                        builder = "/bin/sh";
                        system = "x86_64-linux";
                    };
                    helloPath = "${hello}/bin/hello";
                };
            in pkgs.helloPath
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// builtins.derivation vs derivationStrict Comparison Tests
// =============================================================================

TEST_F(HVM4BackendTest, CannotEvaluateBuiltinsDerivation) {
    // builtins.derivation is an alias for derivation
    auto* expr = state.parseExprFromString(
        R"(
            builtins.derivation {
                name = "via-builtins";
                builder = "/bin/sh";
                system = "x86_64-linux";
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationStrictVsDerivation) {
    // derivationStrict returns just the base attributes
    // derivation adds synthetic attributes like outPath
    auto* expr = state.parseExprFromString(
        R"(
            let
                strict = builtins.derivationStrict {
                    name = "test";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
                normal = derivation {
                    name = "test";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
            in {
                strictHasOutPath = strict ? outPath;
                normalHasOutPath = normal ? outPath;
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Derivation Builder Path Tests
// =============================================================================

TEST_F(HVM4BackendTest, CannotEvaluateDerivationBuilderFromPath) {
    // Builder specified as a path literal
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "builder-path";
                builder = ./builder.sh;
                system = "x86_64-linux";
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Note: Path handling also not implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationBuilderFromDrv) {
    // Builder from another derivation's output
    auto* expr = state.parseExprFromString(
        R"(
            let
                bash = derivation {
                    name = "bash";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
            in derivation {
                name = "uses-bash";
                builder = "${bash}/bin/bash";
                system = "x86_64-linux";
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Derivation Laziness Tests
// =============================================================================

TEST_F(HVM4BackendTest, CannotEvaluateDerivationLazyAttributes) {
    // Derivation attributes should be lazy
    auto* expr = state.parseExprFromString(
        R"(
            let
                drv = derivation {
                    name = "lazy-test";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                    unused = throw "should not be forced";
                };
            in drv.name
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // When implemented: accessing 'name' should NOT force 'unused'
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationUnusedDep) {
    // Unused derivation dependency should remain lazy
    auto* expr = state.parseExprFromString(
        R"(
            let
                unused = derivation {
                    name = "unused";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
                used = derivation {
                    name = "used";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
            in used.name
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // When implemented: 'unused' derivation should not be forced
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Derivation Dynamic Attributes Tests
// =============================================================================

TEST_F(HVM4BackendTest, CannotEvaluateDerivationDynamicName) {
    // Derivation name computed dynamically
    auto* expr = state.parseExprFromString(
        R"(
            let
                version = "1.0.0";
                drv = derivation {
                    name = "mypackage-${version}";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
            in drv.name
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // Expected: Returns "mypackage-1.0.0"
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationDynamicSystem) {
    // System platform computed dynamically
    auto* expr = state.parseExprFromString(
        R"(
            let
                platform = "x86_64-linux";
                drv = derivation {
                    name = "test";
                    builder = "/bin/sh";
                    system = platform;
                };
            in drv.system
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Content-Addressed Derivation Tests
// =============================================================================

TEST_F(HVM4BackendTest, CannotEvaluateContentAddressedDerivation) {
    // Content-addressed derivation (CA derivation)
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "ca-derivation";
                builder = "/bin/sh";
                system = "x86_64-linux";
                __contentAddressed = true;
                outputHashMode = "recursive";
                outputHashAlgo = "sha256";
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Derivation with Structured Attrs Tests
// =============================================================================

TEST_F(HVM4BackendTest, CannotEvaluateDerivationStructuredAttrs) {
    // Derivation with __structuredAttrs
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "structured";
                builder = "/bin/sh";
                system = "x86_64-linux";
                __structuredAttrs = true;
                nested = {
                    foo = "bar";
                    list = [1 2 3];
                };
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Edge Cases and Error Handling Tests
// =============================================================================

TEST_F(HVM4BackendTest, CannotEvaluateDerivationMissingName) {
    // Derivation missing required 'name' attribute
    // Note: This would be a runtime error when evaluated
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                builder = "/bin/sh";
                system = "x86_64-linux";
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // When implemented: should error about missing 'name'
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationMissingBuilder) {
    // Derivation missing required 'builder' attribute
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "test";
                system = "x86_64-linux";
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // When implemented: should error about missing 'builder'
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationMissingSystem) {
    // Derivation missing required 'system' attribute
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "test";
                builder = "/bin/sh";
            }
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // When implemented: should error about missing 'system'
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationInvalidOutput) {
    // Accessing non-existent output
    auto* expr = state.parseExprFromString(
        R"(
            (derivation {
                name = "test";
                builder = "/bin/sh";
                system = "x86_64-linux";
            }).nonexistent
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // When implemented: should error about missing attribute 'nonexistent'
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, CannotEvaluateDerivationEmptyOutputs) {
    // Empty outputs list should use default ["out"]
    auto* expr = state.parseExprFromString(
        R"(
            (derivation {
                name = "test";
                builder = "/bin/sh";
                system = "x86_64-linux";
                outputs = [];
            }).outputs
        )",
        state.rootPath(CanonPath::root));
    // Derivation support not yet implemented
    // When implemented: behavior may vary (error or default to ["out"])
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Comparison with Builtins (Baseline Reference)
// =============================================================================
// These tests verify that similar builtin calls behave as expected,
// providing a baseline comparison for when derivations are implemented.

TEST_F(HVM4BackendTest, CannotEvaluateBuiltinsAny) {
    // Verify that builtins generally are not implemented
    // (derivation relies on builtins infrastructure)
    auto* expr = state.parseExprFromString(
        "builtins.add 1 2",
        state.rootPath(CanonPath::root));
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Future Implementation Tests (Commented Out)
// =============================================================================
// These tests should be enabled once derivation support is implemented.
// They test actual evaluation behavior, not just compilation capability.

/*
TEST_F(HVM4BackendTest, EvalDerivationMinimal) {
    auto* expr = state.parseExprFromString(
        R"(
            derivation {
                name = "test";
                builder = "/bin/sh";
                system = "x86_64-linux";
            }
        )",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nAttrs);
    // Check that derivation has expected attributes
    auto nameAttr = result.attrs()->find(state.symbols.create("name"));
    ASSERT_NE(nameAttr, result.attrs()->end());
    EXPECT_EQ(nameAttr->value->type(), nString);
}

TEST_F(HVM4BackendTest, EvalDerivationName) {
    auto* expr = state.parseExprFromString(
        R"(
            (derivation {
                name = "test-drv";
                builder = "/bin/sh";
                system = "x86_64-linux";
            }).name
        )",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nString);
    EXPECT_EQ(result.string.s, "test-drv");
}

TEST_F(HVM4BackendTest, EvalDerivationIsPureRecord) {
    // Verify that no .drv file is written during evaluation
    auto* expr = state.parseExprFromString(
        R"(
            let
                drv = derivation {
                    name = "pure-check";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
            in drv.name
        )",
        state.rootPath(CanonPath::root));
    Value result;

    // Count .drv files before
    size_t drvCountBefore = countDrvFiles();

    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);

    // Count .drv files after - should be same (no writes during eval)
    size_t drvCountAfter = countDrvFiles();
    EXPECT_EQ(drvCountBefore, drvCountAfter);
}

TEST_F(HVM4BackendTest, EvalDerivationStringCoercion) {
    auto* expr = state.parseExprFromString(
        R"(
            let
                drv = derivation {
                    name = "coerce-test";
                    builder = "/bin/sh";
                    system = "x86_64-linux";
                };
            in "${drv}"
        )",
        state.rootPath(CanonPath::root));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nString);
    // Should contain a store path
    EXPECT_TRUE(hasPrefix(result.string.s, "/nix/store/"));
    // Should have context
    EXPECT_FALSE(result.context().empty());
}
*/

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
