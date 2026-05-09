#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// TraceHashDeterminism — Trace Hash Determinism
//
// Formal:
//   ∀ expr ∈ Gen<TestExpr> where expr.expectsSuccess():
//     [Session A — first cold eval]
//     eval_cold_A = eval_cold(expr)    → records trace with hash H
//
//     [Session B — second cold eval, simulates a new process]
//     eval_cold_B = eval_cold(expr)    → records trace with hash H (same)
//
//     [Warm verify — must hit cache from either session]
//     eval_warm = eval_warm(expr)      → loaderCalls == 0 (cache hit)
//
// This property verifies that two independent cold evals produce the same
// trace hash, so a warm eval after the second cold eval still hits the cache
// recorded by the first.  If the trace hash is non-deterministic (e.g., from
// hash-map iteration order or non-deterministic encoding of dep keys), the
// second cold eval writes a different hash and the warm verify misses.
//
// Structurally similar to P6 (determinism.cc), but the focus is hash stability
// rather than value equality: we assert loaderCalls == 0 without asserting
// that the two cold eval results are equal (that is P6's job).
//
// NOTE: TraceHashDeterminism is structurally similar to CrossSessionDeterminism
// (determinism.cc). CrossSessionDeterminism's step 4 (warm verify after second
// cold eval, loaderCalls == 0) already implies hash stability. TraceHashDeterminism
// is retained as an explicit, focused test of the "same inputs → same hash"
// invariant with a clearer name and simpler structure (no value comparison step).
// If test budget is tight, this test can be removed without coverage loss.
//
// The recreate-session step works by releasing the active TraceSession and
// creating a new one via makeCache.  Within a TraceCacheFixture, all sessions
// share the same ScopedCacheDir (and thus the same SQLite DB), so the second
// session can see traces recorded by the first.
class EvalTraceProperty_TraceHashDeterminism : public TraceCacheFixture {
public:
    EvalTraceProperty_TraceHashDeterminism() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-trace-hash-determinism");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 50;
    return params;
}


TEST_F(EvalTraceProperty_TraceHashDeterminism, SameInputsSameHash)
{
    rc::detail::checkGTestWith(
        [this](TestExpr expr) {
            RC_PRE(expr.expectsSuccess());

            // Guard: GetEnvGen requires impure eval.
            // Use RC_PRE (not ASSERT_FALSE) because ASSERT_* inside a lambda
            // only exits the lambda, not the test.
            RC_PRE(!state.settings.pureEval);

            // --- Cold eval session A ---
            // Records the trace with hash H for the first time.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // --- Cold eval session B (release session A, create fresh session) ---
            // makeCache implicitly releases session A.  The SQLite DB is shared
            // (same ScopedCacheDir), so session B can see session A's trace.
            // Session B re-records the trace; if hash computation is deterministic,
            // it produces the same hash H and the DB now has two rows for H
            // (or updates the existing one — both are fine for warm verify).
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // --- Warm verify after session B ---
            // If both cold evals produced hash H, the warm eval hits the
            // primary session cache. If the two cold evals produced
            // different hashes, deltaTraceCacheHits would be 0 (miss).
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }
        },
        makeParams);
}

// FingerprintIsolation — Fingerprint Isolation
//
// Formal:
//   ∀ expr ∈ Gen<TestExpr> where expr.expectsSuccess():
//     [Session with fingerprint X — cold eval]
//     testFingerprint = X
//     eval_cold_X = eval_cold(expr)    → records trace under fingerprint X
//
//     [Session with fingerprint Y — cold eval]
//     testFingerprint = Y
//     eval_cold_Y = eval_cold(expr)    → records trace under fingerprint Y (separate namespace)
//
//     [Session with fingerprint X again — warm eval must hit]
//     testFingerprint = X
//     eval_warm_X = eval_warm(expr)    → loaderCalls == 0 (X's trace still present)
//
// This tests that fingerprints isolate cache namespaces: a cold eval under
// fingerprint Y does NOT evict or overwrite the trace recorded under fingerprint X,
// so a subsequent warm eval under X still finds its trace.
//
// CrossSessionDeterminism (determinism.cc) uses a single fingerprint throughout;
// it verifies that two cold evals under the same fingerprint produce the same hash.
// FingerprintIsolation is orthogonal: it verifies that two cold evals under
// DIFFERENT fingerprints do not interfere with each other.
//
// Fingerprint leakage between sessions would cause stale results in production
// (e.g., two flake revisions sharing a cache entry because their fingerprints
// were accidentally unified).  This property guards against that class of bug.
//
// Implementation note: TraceCacheFixture stores testFingerprint as a plain Hash
// field.  Changing testFingerprint between makeCache() calls is safe within a
// single test body because makeCache() reads testFingerprint at call time.
// The ScopedCacheDir (SQLite DB file) is shared across all makeCache() calls
// in one fixture instance, which is exactly what we want: both fingerprints'
// traces live in the same DB but in separate namespaces.
class EvalTraceProperty_FingerprintIsolation : public TraceCacheFixture {
public:
    EvalTraceProperty_FingerprintIsolation() {
        // Distinct fingerprint so fingerprint-isolation DB rows don't overlap with trace-hash-determinism's.
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-fingerprint-isolation");
    }
};

TEST_F(EvalTraceProperty_FingerprintIsolation, DifferentFingerprintsDoNotInterfere)
{
    rc::detail::checkGTestWith(
        [this](TestExpr expr) {
            RC_PRE(expr.expectsSuccess());

            // Guard: GetEnvGen requires impure eval.
            // Use RC_PRE (not ASSERT_FALSE) because ASSERT_* inside a lambda
            // only exits the lambda, not the test.
            RC_PRE(!state.settings.pureEval);

            const Hash fpX = hashString(HashAlgorithm::SHA256, "p9b-fingerprint-X");
            const Hash fpY = hashString(HashAlgorithm::SHA256, "p9b-fingerprint-Y");

            // --- Session with fingerprint X: cold eval ---
            // Records trace for expr under fingerprint X's namespace.
            testFingerprint = fpX;
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // --- Session with fingerprint Y: cold eval ---
            // Records trace for expr under fingerprint Y's namespace.
            // Must NOT evict or corrupt X's trace.
            testFingerprint = fpY;
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // --- Session with fingerprint X again: warm eval must hit ---
            // X's trace was recorded before Y's cold eval. If fingerprint
            // isolation is correct, X's entry is still in the DB and the
            // warm eval finds it from the primary session cache.
            // If fingerprints leaked (Y overwrote X's entry),
            // deltaTraceCacheHits would be 0.
            testFingerprint = fpX;
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
