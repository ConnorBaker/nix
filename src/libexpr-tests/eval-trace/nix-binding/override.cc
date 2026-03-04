#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class NixBindingOverrideTest : public MaterializationDepTest
{
protected:
    NixBindingOverrideTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "nix-binding-override-test");
    }

    static auto nixBindingPred(const std::string & bindingName)
    {
        return [=](const nlohmann::json & j) {
            return j.contains("t") && j["t"].get<std::string>() == "n"
                && j.contains("p") && j["p"].is_array()
                && j["p"].size() == 1 && j["p"][0].get<std::string>() == bindingName;
        };
    }
};

// ═══════════════════════════════════════════════════════════════════════
// Two-level override: Content fails but NixBinding passes → valid
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingOverrideTest, ContentFails_NixBindingPasses_TraceValid)
{
    TempTestFile file("{ x = 1; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    file.modify("{ x = 1; y = 999; }");
    invalidateFileCache(file.path);
    state.resetFileCache();

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
    }
    EXPECT_EQ(loaderCalls, 0)
        << "Two-level override: Content fails, NixBinding passes → cache hit";
}

// ═══════════════════════════════════════════════════════════════════════
// Scope change invalidates all bindings
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingOverrideTest, ScopeChange_InvalidatesAll)
{
    TempTestFile file("let helper = 1; in { x = helper; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    file.modify("let helper = 99; in { x = helper; y = 2; }");
    invalidateFileCache(file.path);
    state.resetFileCache();

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
    }
    EXPECT_GT(loaderCalls, 0)
        << "Scope change should invalidate all NixBinding deps";
}

TEST_F(NixBindingOverrideTest, LambdaFormalChange_InvalidatesAll)
{
    TempTestFile file("{ a, ... }: { x = a; y = 2; }");
    auto expr = std::format("(import {} {{ a = 1; }}).x", file.path.string());

    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    file.modify("{ a, b ? 0, ... }: { x = a; y = 2; }");
    invalidateFileCache(file.path);
    state.resetFileCache();

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
    }
    EXPECT_GT(loaderCalls, 0)
        << "Lambda formal change should invalidate all NixBinding deps";
}

// ═══════════════════════════════════════════════════════════════════════
// Structure change: attrset becomes non-eligible
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingOverrideTest, BecomeRecursive_CacheMiss)
{
    TempTestFile file("{ x = 1; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    file.modify("rec { x = 1; y = x; }");
    invalidateFileCache(file.path);
    state.resetFileCache();

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
    }
    EXPECT_GT(loaderCalls, 0)
        << "Becoming rec should invalidate (NixBinding verification returns nullopt)";
}

// ═══════════════════════════════════════════════════════════════════════
// Update (//) operator: LHS tracking
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingOverrideTest, UpdateOperator_LhsTracked)
{
    TempTestFile file("{ x = 1; y = 2; } // { z = 3; }");
    auto expr = std::format("(import {}).x", file.path.string());

    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    file.modify("{ x = 1; y = 999; } // { z = 3; }");
    invalidateFileCache(file.path);
    state.resetFileCache();

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
    }
    EXPECT_EQ(loaderCalls, 0)
        << "Update op: modifying unaccessed LHS binding should cache hit";
}

// ═══════════════════════════════════════════════════════════════════════
// inherit (expr) source change invalidates
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingOverrideTest, InheritFromSourceChange_CacheMiss)
{
    TempTestFile file("let src = { x = 1; }; in { inherit (src) x; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    // Change the source expression — x still exists but comes from different source
    file.modify("let src = { x = 99; }; in { inherit (src) x; y = 2; }");
    invalidateFileCache(file.path);
    state.resetFileCache();

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
    }
    EXPECT_GT(loaderCalls, 0)
        << "Changing inherit (expr) source should invalidate NixBinding";
}

// ═══════════════════════════════════════════════════════════════════════
// Let binding change (unrelated to accessed binding) — cache hit
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingOverrideTest, LetScopeUnrelatedChange_CacheHit)
{
    // Accessed binding x doesn't use the let binding "unused"
    TempTestFile file("let unused = 1; in { x = 42; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    // Change unused let binding value — scope hash changes (conservative),
    // so all bindings invalidate. This tests that the override FAILS
    // (scope change invalidates everything, which is correct but conservative).
    file.modify("let unused = 999; in { x = 42; y = 2; }");
    invalidateFileCache(file.path);
    state.resetFileCache();

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
    }
    EXPECT_GT(loaderCalls, 0)
        << "Let scope change should invalidate all bindings (conservative)";
}

} // namespace nix::eval_trace
