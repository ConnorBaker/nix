#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/**
 * Dep-precision tests for lambda formals (pattern matching).
 *
 * Strict formals (no `...`) record SC #keys because extra keys would be
 * a runtime error. Ellipsis formals skip #keys recording — they accept
 * any extra keys, so only the specific extracted keys matter.
 */

class DepPrecisionFormalsTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// Positive: strict formals (no ...) SHOULD record #keys
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionFormalsTest, StrictFormals_RecordsSCKeys)
{
    // ({ a, b }: a) applied to traced data — strict formals correctly
    // records #keys because extra keys would be an error.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, b }}: a) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    // Positive: strict formals should record StructuredContent #keys
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "Strict formals (no ...) must record #keys — extra keys are an error\n"
        << dumpDeps(deps);
}

TEST_F(DepPrecisionFormalsTest, StrictFormals_KeyAdded_CacheMiss)
{
    // ({ a, b }: a) applied to traced data; traced data gains a key.
    // Strict formals records #keys, so adding a key invalidates.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, b }}: a) ({})", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    file.modify(R"({"a": 1, "b": 2, "c": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Adding "c" to strict formals { a, b } causes Content hash change
        // → cache miss → loader called → throws (strict formals reject "c").
        try { forceRoot(*cache); } catch (...) {}
        EXPECT_EQ(loaderCalls, 1)
            << "Strict formals: adding key must invalidate (Content dep catches it)";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Negative: ellipsis formals ({ x, ... }) should NOT record #keys
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionFormalsTest, EllipsisFormals_NoSCKeys)
{
    // ({ a, ... }: a) applied to traced data — ellipsis formals do NOT
    // record #keys because extra keys are accepted. Only the specific
    // extracted key "a" matters.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, ... }}: a) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "Ellipsis formals ({ a, ... }) should NOT record #keys — extra keys are accepted\n"
        << dumpDeps(deps);
}

TEST_F(DepPrecisionFormalsTest, EllipsisFormals_KeyAdded_CacheHit)
{
    // ({ a, ... }: a) applied to traced data; traced data gains unrelated key.
    // With ellipsis, key additions don't affect the result — cache hit.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, ... }}: a) ({})", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    file.modify(R"({"a": 1, "b": 2, "c": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "Ellipsis formals: adding unrelated key should not invalidate";
        EXPECT_THAT(v, IsIntEq(1));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Explicit verification: ellipsis formals precision
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionFormalsTest, EllipsisFormals_NoSCKeys_Explicit)
{
    // Explicit verification: ellipsis formals do NOT record #keys.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, ... }}: a) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "Ellipsis formals must not record #keys\n"
        << dumpDeps(deps);
}

TEST_F(DepPrecisionFormalsTest, EllipsisFormals_KeyAdded_CacheHit_TraceCache)
{
    // Ellipsis formals do not cause spurious cache miss when an unrelated
    // key is added — no #keys dep means key-set changes are invisible.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, ... }}: a) ({})", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    file.modify(R"({"a": 1, "b": 2, "c": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "Ellipsis formals: adding unrelated key should not invalidate";
        EXPECT_THAT(v, IsIntEq(1));
    }
}

} // namespace nix::eval_trace
