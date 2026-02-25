#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/**
 * Dep-precision tests for builtins.intersectAttrs.
 *
 * Currently (primops.cc:3636-3637), intersectAttrs records #keys for BOTH
 * input operands. This is an over-approximation: only the intersection
 * of key sets matters, not the full key sets of the inputs. Adding a key
 * to one input that doesn't exist in the other doesn't change the result.
 *
 * The ideal behavior: intersectAttrs should record deps that reflect only
 * the actual intersection, not the full input key sets.
 *
 * Tests prefixed DISABLED_ document the ideal behavior that fails due to
 * the current over-approximation.
 */

class DepPrecisionIntersectTest : public TracedDataTest
{
protected:
    std::vector<Dep> evalAndCollectDeps(const std::string & nixExpr)
    {
        DependencyTracker tracker;
        state.traceActiveDepth++;
        auto v = eval(nixExpr, /* forceValue */ true);
        state.traceActiveDepth--;
        return tracker.collectTraces();
    }

    static size_t countDeps(const std::vector<Dep> & deps, DepType type, const std::string & keySubstr)
    {
        size_t n = 0;
        for (auto & d : deps)
            if (d.type == type && d.key.find(keySubstr) != std::string::npos)
                n++;
        return n;
    }

    static bool hasDep(const std::vector<Dep> & deps, DepType type, const std::string & keySubstr)
    {
        return countDeps(deps, type, keySubstr) > 0;
    }
};

// ═══════════════════════════════════════════════════════════════════════
// Positive: intersectAttrs correctly records deps for the intersection
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionIntersectTest, IntersectAttrs_RecordsSCKeys_BothOperands)
{
    // intersectAttrs currently records #keys for both operands.
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = R"(builtins.intersectAttrs (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"()) (builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"()))";

    auto deps = evalAndCollectDeps(expr);

    // Currently records #keys for both — this is an over-approximation
    // but it IS the current behavior we want to test for.
    EXPECT_GE(countDeps(deps, DepType::StructuredContent, "#keys"), 2u)
        << "intersectAttrs currently records #keys for both operands";
}

TEST_F(DepPrecisionIntersectTest, IntersectAttrs_SharedKeyChanged_CacheMiss)
{
    // intersectAttrs(a, b) where shared key "x" changes value → cache miss.
    // This is correct: the result changes.
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = R"((builtins.intersectAttrs (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"()) (builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"())).x)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(10)); // intersectAttrs returns right's values
    }

    fileB.modify(R"({"x": 99, "z": 30})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "Changing shared key's value must invalidate";
        EXPECT_THAT(v, IsIntEq(99));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Negative: adding a key to one operand that doesn't exist in the other
// should NOT invalidate (currently fails — over-approximation)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionIntersectTest, DISABLED_IntersectAttrs_DisjointKeyAdded_CacheHit)
{
    // intersectAttrs(a, b) where a gains a key not in b → should be cache hit.
    // The intersection is unchanged because the new key has no match in b.
    //
    // CURRENTLY FAILS: #keys on a is recorded, so key addition invalidates.
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = R"((builtins.intersectAttrs (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"()) (builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"())).x)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(10));
    }

    // a gains "y" which has no match in b — intersection unchanged
    fileA.modify(R"({"x": 1, "y": 2})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        // Ideal: cache hit (new key "y" has no match in b, intersection unchanged)
        EXPECT_EQ(loaderCalls, 0)
            << "Adding disjoint key to one operand should not invalidate intersection";
        EXPECT_THAT(v, IsIntEq(10));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Documenting current behavior: intersectAttrs records #keys for both
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionIntersectTest, IntersectAttrs_CurrentlyInvalidatesOnDisjointKeyAdd)
{
    // Documents the CURRENT over-approximation: intersectAttrs records #keys
    // for both operands, so adding a disjoint key causes spurious invalidation.
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = R"((builtins.intersectAttrs (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"()) (builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"())).x)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(10));
    }

    // a gains "y" which has no match in b — intersection unchanged
    fileA.modify(R"({"x": 1, "y": 2})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        // Current (imprecise) behavior: #keys on a causes re-eval
        EXPECT_EQ(loaderCalls, 1)
            << "KNOWN IMPRECISION: intersectAttrs records #keys on both operands";
        EXPECT_THAT(v, IsIntEq(10));
    }
}

TEST_F(DepPrecisionIntersectTest, IntersectAttrs_ValueChangeUnrelatedKey_CacheHit)
{
    // intersectAttrs(a, b).x where a changes value of non-shared key → cache hit.
    // This works because SC deps are fine-grained: only x's value is checked.
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = R"((builtins.intersectAttrs (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"()) (builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"())).x)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(10));
    }

    // a changes "y" value — unrelated to the intersection result
    fileA.modify(R"({"x": 1, "y": 99})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0) << "Changing non-shared key value should not invalidate";
        EXPECT_THAT(v, IsIntEq(10));
    }
}

} // namespace nix::eval_trace
