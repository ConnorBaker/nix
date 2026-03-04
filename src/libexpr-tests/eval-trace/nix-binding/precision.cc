#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class NixBindingPrecisionTest : public TraceCacheFixture
{
protected:
    NixBindingPrecisionTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "nix-binding-precision-test");
    }
};

// ═══════════════════════════════════════════════════════════════════════
// Precision: cache HIT after irrelevant changes
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingPrecisionTest, CommentChange_CacheHit)
{
    TempTestFile file("{ x = 1; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    // First eval — scoped to close DB before second eval
    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    file.modify("# this is a comment\n{ x = 1; y = 2; }");
    invalidateFileCache(file.path);
    state.resetFileCache();

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
    }
    EXPECT_EQ(loaderCalls, 0) << "Comment change should not invalidate NixBinding trace";
}

TEST_F(NixBindingPrecisionTest, WhitespaceChange_CacheHit)
{
    TempTestFile file("{ x = 1; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    file.modify("{  x = 1 ;  y = 2 ;  }");
    invalidateFileCache(file.path);
    state.resetFileCache();

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
    }
    EXPECT_EQ(loaderCalls, 0) << "Whitespace change should not invalidate NixBinding trace";
}

TEST_F(NixBindingPrecisionTest, AddUnrelatedBinding_CacheHit)
{
    TempTestFile file("{ x = 1; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    file.modify("{ x = 1; y = 2; z = 3; }");
    invalidateFileCache(file.path);
    state.resetFileCache();

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
    }
    EXPECT_EQ(loaderCalls, 0) << "Adding unrelated binding should not invalidate NixBinding trace";
}

TEST_F(NixBindingPrecisionTest, ModifyUnaccessed_CacheHit)
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
    EXPECT_EQ(loaderCalls, 0) << "Modifying unaccessed binding should not invalidate NixBinding trace";
}

// ═══════════════════════════════════════════════════════════════════════
// Invalidation: accessed binding definition changed
// ═══════════════════════════════════════════════════════════════════════

TEST_F(NixBindingPrecisionTest, ModifyAccessedBinding_CacheMiss)
{
    TempTestFile file("{ x = 1; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    file.modify("{ x = 42; y = 2; }");
    invalidateFileCache(file.path);
    state.resetFileCache();

    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
    }
    EXPECT_GT(loaderCalls, 0) << "Modifying accessed binding should invalidate NixBinding trace";
}

TEST_F(NixBindingPrecisionTest, RemoveAccessedBinding_CacheMiss)
{
    TempTestFile file("{ x = 1; y = 2; }");
    auto expr = std::format("(import {}).x", file.path.string());

    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    file.modify("{ y = 2; }");
    invalidateFileCache(file.path);
    state.resetFileCache();

    int loaderCalls = 0;
    try {
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
    } catch (...) {
        // Expected: .x doesn't exist anymore
    }
    EXPECT_GT(loaderCalls, 0) << "Removing accessed binding should invalidate NixBinding trace";
}

} // namespace nix::eval_trace
