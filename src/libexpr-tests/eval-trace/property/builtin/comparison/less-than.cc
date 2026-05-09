// P36 — builtins.lessThan: structure deps from both operands
//
// Expression:
//   builtins.lessThan
//     (builtins.length (builtins.fromJSON (builtins.readFile fileA)))
//     (builtins.length (builtins.fromJSON (builtins.readFile fileB)))
//
// Both operands are list lengths derived from separate JSON files.  The dep
// system records a #len shape dep on each list's provenance.
//
// Soundness:
//   Changing the length of fileA's array must invalidate the cached result.
//
// Precision:
//   Changing the *values* inside fileA's array (same length) must NOT
//   invalidate the cached result — only the #len dep was recorded, not the
//   individual element values.
//
// Test strategy: deterministic, hand-crafted expressions (no RapidCheck).

#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// ── Fixture ─────────────────────────────────────────────────────────────────

class EvalTraceProperty_LessThan : public TraceCacheFixture {
public:
    EvalTraceProperty_LessThan() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-less-than");
    }
};

// ── Soundness ────────────────────────────────────────────────────
//
// fileA = [1, 2, 3]  (length 3)
// fileB = [4, 5, 6]  (length 3)
// Expression: lessThan(length(fromJSON(readFile fileA)), length(fromJSON(readFile fileB)))
//
// Initial result: false (3 < 3 == false).
//
// Mutation: fileA grows to [1, 2, 3, 4]  (length 4).
// After mutation: false (4 < 3 == false), but the length dep on fileA changed
// so the trace must be invalidated and re-evaluated.
TEST_F(EvalTraceProperty_LessThan, Soundness)
{
    TempJsonFile fileA(R"([1, 2, 3])");
    TempJsonFile fileB(R"([4, 5, 6])");

    auto nixCode =
        "builtins.lessThan"
        " (builtins.length (builtins.fromJSON (builtins.readFile " + fileA.path.string() + ")))"
        " (builtins.length (builtins.fromJSON (builtins.readFile " + fileB.path.string() + ")))";

    // Cold eval — evaluates expression, records #len deps on both operands.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool) << "lessThan should return a bool";
        // 3 < 3 == false
        EXPECT_FALSE(v.boolean());
    }

    // Warm eval — confirm cache hit (precision pre-condition before mutation).
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before fileA mutation";
    }

    // Mutate fileA: add an element so its length changes from 3 to 4.
    fileA.modify(R"([1, 2, 3, 4])");
    invalidateFileCache(fileA.path);

    // Warm eval must re-evaluate: the #len dep on fileA's array changed.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: fileA's array length changed, #len dep must invalidate";
        EXPECT_EQ(v.type(), nBool);
        // 4 < 3 == false (still false, but re-evaluated)
        EXPECT_FALSE(v.boolean());
    }
}

// ── Precision ────────────────────────────────────────────────────
//
// Same setup as Soundness, but the mutation changes ELEMENT VALUES without
// changing the array LENGTH.
//
// fileA starts as [1, 2, 3]  (length 3).
// Mutation: fileA becomes [10, 20, 30]  (still length 3 — only values changed).
//
// The #len dep records the length hash, not the element values.  Changing
// element values does not affect the length hash, so the cache should still
// hit after this mutation.
TEST_F(EvalTraceProperty_LessThan, Precision)
{
    TempJsonFile fileA(R"([1, 2, 3])");
    TempJsonFile fileB(R"([4, 5, 6])");

    auto nixCode =
        "builtins.lessThan"
        " (builtins.length (builtins.fromJSON (builtins.readFile " + fileA.path.string() + ")))"
        " (builtins.length (builtins.fromJSON (builtins.readFile " + fileB.path.string() + ")))";

    // Cold eval — evaluates expression, records #len deps.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        // 3 < 3 == false
        EXPECT_FALSE(v.boolean());
    }

    // Mutate fileA: same length, different element values.
    fileA.modify(R"([10, 20, 30])");
    invalidateFileCache(fileA.path);

    // Warm eval — should still hit cache: the #len dep is unchanged (length is
    // still 3), so the element value change must not cause re-evaluation.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0)
            << "cache hit expected: fileA's element values changed but length is unchanged; "
               "#len dep should not be invalidated by value-only mutations";
        EXPECT_EQ(v.type(), nBool);
    }
}

} // namespace nix::eval_trace::proptest
