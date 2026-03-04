#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class NixBindingBasicTest : public DepPrecisionTest
{
protected:
    /// Match a NixBinding SC dep: format "n", path contains [bindingName]
    static auto nixBindingPred(const std::string & bindingName)
    {
        return [=](const nlohmann::json & j) {
            return j.contains("t") && j["t"].get<std::string>() == "n"
                && j.contains("p") && j["p"].is_array()
                && j["p"].size() == 1 && j["p"][0].get<std::string>() == bindingName;
        };
    }

    /// Count NixBinding SC deps with format "n"
    static size_t countNixBindingDeps(const std::vector<ResolvedDep> & deps)
    {
        return countJsonDeps(deps, DepType::StructuredContent,
            [](const nlohmann::json & j) {
                return j.contains("t") && j["t"].get<std::string>() == "n";
            });
    }
};

// ═══════════════════════════════════════════════════════════════════════
// Basic recording: single access
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingBasicTest, SingleAccess_RecordsNixBindingDep)
{
    TempTestFile file("{ x = 1; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("x")))
        << "Accessing .x should record a NixBinding SC dep\n" << dumpDeps(deps);
}

TEST_F(NixBindingBasicTest, SingleAccess_NoDepForUnaccessed)
{
    TempTestFile file("{ x = 1; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("y")))
        << "Unaccessed binding y should NOT have a NixBinding dep\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Multi-access: each binding gets its own dep
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingBasicTest, MultiAccess_EachBindingRecorded)
{
    TempTestFile file("{ a = 1; b = 2; c = 3; d = 4; }");
    auto expr = std::format("let f = import {}; in f.a + f.b + f.c", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("a")))
        << dumpDeps(deps);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("b")))
        << dumpDeps(deps);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("c")))
        << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("d")))
        << "Unaccessed binding d should NOT have a dep\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// hasAttr (?) operator
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingBasicTest, HasAttr_RecordsNixBindingDep)
{
    TempTestFile file("{ x = 1; y = 2; }");
    auto expr = std::format("(import {}) ? x", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("x")))
        << "? operator should record NixBinding dep for found attr\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// builtins.getAttr
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingBasicTest, GetAttr_RecordsNixBindingDep)
{
    TempTestFile file("{ x = 1; y = 2; }");
    auto expr = std::format("builtins.getAttr \"x\" (import {})", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("x")))
        << "builtins.getAttr should record NixBinding dep\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Excluded patterns: rec, dynamic, list, if
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingBasicTest, RecAttrset_NoTracking)
{
    TempTestFile file("rec { x = 1; y = x; }");
    auto expr = std::format("(import {}).x", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_EQ(countNixBindingDeps(deps), 0u)
        << "rec attrsets should NOT get NixBinding deps\n" << dumpDeps(deps);
}

TEST_F(NixBindingBasicTest, DynamicAttrs_NoTracking)
{
    // Use let to create a dynamic key — the interpolation in attr position
    // creates a DynamicAttrDef in the parser.
    TempTestFile file("let name = \"x\"; in { ${ name } = 1; y = 2; }");
    auto expr = std::format("(import {}).y", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_EQ(countNixBindingDeps(deps), 0u)
        << "Attrsets with dynamic attrs should NOT get NixBinding deps\n" << dumpDeps(deps);
}

TEST_F(NixBindingBasicTest, ListReturn_NoTracking)
{
    TempTestFile file("[ 1 2 3 ]");
    auto expr = std::format("builtins.elemAt (import {}) 0", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_EQ(countNixBindingDeps(deps), 0u)
        << "List returns should NOT get NixBinding deps\n" << dumpDeps(deps);
}

TEST_F(NixBindingBasicTest, IfReturn_NoTracking)
{
    TempTestFile file("if true then { x = 1; } else { y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_EQ(countNixBindingDeps(deps), 0u)
        << "if/then/else body should NOT get NixBinding deps\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Lambda-wrapped attrset (nixpkgs pattern)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingBasicTest, LambdaWrapped_RecordsNixBindingDep)
{
    TempTestFile file("self: super: { x = 1; y = 2; }");
    auto expr = std::format("(import {} null null).x", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("x")))
        << "Lambda-wrapped attrset should get NixBinding deps\n" << dumpDeps(deps);
}

TEST_F(NixBindingBasicTest, LetWrapped_RecordsNixBindingDep)
{
    TempTestFile file("let helper = 1; in { x = helper; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("x")))
        << "Let-wrapped attrset should get NixBinding deps\n" << dumpDeps(deps);
}

TEST_F(NixBindingBasicTest, WithWrapped_RecordsNixBindingDep)
{
    TempTestFile file("with builtins; { x = toString 1; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("x")))
        << "With-wrapped attrset should get NixBinding deps\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Content dep is always present (NixBinding is additive)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingBasicTest, ContentDepAlwaysPresent)
{
    TempTestFile file("{ x = 1; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::Content, file.path.string()))
        << "Content dep should always be present alongside NixBinding\n" << dumpDeps(deps);
}

} // namespace nix::eval_trace
