#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// Concurrent / Interleaved Evaluation
//
// These tests address the soundness concern raised in the soundness analysis:
// concurrent parallel-attribute evaluation can produce stale results if
// SqliteTraceStorage or EvalState are accessed from multiple threads simultaneously.
//
// Two tests are provided:
//
//   1. ConcurrentColdEval_IndependentExpressions_NoCorruption (SKIPPED)
//      The ideal property test: two threads evaluate independent expressions
//      against the same TraceCacheFixture concurrently.  Skipped because
//      TraceCacheFixture is not thread-safe — it shares a single EvalState
//      and makeCache() calls releaseActiveSession(), both of which have
//      unsynchronized mutable state.  See implementation note below.
//
//   2. InterleavedEval_AlternatingExpressions_NoCrossContamination
//      Runnable substitute: cold-eval A, cold-eval B, warm-eval A, warm-eval B
//      in strict sequence.  Exercises the cross-expression state isolation that
//      concurrent access would stress, without requiring thread safety from the
//      fixture.  Catches cross-expression trace contamination bugs (e.g., one
//      expression's dep set leaking into another's cached trace entry).
//
// Infrastructure gap: to enable real concurrent evaluation tests, a
// per-thread EvalState fixture would be needed.  Each thread would need its
// own EvalState (and thus its own InterningPools, lstat cache, and GC
// heap) sharing only the SQLite SqliteTraceStorage backend.  See CLAUDE.md Section 8
// for the parallel subprocess-based gap (store/concurrency.cc).

class EvalTraceProperty_ConcurrentEval : public TraceCacheFixture {
public:
    EvalTraceProperty_ConcurrentEval() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-concurrent-eval");
    }
};

// ── Test 1: Concurrent cold eval (SKIPPED) ───────────────────────────────────
//
// What this would test:
//   Create two independent file-backed expressions (disjoint dep sets).
//   Evaluate them cold concurrently from two threads, then verify both
//   produce warm cache hits afterward.
//
// Why it is skipped:
//   TraceCacheFixture shares a single EvalState across all makeCache calls.
//   EvalState is NOT thread-safe:
//     - Its GC heap, symbol table, and value allocator are not guarded by
//       any mutex.
//     - makeCache() calls releaseActiveSession(), which writes to
//       activeSession_ — unsynchronized shared state.
//   Calling makeCache() from two threads against the same fixture would
//   produce data races and undefined behavior.
//
//   The store/concurrency.cc test (ConcurrentUpserts_ParallelWrites_NoCorruption)
//   addresses multi-process SQLite concurrent writes at the SqliteTraceStorage level and
//   is also skipped pending a subprocess harness.
//
// What's needed to unblock:
//   A thread-safe variant of TraceCacheFixture where each thread holds its own
//   EvalState (constructed with the same settings and the same ScopedCacheDir),
//   and makeCache() is called per-thread rather than on a shared fixture.
//   The SQLite backend can be shared across EvalState instances because SqliteTraceStorage
//   uses WAL mode with per-connection locking.

TEST_F(EvalTraceProperty_ConcurrentEval, IndependentExpressions_NoCorruption)
{
    GTEST_SKIP() << "Concurrent evaluation requires a thread-safe fixture with "
                    "per-thread EvalState instances.  The current TraceCacheFixture "
                    "shares a single EvalState (non-thread-safe GC heap, symbol table, "
                    "and activeSession_ state).  See store/concurrency.cc for the "
                    "blocked subprocess-based approach.  Implement when a per-thread "
                    "EvalState fixture is available.";
}

// ── Test 2: Interleaved eval — no cross-expression contamination ──────────────
//
// Simulates the observable effects of concurrent evaluation without requiring
// thread safety: evaluate two independent file-backed expressions in an
// interleaved order and assert that neither expression's trace contaminates
// the other.
//
// Sequence:
//   1. Cold-eval A  (records trace for A at fp(A))
//   2. Cold-eval B  (records trace for B at fp(B); distinct session slot)
//   3. Warm-eval A  (must PRIMARY-cache-hit; no recovery)
//   4. Warm-eval B  (must PRIMARY-cache-hit; no recovery)
//
// §N.1 note: `TraceCacheFixture::makeCache` now mixes the Nix expression
// into the session fingerprint, so exprA and exprB land at DIFFERENT
// `(session_key, AttrPathId(0))` slots — the pre-§N.1 "shared-fingerprint
// overwrite" hazard is no longer exercised by this call shape. The test
// still catches:
//   - cross-session-slot data leaks (e.g., one session's bulkLoadAll
//     polluting another's sessionCache_)
//   - encoding bugs that conflate payloads across distinct trace rows
//
// §N.5 root fix: counter-delta via PathCountersSnapshot. The old
// `loaderCalls == 0` would accept recovery-after-overwrite as "cache hit";
// `deltaTraceCacheHits >= 1 && deltaRecoveryAttempts == 0` forbids that.
// If this test ever needs to re-exercise the shared-fingerprint hazard,
// that requires explicitly-keyed TraceSession construction (see
// `TraceCacheTest::makeCache(expr, Hash, int*)` overload in store/cache.cc).

TEST_F(EvalTraceProperty_ConcurrentEval, InterleavedExpressions_NoCrossContamination)
{
    // Two independent file-backed expressions with disjoint dep sets.
    TempTextFile fileA("content_alpha");
    TempTextFile fileB("content_beta");

    const std::string exprA = "builtins.readFile " + fileA.path.string();
    const std::string exprB = "builtins.readFile " + fileB.path.string();

    // Step 1: Cold eval A — records A's trace.
    {
        auto cache = makeCache(exprA);
        (void) forceRoot(*cache);
    }

    // Step 2: Cold eval B — records B's trace.  Must not disturb A's entry.
    {
        auto cache = makeCache(exprB);
        (void) forceRoot(*cache);
    }

    // Step 3: Warm eval A — must primary-cache-hit.  No recovery.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(exprA);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "Warm eval A missed primary cache after cold eval B — "
               "B's trace may have contaminated A's entry";
    }

    // Step 4: Warm eval B — must primary-cache-hit.  No recovery.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(exprB);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "Warm eval B missed primary cache — cross-expression state leak suspected";
    }
}

} // namespace nix::eval_trace::proptest
