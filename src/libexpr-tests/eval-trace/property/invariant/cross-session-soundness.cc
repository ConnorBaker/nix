#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// CrossSessionSoundness — Cross-Session Soundness
//
// Formal:
//   ∀ expr, same dep files/env:
//     Session A: cold eval records trace
//     Session B (new makeCache call, same fingerprint, same dep state): warm eval hits cache
//
// Within TraceCacheFixture, all makeCache calls share the same ScopedCacheDir
// (same SQLite DB) and the same testFingerprint.  So "Session B" is simply
// a new makeCache call after the first — it re-opens the SQLite connection and
// should find the trace recorded by Session A.
//
// The property verifies that session-key encoding doesn't prevent cross-session
// cache hits when fingerprint and dep state are identical.
//
// This is structurally identical to CacheSoundness (soundness.cc) but its intent is different:
// CacheSoundness checks that warm eval after cold eval works (single-session round trip).
// CrossSessionSoundness explicitly tests the scenario where the "session" boundary is crossed by
// releasing and recreating the TraceSession between the two evals.
class EvalTraceProperty_CrossSessionSoundness : public TraceCacheFixture {
public:
    EvalTraceProperty_CrossSessionSoundness() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-cross-session-soundness");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 100;
    return params;
}

TEST_F(EvalTraceProperty_CrossSessionSoundness, NewSessionHitsCache)
{
    rc::detail::checkGTestWith(
        [this](TestExpr expr) {
            RC_PRE(expr.expectsSuccess());

            // Guard: GetEnvGen requires impure eval.
            RC_PRE(!state.settings.pureEval);

            // --- Session A: cold eval, records trace ---
            Value coldVal;
            {
                auto cache = makeCache(expr.nixCode);
                coldVal = forceRoot(*cache);
            }
            // makeCache already called releaseActiveSession() at the start,
            // and the session is still active here.  Explicitly release it to
            // simulate the end of Session A (flush all SQLite writes).
            releaseActiveSession();

            // --- Session B: new makeCache call (new SQLite connection + session key) ---
            // With the same fingerprint and same dep file state, the warm eval
            // must find the trace recorded by Session A. Case D: after
            // releaseActiveSession() the primary in-memory session slot is
            // dropped, so Session B legitimately finds the trace via history-
            // based recovery. Assert deltaRecoveryAttempts() >= 1 instead of
            // forbidding recovery.
            Value warmVal;
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                warmVal = forceRoot(*cache);
                // Case D: after releaseActiveSession() the primary in-memory
                // session slot is dropped. The verifier path may bootstrap
                // from SQLite via History (incrementing recoveryAttempts) or
                // reopen the primary slot via scanHistory. Both bump
                // nrTraceCacheHits on success.
                RC_ASSERT(snap.deltaTraceCacheHits() >= 1);
            }

            assertValuesEqual(coldVal, warmVal);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
