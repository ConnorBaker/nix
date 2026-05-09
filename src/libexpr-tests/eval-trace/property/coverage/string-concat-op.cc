// String concatenation operator (+) on traced strings from readFile.
//
// Tests that dep tracking correctly propagates through the + string
// concatenation operator operating on values sourced from builtins.readFile.
//
// Test content layout:
//   a) StringConcat_Soundness  — change fA content → concatenated result changes → miss
//   b) StringConcat_Precision  — change unrelated file fC → fA and fB deps unchanged → hit

#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;

// ── Fixture ─────────────────────────────────────────────────────────────────

class EvalTraceProperty_StringConcatOp : public TraceCacheFixture {
public:
    EvalTraceProperty_StringConcatOp() {
        testFingerprint = hashString(HashAlgorithm::SHA256,
                                    "prop-string-concat-op-traced-data");
    }
};

// ── a) StringConcat_Soundness ─────────────────────────────────────────────────
//
// Expression:  (builtins.readFile fA) + (builtins.readFile fB)
//
// fA = "hello", fB = " world" → result = "hello world".
// Soundness: change fA content → concatenated result changes → cache miss.
TEST_F(EvalTraceProperty_StringConcatOp, StringConcat_Soundness)
{
    TempTextFile fA("hello");
    TempTextFile fB(" world");

    auto expr =
        "(builtins.readFile " + fA.path.string() + ")"
        " + (builtins.readFile " + fB.path.string() + ")";

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "hello world");
    }

    // Warm hit (pre-condition).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change fA: result changes from "hello world" to "goodbye world".
    fA.modify("goodbye");
    invalidateFileCache(fA.path);

    // Warm eval: must re-evaluate (soundness).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: fA changed, string concat result must invalidate";
        EXPECT_EQ(std::string_view(v.string_view()), "goodbye world");
    }
}

// ── b) StringConcat_Precision ──────────────────────────────────────────────────
//
// Expression:  (builtins.readFile fA) + (builtins.readFile fB)
//
// Precision: change unrelated file fC (not referenced by the expression) →
// fA and fB FileBytes deps are unchanged → cache hit.
TEST_F(EvalTraceProperty_StringConcatOp, StringConcat_Precision)
{
    TempTextFile fA("hello");
    TempTextFile fB(" world");
    TempTextFile fC("unrelated content — not referenced by the expression");

    auto expr =
        "(builtins.readFile " + fA.path.string() + ")"
        " + (builtins.readFile " + fB.path.string() + ")";

    // Cold eval.
    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    // Warm hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change fC only — not referenced by the expression.
    fC.modify("completely different unrelated content — much longer than before");
    invalidateFileCache(fC.path);

    // Warm eval: must remain a cache hit (precision).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0)
            << "cache hit expected: only unrelated file fC changed, "
               "fA and fB FileBytes deps are unchanged";
        EXPECT_EQ(std::string_view(v.string_view()), "hello world");
    }
}

} // namespace nix::eval_trace::proptest
