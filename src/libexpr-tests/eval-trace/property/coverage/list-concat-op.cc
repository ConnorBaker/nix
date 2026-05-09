// List concatenation operator (++) on traced lists from fromJSON.
//
// Tests that dep tracking correctly propagates through the ++ list
// concatenation operator operating on values sourced from fromJSON (readFile f).
//
// Test content layout:
//   a) ListConcat_Soundness     — change fA (add element) → length miss
//   b) ListConcat_Precision     — change element VALUE in fA (same length) → length hit
//   c) ListConcat_ElemAt_Soundness — change first element of fA → elemAt 0 miss

#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;

// ── Fixture ─────────────────────────────────────────────────────────────────

class EvalTraceProperty_ListConcatOp : public TraceCacheFixture {
public:
    EvalTraceProperty_ListConcatOp() {
        testFingerprint = hashString(HashAlgorithm::SHA256,
                                    "prop-list-concat-op-traced-data");
    }
};

// ── a) ListConcat_Soundness ───────────────────────────────────────────────────
//
// Expression:  builtins.length ((fromJSON (readFile fA)) ++ (fromJSON (readFile fB)))
//
// fA = [1, 2], fB = [3, 4] → length 4.
// Soundness: add element to fA → fA becomes [1, 2, 99] → length changes to 5 → miss.
TEST_F(EvalTraceProperty_ListConcatOp, ListConcat_Soundness)
{
    TempJsonFile fA(R"([1, 2])");
    TempJsonFile fB(R"([3, 4])");

    auto expr =
        "builtins.length"
        " ((builtins.fromJSON (builtins.readFile " + fA.path.string() + "))"
        " ++ (builtins.fromJSON (builtins.readFile " + fB.path.string() + ")))";

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 4);
    }

    // Warm hit (pre-condition).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Add element to fA: length changes from 4 to 5.
    fA.modify(R"([1, 2, 99])");
    invalidateFileCache(fA.path);

    // Warm eval: must re-evaluate (soundness).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: fA gained an element, total length changes";
        EXPECT_EQ(v.integer().value, 5);
    }
}

// ── b) ListConcat_Precision ────────────────────────────────────────────────────
//
// Expression:  builtins.length ((fromJSON (readFile fA)) ++ (fromJSON (readFile fB)))
//
// fA = [1, 2], fB = [3, 4] → length 4.
// Precision: change element VALUE in fA (1 → 999), same array length → length dep
// still verifies → cache hit.
TEST_F(EvalTraceProperty_ListConcatOp, ListConcat_Precision)
{
    TempJsonFile fA(R"([1, 2])");
    TempJsonFile fB(R"([3, 4])");

    auto expr =
        "builtins.length"
        " ((builtins.fromJSON (builtins.readFile " + fA.path.string() + "))"
        " ++ (builtins.fromJSON (builtins.readFile " + fB.path.string() + ")))";

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.integer().value, 4);
    }

    // Warm hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change element value only (array stays length 2).
    fA.modify(R"([999, 2])");
    invalidateFileCache(fA.path);

    // Warm eval: must remain a cache hit (precision).
    // The #len SC dep records only list length, not element values;
    // the length is still 4 so the dep verifies.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0)
            << "cache hit expected: element value changed but list lengths unchanged";
        EXPECT_EQ(v.integer().value, 4);
    }
}

// ── c) ListConcat_ElemAt_Soundness ─────────────────────────────────────────────
//
// Expression:  builtins.elemAt ((fromJSON (readFile fA)) ++ (fromJSON (readFile fB))) 0
//
// fA = [1, 2], fB = [3, 4] → elemAt 0 = 1.
// Soundness: change first element of fA from 1 to 77 → elemAt 0 = 77 → miss.
TEST_F(EvalTraceProperty_ListConcatOp, ListConcat_ElemAt_Soundness)
{
    TempJsonFile fA(R"([1, 2])");
    TempJsonFile fB(R"([3, 4])");

    auto expr =
        "builtins.elemAt"
        " ((builtins.fromJSON (builtins.readFile " + fA.path.string() + "))"
        " ++ (builtins.fromJSON (builtins.readFile " + fB.path.string() + ")))"
        " 0";

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 1);
    }

    // Warm hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change first element of fA from 1 to 77: elemAt 0 changes.
    fA.modify(R"([77, 2])");
    invalidateFileCache(fA.path);

    // Warm eval: must re-evaluate (soundness).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: first element of fA changed, elemAt 0 result changes";
        EXPECT_EQ(v.integer().value, 77);
    }
}

} // namespace nix::eval_trace::proptest
