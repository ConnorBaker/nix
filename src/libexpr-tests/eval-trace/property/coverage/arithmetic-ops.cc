// Arithmetic operations on traced integers from fromJSON.
//
// Tests that dep tracking correctly propagates through arithmetic builtins
// (+, -, *, /) operating on values sourced from fromJSON (readFile f).
//
// Each soundness test changes an operand and expects a cache miss (the
// expression result changes).  Each precision test changes an unrelated key
// in the same JSON file and expects a cache hit (the accessed operands are
// unchanged so the SC deps still verify).
//
// The fixture uses TempJsonFile with {"x": 10, "y": 3, "z": 99} as the
// base content.  Tests that need two separate operand files or a different
// initial state declare their own files inline.

#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;

// ── Fixture ─────────────────────────────────────────────────────────────────

class EvalTraceProperty_ArithmeticOps : public TraceCacheFixture {
public:
    EvalTraceProperty_ArithmeticOps() {
        testFingerprint = hashString(HashAlgorithm::SHA256,
                                    "prop-arithmetic-ops-traced-data");
    }
};

// ── Helper ───────────────────────────────────────────────────────────────────

// Build `(fromJSON (readFile PATH)).KEY` expression fragment.
static std::string fj(const std::filesystem::path & path, const std::string & key)
{
    return "(builtins.fromJSON (builtins.readFile " + path.string() + "))." + key;
}

// ── a) Addition_Soundness ────────────────────────────────────────────────────
//
// Expression:  (fromJSON (readFile f)).x + (fromJSON (readFile f)).y
//
// Soundness: change x's value → the sum changes → cache miss.
TEST_F(EvalTraceProperty_ArithmeticOps, Addition_Soundness)
{
    TempJsonFile file(R"({"x": 10, "y": 3, "z": 99})");
    auto expr = fj(file.path, "x") + " + " + fj(file.path, "y");

    // Cold eval: records trace.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 13);
    }

    // Warm eval: confirm cache hit (precision pre-condition).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change x: result changes from 13 to 23.
    file.modify(R"({"x": 20, "y": 3, "z": 99})");
    invalidateFileCache(file.path);

    // Warm eval: must re-evaluate (soundness).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: x changed, sum dep must invalidate";
        EXPECT_EQ(v.integer().value, 23);
    }
}

// ── b) Addition_Precision ────────────────────────────────────────────────────
//
// Expression:  (fromJSON (readFile f)).x + (fromJSON (readFile f)).y
//
// Precision: change unrelated key z → x and y deps unchanged → cache hit.
TEST_F(EvalTraceProperty_ArithmeticOps, Addition_Precision)
{
    TempJsonFile file(R"({"x": 10, "y": 3, "z": 99})");
    auto expr = fj(file.path, "x") + " + " + fj(file.path, "y");

    // Cold eval.
    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    // Warm hit (pre-condition).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change z only — x and y are unchanged.
    file.modify(R"({"x": 10, "y": 3, "z": 1234})");
    invalidateFileCache(file.path);

    // Warm eval: must remain a cache hit (precision).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0)
            << "cache hit expected: only unrelated key z changed";
        EXPECT_EQ(v.integer().value, 13);
    }
}

// ── c) Subtraction_Soundness ─────────────────────────────────────────────────
//
// Expression:  (fromJSON (readFile f)).x - (fromJSON (readFile f)).y
//
// Soundness: change x → result changes → cache miss.
TEST_F(EvalTraceProperty_ArithmeticOps, Subtraction_Soundness)
{
    TempJsonFile file(R"({"x": 10, "y": 3, "z": 99})");
    auto expr = fj(file.path, "x") + " - " + fj(file.path, "y");

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 7);
    }

    // Warm hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change x.
    file.modify(R"({"x": 50, "y": 3, "z": 99})");
    invalidateFileCache(file.path);

    // Warm eval: must re-evaluate.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: x changed, subtraction result must invalidate";
        EXPECT_EQ(v.integer().value, 47);
    }
}

// ── d) Multiplication_Soundness ──────────────────────────────────────────────
//
// Expression:  (fromJSON (readFile f)).x * (fromJSON (readFile f)).y
//
// Soundness: change y → result changes → cache miss.
TEST_F(EvalTraceProperty_ArithmeticOps, Multiplication_Soundness)
{
    TempJsonFile file(R"({"x": 10, "y": 3, "z": 99})");
    auto expr = fj(file.path, "x") + " * " + fj(file.path, "y");

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 30);
    }

    // Warm hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change y from 3 to 7.
    file.modify(R"({"x": 10, "y": 7, "z": 99})");
    invalidateFileCache(file.path);

    // Warm eval: must re-evaluate.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: y changed, product dep must invalidate";
        EXPECT_EQ(v.integer().value, 70);
    }
}

// ── e) Division_Soundness ────────────────────────────────────────────────────
//
// Expression:  (fromJSON (readFile f)).x / (fromJSON (readFile f)).y
//
// Ensures y != 0 (initial y = 5).  Soundness: change x → result changes → miss.
TEST_F(EvalTraceProperty_ArithmeticOps, Division_Soundness)
{
    TempJsonFile file(R"({"x": 20, "y": 5, "z": 99})");
    auto expr = fj(file.path, "x") + " / " + fj(file.path, "y");

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 4);
    }

    // Warm hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change x from 20 to 40.
    file.modify(R"({"x": 40, "y": 5, "z": 99})");
    invalidateFileCache(file.path);

    // Warm eval: must re-evaluate.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: x changed, quotient dep must invalidate";
        EXPECT_EQ(v.integer().value, 8);
    }
}

// ── f) Negation_Soundness ────────────────────────────────────────────────────
//
// Expression:  0 - (fromJSON (readFile f)).x   (unary minus via subtraction)
//
// Soundness: change x → negated value changes → cache miss.
TEST_F(EvalTraceProperty_ArithmeticOps, Negation_Soundness)
{
    TempJsonFile file(R"({"x": 10, "y": 3, "z": 99})");
    auto expr = "0 - " + fj(file.path, "x");

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, -10);
    }

    // Warm hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change x from 10 to 42.
    file.modify(R"({"x": 42, "y": 3, "z": 99})");
    invalidateFileCache(file.path);

    // Warm eval: must re-evaluate.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: x changed, negation result must invalidate";
        EXPECT_EQ(v.integer().value, -42);
    }
}

} // namespace nix::eval_trace::proptest
