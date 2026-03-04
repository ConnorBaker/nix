#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class NixBindingEdgeCaseTest : public DepPrecisionTest
{
protected:
    static auto nixBindingPred(const std::string & bindingName)
    {
        return [=](const nlohmann::json & j) {
            return j.contains("t") && j["t"].get<std::string>() == "n"
                && j.contains("p") && j["p"].is_array()
                && j["p"].size() == 1 && j["p"][0].get<std::string>() == bindingName;
        };
    }

    static size_t countNixBindingDeps(const std::vector<ResolvedDep> & deps)
    {
        return countJsonDeps(deps, DepType::StructuredContent,
            [](const nlohmann::json & j) {
                return j.contains("t") && j["t"].get<std::string>() == "n";
            });
    }
};

// ═══════════════════════════════════════════════════════════════════════
// Hyphenated names
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingEdgeCaseTest, HyphenatedName)
{
    TempTestFile file("{ my-package = 1; other-pkg = 2; }");
    auto expr = std::format("(import {}).my-package", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("my-package")))
        << "Hyphenated names should work as NixBinding deps\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Quoted names
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingEdgeCaseTest, QuotedName)
{
    TempTestFile file("{ \"hello world\" = 1; y = 2; }");
    auto expr = std::format("(import {}).\"hello world\"", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("hello world")))
        << "Quoted names should work as NixBinding deps\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Inherit pattern
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingEdgeCaseTest, InheritPattern)
{
    TempTestFile file("let x = 1; y = 2; in { inherit x y; z = 3; }");
    auto expr = std::format("(import {}).x", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("x")))
        << "Inherited attrs should get NixBinding deps\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// inherit (expr) pattern — InheritedFrom kind
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingEdgeCaseTest, InheritFromExpr)
{
    TempTestFile file("{ inherit (builtins) toString; y = 2; }");
    auto expr = std::format("(import {}).toString 42", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    // InheritedFrom bindings get NixBinding deps (with kind tag but no show(expr))
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("toString")))
        << "inherit (expr) attrs should get NixBinding deps\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Empty attrset
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingEdgeCaseTest, EmptyAttrset_NoDeps)
{
    TempTestFile file("{ }");
    auto expr = std::format("import {}", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    EXPECT_EQ(countNixBindingDeps(deps), 0u)
        << "Empty attrset should produce no NixBinding deps\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Nested attrsets (only top-level tracked)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingEdgeCaseTest, NestedAttrset_TopLevelOnly)
{
    TempTestFile file("{ outer = { inner = 1; }; y = 2; }");
    auto expr = std::format("(import {}).outer.inner", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    // Only the top-level "outer" should get a NixBinding dep
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("outer")))
        << "Top-level binding should get NixBinding dep\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, nixBindingPred("inner")))
        << "Nested binding should NOT get NixBinding dep\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Assert body excluded
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingEdgeCaseTest, AssertBody_NoTracking)
{
    TempTestFile file("assert true; { x = 1; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    // ExprAssert is not in the eligible walk list → not eligible
    EXPECT_EQ(countNixBindingDeps(deps), 0u)
        << "Assert body should NOT be eligible for NixBinding\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Dedup: same binding accessed multiple times → single dep
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingEdgeCaseTest, DuplicateAccess_SingleDep)
{
    TempTestFile file("{ x = 1; y = 2; }");
    auto expr = std::format("let f = import {}; in f.x + f.x + f.x", file.path.string());

    auto deps = evalAndCollectDeps(expr);
    auto count = countJsonDeps(deps, DepType::StructuredContent, nixBindingPred("x"));
    EXPECT_EQ(count, 1u)
        << "Multiple accesses to same binding should produce exactly 1 dep\n" << dumpDeps(deps);
}

} // namespace nix::eval_trace
