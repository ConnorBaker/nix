#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/**
 * Dep-precision tests for lambda formals (pattern matching).
 *
 * Currently (eval.cc:1650), maybeRecordAttrKeysDep is called unconditionally
 * for ALL lambda formals, even when the lambda has `...` (ellipsis). This is
 * an over-approximation: a `{ x, ... }:` lambda doesn't observe the full key
 * set (it only extracts `x` and ignores the rest). Recording #keys causes
 * spurious invalidation when unrelated keys are added to the traced data input.
 *
 * The ideal behavior: only record #keys for STRICT formals (no `...`).
 * For ellipsis formals, only the specific extracted keys matter.
 *
 * Tests prefixed DISABLED_ document the ideal behavior that fails due to
 * the current over-approximation.
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
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Strict formals (no ...) must record #keys — extra keys are an error\n"
        << dumpDeps(deps);
}

TEST_F(DepPrecisionFormalsTest, DISABLED_StrictFormals_KeyAdded_CacheMiss)
{
    // ({ a, b }: a) applied to traced data; traced data gains a key.
    // Strict formals records #keys, so adding a key SHOULD invalidate.
    //
    // CURRENTLY FAILS: The ImplicitShape fallback or the trace caching
    // structure allows verification to pass despite #keys mismatch.
    // Needs investigation — the cached result (1) is correct but the
    // lambda would throw at runtime if re-evaluated with { a, b, c }.
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
        // Adding "c" to strict formals { a, b } would be an error, so
        // the #keys dep should cause re-evaluation.
        EXPECT_EQ(loaderCalls, 1)
            << "Strict formals: adding key must invalidate (#keys dep catches it)";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Negative: ellipsis formals ({ x, ... }) should NOT record #keys
// (Currently over-approximated — these tests are DISABLED)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionFormalsTest, DISABLED_EllipsisFormals_NoSCKeys)
{
    // ({ a, ... }: a) applied to traced data — ellipsis formals should NOT
    // record #keys because extra keys are accepted. Only the specific
    // extracted key "a" matters.
    //
    // CURRENTLY FAILS: eval.cc:1650 calls maybeRecordAttrKeysDep
    // unconditionally, recording #keys even with ellipsis.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, ... }}: a) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    // Ideal: NO StructuredContent #keys (ellipsis accepts any keys)
    EXPECT_FALSE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "Ellipsis formals ({ a, ... }) should NOT record #keys — extra keys are accepted\n"
        << dumpDeps(deps);
}

TEST_F(DepPrecisionFormalsTest, DISABLED_EllipsisFormals_KeyAdded_CacheHit)
{
    // ({ a, ... }: a) applied to traced data; traced data gains unrelated key.
    // With ellipsis, key additions don't affect the result — should be cache hit.
    //
    // CURRENTLY FAILS: #keys is recorded, causing spurious invalidation.
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
        // Ideal: cache hit (ellipsis accepts "c", result is still 1)
        EXPECT_EQ(loaderCalls, 0)
            << "Ellipsis formals: adding unrelated key should not invalidate";
        EXPECT_THAT(v, IsIntEq(1));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Documenting current behavior: ellipsis formals DO record #keys (over-approx)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionFormalsTest, EllipsisFormals_CurrentlyRecordsSCKeys)
{
    // Documents the CURRENT over-approximation: ellipsis formals record #keys.
    // This test will need updating when the imprecision is fixed.
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, ... }}: a) ({})", fj(file.path));

    auto deps = evalAndCollectDeps(expr);

    // Current (imprecise) behavior: #keys IS recorded even with ellipsis
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "#keys"))
        << "KNOWN IMPRECISION: ellipsis formals currently record #keys (eval.cc:1650)\n"
        << dumpDeps(deps);
}

TEST_F(DepPrecisionFormalsTest, EllipsisFormals_CurrentlyInvalidatesOnKeyAdd)
{
    // Documents the CURRENT over-approximation: ellipsis formals cause
    // spurious cache miss when an unrelated key is added.
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
        // Current (imprecise) behavior: #keys causes re-eval even with ellipsis
        EXPECT_EQ(loaderCalls, 1)
            << "KNOWN IMPRECISION: ellipsis formals + key addition → spurious re-eval";
        EXPECT_THAT(v, IsIntEq(1));
    }
}

} // namespace nix::eval_trace
