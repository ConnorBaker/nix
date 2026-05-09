// Evaluation control builtins property tests — verify soundness and precision
// of tryEval, seq, and deepSeq on traced data.
//
// Test strategy: deterministic, hand-crafted expressions (no RapidCheck) that
// exercise each control builtin through the cold→warm→mutate→warm cycle.
//
// Background:
//   builtins.tryEval catches evaluation errors and returns { success, value }.
//   builtins.seq forces its first argument and returns its second.
//   builtins.deepSeq forces its first argument deeply and returns its second.
//
// Key question for seq: does the cache record a dep on the *value* of the
// forced (but discarded) first argument?  The tests document the actual
// behavior rather than asserting a fixed expectation, since the implementation
// may elect not to track the discarded argument's content.

#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// ── Fixture ──────────────────────────────────────────────────────────────────

class EvalTraceProperty_EvalControl : public TraceCacheFixture {
public:
    EvalTraceProperty_EvalControl() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-eval-control-test");
    }
};

// ── Test a: TryEval_Soundness ─────────────────────────────────────────────
//
// Expression: (builtins.tryEval (builtins.fromJSON (builtins.readFile f)).x).value
//
// f initially contains {"x": 42}.  The expression succeeds and returns 42.
// Changing x's value → the result changes → must miss.
TEST_F(EvalTraceProperty_EvalControl, TryEval_Soundness)
{
    TempJsonFile f(R"({"x": 42})");
    auto nixCode = "(builtins.tryEval (builtins.fromJSON (builtins.readFile "
        + f.path.string() + ")).x).value";

    // Cold eval — records trace.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
    }

    // Warm eval — confirm cache hit (precision pre-condition).
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change x → the tryEval result changes.
    f.modify(R"({"x": 99})");
    invalidateFileCache(f.path);

    // Must re-evaluate: the accessed key's value changed.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: x changed, tryEval.value must invalidate";
        EXPECT_EQ(v.type(), nInt);
    }
}

// ── Test b: TryEval_ErrorRecovery ────────────────────────────────────────────
//
// Expression:
//   (builtins.tryEval (if (builtins.fromJSON (builtins.readFile f)) ? "missing"
//     then (builtins.fromJSON (builtins.readFile f)).missing
//     else throw "no missing key")).success
//
// Initially f = {"x": 1} — "missing" key absent → throw → tryEval catches →
// success = false.  After mutation to {"x": 1, "missing": 42} → "missing"
// key exists → access succeeds → success = true → must miss.
//
// This tests: (1) file dep tracking through tryEval, (2) cache invalidation
// on file change that affects the try/catch outcome.
TEST_F(EvalTraceProperty_EvalControl, TryEval_ErrorRecovery)
{
    TempJsonFile f(R"({"x": 1})");
    auto nixCode =
        "(builtins.tryEval (if (builtins.fromJSON (builtins.readFile "
        + f.path.string() + ")) ? \"missing\""
        " then (builtins.fromJSON (builtins.readFile "
        + f.path.string() + ")).missing"
        " else throw \"no missing key\")).success";

    // Cold eval — "missing" absent → throw → tryEval catches → success = false.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        EXPECT_FALSE(v.boolean()) << "expected tryEval to catch throw (no 'missing' key)";
    }

    // Warm eval — confirm cache hit (file dep tracked through tryEval).
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Add "missing" key — expression now succeeds, success changes to true.
    f.modify(R"({"x": 1, "missing": 42})");
    invalidateFileCache(f.path);

    // Must re-evaluate: file changed → result changes from false to true.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: 'missing' key added, success now true";
        EXPECT_EQ(v.type(), nBool);
        EXPECT_TRUE(v.boolean()) << "expected tryEval to succeed after adding 'missing' key";
    }
}

// ── Test c: Seq_Soundness ─────────────────────────────────────────────────
//
// Expression: builtins.seq (builtins.fromJSON (builtins.readFile f)).x
//                          (builtins.readFile g)
//
// seq forces its first argument, then returns its second.  The *returned*
// value comes from g.  A change to g must invalidate.
//
// Changing f (the forced-and-discarded first argument) may or may not
// invalidate depending on whether the implementation records a dep on the
// forced-but-discarded value — this is documented in Test d.
TEST_F(EvalTraceProperty_EvalControl, Seq_Soundness)
{
    TempJsonFile f(R"({"x": 1})");
    TempTextFile g("result from g");
    auto nixCode = "builtins.seq (builtins.fromJSON (builtins.readFile "
        + f.path.string() + ")).x (builtins.readFile " + g.path.string() + ")";

    // Cold eval — records trace.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before any mutation";
    }

    // Change g — the returned value changes → must miss.
    g.modify("different result from g — changed content");
    invalidateFileCache(g.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: g changed, returned value differs";
        EXPECT_EQ(v.type(), nString);
    }
}

// ── Test d: Seq_ForcesDep ────────────────────────────────────────────────
//
// Expression: builtins.seq (builtins.readFile f) "done"
//
// seq forces f (a string), then discards it and returns "done".
// The question is whether the cache records a dep on f even though f's value
// is never part of the returned result.
//
// Two possible outcomes are both correct and documented here:
//   loaderCalls == 0 (cache hit): seq does NOT track the forced-but-discarded
//     dep — this is a known precision trade-off.  The cache is precise but
//     potentially unsound in a hypothetical future where the forced value is
//     observable (it is not here).
//   loaderCalls == 1 (cache miss): seq DOES track the forced dep — this is
//     sound but less precise.
//
// The test asserts the ACTUAL behavior and documents it.
TEST_F(EvalTraceProperty_EvalControl, Seq_ForcesDep)
{
    TempTextFile f("original content");
    auto nixCode = "builtins.seq (builtins.readFile " + f.path.string() + ") \"done\"";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Warm eval — confirm cache hit baseline.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change f — seq forces f but discards it. `prim_seq` calls
    // `forceValue(args[0])`, which inside `readFile` records a
    // FileBytes dep; the fact that the result of `readFile` is
    // discarded by `seq` does not (and should not) suppress dep
    // recording. So the warm-after-mutation eval must miss.
    f.modify("mutated content — clearly different size and content!!");
    invalidateFileCache(f.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(calls, 1)
            << "seq forces `readFile f`, which records a FileBytes dep; "
               "mutating f must invalidate the trace. A regression that "
               "causes seq to silently drop the dep would show calls=0 here.";
    }
}

// ── Test e: DeepSeq_Soundness ─────────────────────────────────────────────
//
// Expression: builtins.deepSeq (builtins.fromJSON (builtins.readFile f)) "done"
//
// deepSeq forces its first argument deeply.  If the implementation records
// deps on the deeply-forced structure, changing f must invalidate.
// If deepSeq records no dep on the forced-but-discarded structure (analogous
// to seq), the cache hits on f change — documented similarly to Test d.
TEST_F(EvalTraceProperty_EvalControl, DeepSeq_Soundness)
{
    TempJsonFile f(R"({"a": 1, "b": {"c": 2}})");
    auto nixCode = "builtins.deepSeq (builtins.fromJSON (builtins.readFile "
        + f.path.string() + ")) \"done\"";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change f — deepSeq forces it deeply. `prim_deepSeq` calls
    // `forceValueDeep(args[0])`, which forces every leaf. The
    // `builtins.readFile` call inside fromJSON records a FileBytes
    // dep; even though deepSeq does NOT record shape deps (see the
    // precision note in prim_deepSeq), the Content dep is recorded
    // through the readFile observation. Mutating f must invalidate.
    f.modify(R"({"a": 99, "b": {"c": 100}})");
    invalidateFileCache(f.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(calls, 1)
            << "deepSeq forces `readFile f` which records a FileBytes dep; "
               "mutating f must invalidate. A regression that causes deepSeq "
               "to silently drop the Content dep would show calls=0 here.";
    }
}

} // namespace nix::eval_trace::proptest
