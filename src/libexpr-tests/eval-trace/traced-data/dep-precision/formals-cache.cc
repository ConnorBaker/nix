#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/**
 * Cache behavior tests for lambda formals.
 *
 * Tests use makeCache() + forceRoot() to verify cache hit/miss behavior,
 * and loaderCalls to count re-evaluations.
 *
 * Sections:
 *   I. Strict formals — cache invalidation
 *   J. Ellipsis formals — cache invalidation
 *   K. Named formals — cache behavior
 */

class FormalsCacheTest : public MaterializationDepTest {};

// ═══════════════════════════════════════════════════════════════════════
// I. Strict formals — cache invalidation
// ═══════════════════════════════════════════════════════════════════════

TEST_F(FormalsCacheTest, StrictFormals_ValueChanged_CacheMiss)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, b }}: a) ({})", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    file.modify(R"({"a": 99, "b": 2})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1)
            << "SC j:a value changed — must re-evaluate";
        EXPECT_THAT(v, IsIntEq(99));
    }
}

TEST_F(FormalsCacheTest, StrictFormals_KeyRemoved_CacheMiss)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, b }}: a) ({})", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    file.modify(R"({"a": 1})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Content hash changed → cache miss → loader called → throws
        // (strict formals: missing required argument "b").
        try { forceRoot(*cache); } catch (...) {}
        EXPECT_EQ(loaderCalls, 1)
            << "SC #keys hash changed (key removed) — must re-evaluate";
    }
}

TEST_F(FormalsCacheTest, StrictFormals_UnrelatedValueChanged_CacheHit)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, b }}: a) ({})", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // Change b but not a — result unchanged
    file.modify(R"({"a": 1, "b": 99})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "#keys unchanged, j:a unchanged — cache hit";
        EXPECT_THAT(v, IsIntEq(1));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// J. Ellipsis formals — cache invalidation
// ═══════════════════════════════════════════════════════════════════════

TEST_F(FormalsCacheTest, EllipsisFormals_AccessedValueChanged_CacheMiss)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, ... }}: a) ({})", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    file.modify(R"({"a": 99, "b": 2})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1)
            << "SC j:a changed — must re-evaluate";
        EXPECT_THAT(v, IsIntEq(99));
    }
}

TEST_F(FormalsCacheTest, EllipsisFormals_UnrelatedValueChanged_CacheHit)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, ... }}: a) ({})", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    file.modify(R"({"a": 1, "b": 99})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "j:a unchanged — cache hit";
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(FormalsCacheTest, EllipsisFormals_KeyRemoved_KeepingAccessed_CacheHit)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, ... }}: a) ({})", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // Remove b, keep a — no SC #keys, j:a unchanged
    file.modify(R"({"a": 1})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "No SC #keys, j:a unchanged — cache hit";
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(FormalsCacheTest, EllipsisFormals_KeyRemoved_RemovingAccessed_CacheMiss)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, ... }}: a) ({})", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // Remove a — SC j:a can't be computed
    file.modify(R"({"b": 2})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Content hash changed → cache miss → loader called → throws
        // (ellipsis formals: missing required argument "a").
        try { forceRoot(*cache); } catch (...) {}
        EXPECT_EQ(loaderCalls, 1)
            << "SC j:a can't be computed (key gone) — cache miss";
    }
}

TEST_F(FormalsCacheTest, EllipsisFormals_WithDefault_UnrelatedKeyAdded_CacheHit)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("({{ a, b ? 0, ... }}: a + b) ({})", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"a": 1, "b": 2, "c": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "Ellipsis + unrelated key added — cache hit";
        EXPECT_THAT(v, IsIntEq(3));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// K. Named formals — cache behavior
// ═══════════════════════════════════════════════════════════════════════

TEST_F(FormalsCacheTest, NamedStrictFormals_KeyAdded_CacheMiss)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("(args@{{ a, b }}: a + args.b) ({})", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"a": 1, "b": 2, "c": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Content hash changed → cache miss → loader called → throws
        // (strict formals: unexpected argument "c").
        try { forceRoot(*cache); } catch (...) {}
        EXPECT_EQ(loaderCalls, 1)
            << "Named strict formals: SC #keys changed — cache miss";
    }
}

TEST_F(FormalsCacheTest, NamedEllipsisFormals_KeyAdded_CacheHit)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = std::format("(args@{{ a, ... }}: a) ({})", fj(file.path));

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
            << "Named ellipsis formals: no SC #keys — cache hit";
        EXPECT_THAT(v, IsIntEq(1));
    }
}

} // namespace nix::eval_trace
