// Error-path property tests — verify that expressions that throw during traced
// evaluation do NOT leave a committed (partial) trace behind, and that the
// cache remains uncorrupted for unrelated expressions.
//
// Test strategy: deterministic, hand-crafted expressions (no RapidCheck) that
// force error conditions during forceRoot().
//
// Background: forceRoot() calls state.forceValue() which may throw a nix::Error.
// The TraceSession's trace scope should handle the exception cleanly — either
// via RAII or by not committing on the error path.  If a partial trace were
// committed, a subsequent call with a corrected expression would incorrectly
// serve the old (erroneous) cached result.

#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// ── Fixture ─────────────────────────────────────────────────────────────────

class EvalTraceProperty_ErrorPath : public TraceCacheFixture {
public:
    EvalTraceProperty_ErrorPath() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-error-path");
    }
};

// ── Test 1: No partial trace after an eval error ─────────────────────────────
//
// Expression: let d = fromJSON (readFile file); in d.missing_key
//
// With an initial JSON of {"a": 1}, accessing .missing_key throws because the
// attribute is absent.  After fixing the file to {"a": 1, "missing_key": 42}
// the expression should succeed and a correct trace should be recorded.
//
// Soundness: If a partial trace had been committed during the first (throwing)
// evaluation, the warm eval after the fix would incorrectly serve a stale
// result.  Conversely, if no trace was committed, the warm eval must re-call
// the loader (loaderCalls == 1) and then the subsequent warm eval must hit
// (loaderCalls == 0).
TEST_F(EvalTraceProperty_ErrorPath, NoPartialTrace_AfterEvalError)
{
    TempJsonFile file(R"({"a": 1})");
    auto nixCode = "let d = builtins.fromJSON (builtins.readFile "
        + file.path.string() + "); in d.missing_key";

    // Cold eval — should throw because "missing_key" is absent in the JSON.
    {
        auto cache = makeCache(nixCode);
        EXPECT_THROW(forceRoot(*cache), nix::Error)
            << "accessing a missing JSON attribute should throw";
    }

    // Invalidate so the next makeCache sees the updated file.
    invalidateFileCache(file.path);

    // Fix the file — add the missing key.
    file.modify(R"({"a": 1, "missing_key": 42})");
    invalidateFileCache(file.path);

    // Cold eval after fix — should succeed and record a correct trace.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "no committed trace from the errored eval; loader must run";
        EXPECT_EQ(v.type(), nInt) << "missing_key = 42 should evaluate to an int";
    }

    // Warm eval — trace was recorded in the previous step; should hit cache.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected: correct trace was recorded after the fix";
        EXPECT_EQ(v.type(), nInt);
    }
}

// ── Test 2: Error in expression B does not corrupt cache for expression A ────
//
// Expression A: builtins.readFile <file>  — always succeeds; records a trace.
// Expression B: builtins.fromJSON "not valid json"  — always throws.
//
// The invariant: after B throws during forceRoot(), the trace for A that was
// committed in the first step must remain intact and continue to serve warm
// hits.  B's failure must not corrupt A's stored trace.
//
// Each expression uses a unique fingerprint in makeCache(), so they operate
// in different DB namespaces.  However, both use the same TraceCacheFixture
// instance (and hence the same SQLite DB file).  Corruption would manifest as
// A's warm eval re-calling the loader after B's failure.
TEST_F(EvalTraceProperty_ErrorPath, ValidEval_AfterError_NotCorrupted)
{
    TempTextFile file("hello from readFile");

    // Expression A: a valid readFile that records a stable trace.
    auto nixCodeA = "builtins.readFile " + file.path.string();

    // Cold eval of A — records trace for A.
    {
        auto cache = makeCache(nixCodeA);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Confirm A hits cache (precision pre-condition).
    {
        int calls = 0;
        auto cache = makeCache(nixCodeA, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before B's error";
    }

    // Expression B: always throws (invalid JSON string literal).
    // Use a different fingerprint so A's and B's traces are in separate namespaces.
    // We do this by temporarily changing testFingerprint.
    {
        auto savedFingerprint = testFingerprint;
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-error-path-expr-b");

        auto nixCodeB = R"(builtins.fromJSON "not valid json")";
        auto cache = makeCache(nixCodeB);
        EXPECT_THROW(forceRoot(*cache), nix::Error)
            << "fromJSON with invalid JSON string should throw";

        testFingerprint = savedFingerprint;
    }

    // Warm eval of A — B's error must not have corrupted A's trace.
    {
        int calls = 0;
        auto cache = makeCache(nixCodeA, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0)
            << "A's cache hit must survive B's throwing eval; B must not corrupt A's trace";
        EXPECT_EQ(v.type(), nString);
    }
}

} // namespace nix::eval_trace::proptest
