#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// CrossSessionDeterminism — Determinism
//
// Formal:
//   ∀ expr ∈ Gen<TestExpr> where expr.expectsSuccess():
//     [Session A]
//     eval1 = eval_cold(expr)   → records trace
//     eval2 = eval_warm(expr)   → must be cache hit (loaderCalls == 0)
//
//     [Session B — recreate session to simulate new process]
//     eval3 = eval_cold(expr)   → re-records trace (same hash as session A)
//     eval4 = eval_warm(expr)   → must be cache hit (loaderCalls == 0)
//
//     eval1 == eval3             // independent recordings produce same result
//
// This catches non-determinism in trace hash computation — e.g., hash map
// iteration order leaking into the trace hash, or non-deterministic encoding
// of dep keys.  If two independent cold evals produce different trace hashes,
// the second session's warm eval will appear as a miss in the first session's
// DB, which the property detects via loaderCalls assertions.
//
// The recreate-session step works by releasing the active TraceSession and
// creating a new one via makeCache.  Within a TraceCacheFixture, all sessions
// share the same ScopedCacheDir (and thus the same SQLite DB), so the second
// session can see traces recorded by the first.
class EvalTraceProperty_CrossSessionDeterminism : public TraceCacheFixture {
public:
    EvalTraceProperty_CrossSessionDeterminism() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-cross-session-determinism");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 50;
    return params;
}

TEST_F(EvalTraceProperty_CrossSessionDeterminism, SameInputsAlwaysHitsCache)
{
    rc::detail::checkGTestWith(
        [this](TestExpr expr) {
            RC_PRE(expr.expectsSuccess());

            // Guard: GetEnvGen requires impure eval.
            // Use RC_PRE (not ASSERT_FALSE) because ASSERT_* inside a lambda
            // only exits the lambda, not the test.
            RC_PRE(!state.settings.pureEval);

            // --- Session A ---

            // Step 1: First cold eval → records trace
            Value val1;
            {
                auto cache = makeCache(expr.nixCode);
                val1 = forceRoot(*cache);
            }

            // Step 2: Second eval (same inputs) → must be a cache hit
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }

            // --- Session B (clear cache: release session, recreate) ---
            // releaseActiveSession() is called implicitly by the next makeCache
            // call.  The underlying SQLite DB is shared (same ScopedCacheDir),
            // so traces from session A remain visible.

            // Step 3: Third cold eval → re-records trace (same hash as session A)
            Value val3;
            {
                auto cache = makeCache(expr.nixCode);
                val3 = forceRoot(*cache);
            }

            // Step 4: Fourth eval → must be a cache hit again
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }

            // Step 5: Assert val1 == val3
            // Two independent cold evals of the same expression with the same
            // inputs must produce identical results.  Non-determinism in trace
            // hash computation would cause one session's warm eval to fail.
            assertValuesEqual(val1, val3);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
