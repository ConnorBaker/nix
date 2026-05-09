/// sv-telemetry.cc — Per-DepKeySetId structural-variant candidate
/// telemetry tests.  These exercise the counters that back
/// `evalTrace.structVariant.byDepKeySet` in NIX_SHOW_STATS JSON.

#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/util/logging.hh"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

namespace {

/// RAII helper that clears the process-scoped SV telemetry map and the
/// `Counter::enabled` gate around each test.  `clearSVCandidateStats()`
/// is idempotent; each test starts from a known-empty snapshot.
struct SVTelemetryScope
{
    bool savedEnabled;

    SVTelemetryScope()
        : savedEnabled(Counter::enabled)
    {
        clearSVCandidateStats();
        Counter::enabled = true;
    }

    ~SVTelemetryScope()
    {
        clearSVCandidateStats();
        Counter::enabled = savedEnabled;
    }

    /// Snapshot of the current map contents.
    SVCandidateStatsMap snapshot() const
    {
        return snapshotSVCandidateStats();
    }

    /// Total over every entry — useful when the test doesn't need to
    /// differentiate between buckets.
    SVCandidateStats totals() const
    {
        SVCandidateStats t;
        for (auto & kv : snapshot()) {
            t.tried         += kv.second.tried;
            t.succeeded     += kv.second.succeeded;
            t.abortedEarly  += kv.second.abortedEarly;
            t.hashMismatch  += kv.second.hashMismatch;
            t.depsResolvedSum += kv.second.depsResolvedSum;
            t.depResolveUsSum += kv.second.depResolveUsSum;
            t.bothSetCount                += kv.second.bothSetCount;
            t.earlierHashMismatchCount    += kv.second.earlierHashMismatchCount;
            t.earlierHashMismatchSavedDeps += kv.second.earlierHashMismatchSavedDeps;
            t.hashMismatchOnlyCount       += kv.second.hashMismatchOnlyCount;
            t.hashMismatchOnlySavedDeps   += kv.second.hashMismatchOnlySavedDeps;
        }
        return t;
    }
};

/// Simple Logger that captures all log messages at or below
/// `lvlDebug` into an in-memory buffer.  Installed as the process
/// logger for the duration of the scope; the old logger is restored
/// on destruction.  Sets `nix::verbosity` to `lvlDebug` so `debug()`
/// call sites actually emit (`printMsg` gates on the global).
struct DebugLogCaptureScope
{
    std::unique_ptr<nix::Logger> savedLogger;
    nix::Verbosity savedVerbosity;
    std::shared_ptr<std::vector<std::string>> messages;
    std::shared_ptr<std::mutex> mutex;

    struct CapturingLogger : public nix::Logger
    {
        std::shared_ptr<std::vector<std::string>> messages;
        std::shared_ptr<std::mutex> mutex;

        CapturingLogger(
            std::shared_ptr<std::vector<std::string>> m,
            std::shared_ptr<std::mutex> mu)
            : messages(std::move(m))
            , mutex(std::move(mu))
        {}

        void log(nix::Verbosity, std::string_view s) override
        {
            std::lock_guard<std::mutex> lk(*mutex);
            messages->emplace_back(std::string(s));
        }
        void logEI(const nix::ErrorInfo &) override {}
    };

    DebugLogCaptureScope()
        : savedLogger(std::move(nix::logger))
        , savedVerbosity(nix::verbosity)
        , messages(std::make_shared<std::vector<std::string>>())
        , mutex(std::make_shared<std::mutex>())
    {
        nix::verbosity = nix::lvlDebug;
        nix::logger = std::make_unique<CapturingLogger>(messages, mutex);
    }

    ~DebugLogCaptureScope()
    {
        nix::logger = std::move(savedLogger);
        nix::verbosity = savedVerbosity;
    }

    std::vector<std::string> matching(const std::string & needle) const
    {
        std::lock_guard<std::mutex> lk(*mutex);
        std::vector<std::string> out;
        for (auto & m : *messages)
            if (m.find(needle) != std::string::npos)
                out.push_back(m);
        return out;
    }
};

} // anonymous namespace

// Success case: tryStructuralVariantRecovery accepts a candidate.
// After one successful SV recovery, the telemetry map must contain at
// least one entry with `succeeded >= 1` and the total `tried >= 1`.
TEST_F(TraceStoreTest, SVTelemetry_SuccessCase_RecordsAcceptance)
{
    SVTelemetryScope scope;

    // Same shape as `Recovery_StructuralVariant_DifferentStructHash_StillFindsMatch`
    // — two structural groups, invalidate a dep so direct-hash misses,
    // SV scan finds a sibling group whose dep hashes still match.
    ScopedEnvVar envA("NIX_SVT_SUCCESS_A", "aval");
    ScopedEnvVar envB("NIX_SVT_SUCCESS_B", "bval");

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        // Group 1: [A] — will be the acceptance target.
        db->record(ea, rootPath(), string_t{"sv-success-A", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_SUCCESS_A", "aval")});

        // Group 2: [A, B] — latest, so direct-hash lookup targets this
        // bucket; we invalidate B below to force direct-hash to miss.
        db->record(ea, rootPath(), string_t{"sv-success-AB", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_SUCCESS_A", "aval"),
             makeEnvVarDep(pools(), "NIX_SVT_SUCCESS_B", "bval")});
    });

    setenv("NIX_SVT_SUCCESS_B", "bval-new", 1);
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value())
        << "SV recovery must succeed in this scenario";

    auto totals = scope.totals();
    EXPECT_GE(totals.tried, 1u) << "at least one bucket considered";
    EXPECT_GE(totals.succeeded, 1u) << "at least one bucket succeeded";
    EXPECT_EQ(totals.abortedEarly, 0u)
        << "no abort expected — all deps are EnvVars and resolveable";
    // `depsResolvedSum >= 1` because the [A]-only candidate resolves A.
    EXPECT_GE(totals.depsResolvedSum, 1u);
}

// Failure case: every SV candidate fails either via abort (volatile/
// resolveFailed) or hash-mismatch.  Telemetry must record non-zero
// failures and zero successes.
TEST_F(TraceStoreTest, SVTelemetry_FailureCase_RecordsMismatch)
{
    SVTelemetryScope scope;

    ScopedEnvVar envA("NIX_SVT_FAIL_A", "a-v1");
    ScopedEnvVar envB("NIX_SVT_FAIL_B", "b-v1");

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        // Group 1: [A] — recorded with A=a-v1.
        db->record(ea, rootPath(), string_t{"fail-A", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_FAIL_A", "a-v1")});

        // Group 2: [A, B] — recorded with A=a-v1, B=b-v1.  Latest, so
        // direct-hash targets this bucket.
        db->record(ea, rootPath(), string_t{"fail-AB", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_FAIL_A", "a-v1"),
             makeEnvVarDep(pools(), "NIX_SVT_FAIL_B", "b-v1")});
    });

    // Mutate BOTH A and B to values not matching any recorded group.
    // Direct-hash sees (A=v2, B=v2), no match.  SV scan finds [A]-only
    // group and recomputes hash(A=v2) — no match; [A,B] bucket is
    // skipped in SV scan because its structural hash matches the
    // direct-hash target.  Net: SV iterates the [A] bucket once with
    // no match → hashMismatch increments.
    setenv("NIX_SVT_FAIL_A", "a-v2", 1);
    setenv("NIX_SVT_FAIL_B", "b-v2", 1);
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value())
        << "SV recovery must fail when all candidates mismatch";

    auto totals = scope.totals();
    EXPECT_GE(totals.tried, 1u)
        << "at least one bucket must have been tried";
    EXPECT_EQ(totals.succeeded, 0u)
        << "no bucket must have succeeded";
    EXPECT_GE(totals.hashMismatch, 1u)
        << "at least one bucket must have fallen through to hash mismatch";
}

// When SV is never entered, the telemetry map stays empty so the
// JSON emitter can skip the `byDepKeySet` block entirely.
TEST_F(TraceStoreTest, SVTelemetry_NoSVEntry_MapStaysEmpty)
{
    SVTelemetryScope scope;

    ScopedEnvVar env("NIX_SVT_EMPTY", "val");

    // Single recording, no mutation — verify hits primary cache directly.
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"no-sv", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_EMPTY", "val")});
    });
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());

    EXPECT_TRUE(scope.snapshot().empty())
        << "no SV recovery should have fired in this scenario";
}

// ── SV any-dep early-exit gating signal ───────────────────────────────
//
// Three things under test:
// 1. `firstHashMismatchIdx` / `firstResolveFailIdx` populate as
//    documented: the per-candidate loop no longer breaks on hash
//    mismatch (it scans until the first resolveFail), and the two
//    indices are captured independently.
// 2. The `eval-trace/recovery: SV early-exit signal ...` debug log
//    fires per candidate and carries plausible values.
// 3. The aggregate savings counters on `SVCandidateStats`
//    (`bothSetCount`, `earlierHashMismatchCount`,
//    `earlierHashMismatchSavedDeps`, `hashMismatchOnlyCount`,
//    `hashMismatchOnlySavedDeps`) accumulate in the expected buckets.
//
// Scenario: two recorded groups, `[A]` and `[A, B]`.  The [A,B]
// bucket is the last recording, so its DepKeySetId is the
// directResult's structHash target — SV skips it and only the [A]
// bucket is scanned.
//
// Between record and warm-verify we mutate A (so A no longer
// matches what was recorded) — direct hash now computes
// `hash(A=a2, B=b1)` which is not in history, so direct hash misses.
// SV then scans the [A] bucket.  The [A] bucket's rep dep has
// recorded `A=a1`; current state resolves A to a2, the resolved
// value differs from the stored dep.hash, so
// `firstHashMismatchIdx = 0`.  No resolveFail (EnvVar resolves
// cleanly), totalDeps=1.  The candidate iterates fully, computes
// a hash that has no match in history, and records `hashMismatch`.
//
// This exercises the "hashMismatchOnly" slice of the savings
// counter: `hashMismatchOnlySavedDeps += (totalDeps - firstHash
// MismatchIdx) = 1`.
TEST_F(TraceStoreTest, SVTelemetry_EarlyExitSignal_HashMismatchOnly)
{
    SVTelemetryScope scope;
    DebugLogCaptureScope logs;
    evalSettings.useStructuralRecoveryMismatchTelemetry = true;

    ScopedEnvVar envA("NIX_SVT_EESIG_A", "a1");
    ScopedEnvVar envB("NIX_SVT_EESIG_B", "b1");

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        // Group 1: [A] — sibling group SV will scan.
        db->record(ea, rootPath(), string_t{"eesig-A", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_EESIG_A", "a1")});
        // Group 2: [A, B] — latest.  Direct-hash targets this bucket
        // after mutation; SV will skip this bucket (structHash
        // matches directResult's target).
        db->record(ea, rootPath(), string_t{"eesig-AB", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_EESIG_A", "a1"),
             makeEnvVarDep(pools(), "NIX_SVT_EESIG_B", "b1")});
    });

    // Mutate A.  Direct hash on [A,B] deps resolves to (a2, b1);
    // recorded was (a1, b1); hashes don't match → direct-hash miss.
    // SV scans [A], which resolves A to a2 — mismatch against the
    // [A] bucket's stored A=a1.
    setenv("NIX_SVT_EESIG_A", "a2", 1);
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value())
        << "SV must not recover — A changed and no candidate matches";

    // The savings signal: the [A] candidate iterated fully with a
    // hash mismatch at index 0.  No resolveFail occurred.
    auto totals = scope.totals();
    EXPECT_EQ(totals.earlierHashMismatchCount, 0u)
        << "no resolveFail occurred — earlierHashMismatch must stay 0";
    EXPECT_GE(totals.hashMismatchOnlyCount, 1u)
        << "at least one candidate iterated fully with a hash mismatch";
    EXPECT_GE(totals.hashMismatchOnlySavedDeps, 1u)
        << "positive savings: (totalDeps - firstHashMismatchIdx) > 0";

    // Debug log fired with the new `SV early-exit signal` prefix.
    // Sample the captured messages so the test exposes concrete
    // evidence of instrumentation output.
    auto eeLogs = logs.matching("SV early-exit signal");
    ASSERT_GE(eeLogs.size(), 1u) << "SV early-exit signal log did not fire";

    // The [A] candidate's line must show firstHashMismatch=0,
    // firstResolveFail=none, totalDeps=1, outcome=hashMismatch.
    bool sawAMismatch = false;
    for (auto & m : eeLogs) {
        if (m.find("totalDeps=1") != std::string::npos
            && m.find("outcome=hashMismatch") != std::string::npos
            && m.find("firstResolveFail=none") != std::string::npos
            && m.find("firstHashMismatch=0") != std::string::npos) {
            sawAMismatch = true;
        }
    }
    EXPECT_TRUE(sawAMismatch)
        << "expected [A] candidate's log line with totalDeps=1, "
           "firstHashMismatch=0, firstResolveFail=none, "
           "outcome=hashMismatch. "
           "Captured lines:\n"
        << [&](){
            std::string out;
            for (auto & m : eeLogs) { out += "  "; out += m; out += '\n'; }
            return out;
        }();
}

// Multi-candidate signal: record several structural groups whose
// deps all resolve cleanly but produce hash mismatches at different
// positions.  Exercises the log line multiplicity and the per-bucket
// telemetry accumulation.  Also provides sample log output for the
// field report so that downstream consumers (grep, eval-trace-bench logs)
// see plausible multi-candidate traces.
TEST_F(TraceStoreTest, SVTelemetry_EarlyExitSignal_MultipleCandidates)
{
    SVTelemetryScope scope;
    DebugLogCaptureScope logs;
    evalSettings.useStructuralRecoveryMismatchTelemetry = true;

    ScopedEnvVar envA("NIX_SVT_MC_A", "a1");
    ScopedEnvVar envC("NIX_SVT_MC_C", "c1");
    ScopedEnvVar envD("NIX_SVT_MC_D", "d1");
    ScopedEnvVar envE("NIX_SVT_MC_E", "e1");

    // Every recorded bucket MUST contain A (the variable we mutate
    // below).  If a bucket's key set is disjoint from the mutated
    // variable, its deps all resolve to their stored hashes and SV
    // would happily recover that bucket — making the scenario a
    // recovery-success, not a scan-all-and-miss.  Four distinct
    // DepKeySetIds, all including A.
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"mc-A", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_MC_A", "a1")});
        db->record(ea, rootPath(), string_t{"mc-AC", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_MC_A", "a1"),
             makeEnvVarDep(pools(), "NIX_SVT_MC_C", "c1")});
        db->record(ea, rootPath(), string_t{"mc-AE", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_MC_A", "a1"),
             makeEnvVarDep(pools(), "NIX_SVT_MC_E", "e1")});
        // Latest: [A, D] — directHash target after mutation.
        db->record(ea, rootPath(), string_t{"mc-AD", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_MC_A", "a1"),
             makeEnvVarDep(pools(), "NIX_SVT_MC_D", "d1")});
    });

    // Mutate A.  Every stored bucket contains A with "a1"; current
    // resolves A to "a2" → mismatch in every bucket.  DirectHash on
    // [A,D] targets `hash(a2,d1)` — no match.  SV scans [A], [A,C],
    // [A,E] (skipping [A,D] whose structHash matches the direct
    // target).  Expected outcomes (A sorts first in every bucket):
    //   [A]    — hashMismatch at index 0, totalDeps=1, outcome=hashMismatch
    //   [A,C]  — hashMismatch at index 0, totalDeps=2, outcome=hashMismatch
    //   [A,E]  — hashMismatch at index 0, totalDeps=2, outcome=hashMismatch
    // No resolveFail in any bucket (every env-var resolves cleanly).
    setenv("NIX_SVT_MC_A", "a2", 1);
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value())
        << "every scanned bucket contains mutated A → all mismatch";

    auto totals = scope.totals();
    EXPECT_EQ(totals.succeeded, 0u)
        << "all three scanned buckets must fail — every bucket contains "
           "mutated A and A sorts first, so all mismatch at index 0";
    EXPECT_EQ(totals.earlierHashMismatchCount, 0u)
        << "no resolveFail occurred in any bucket";
    EXPECT_EQ(totals.bothSetCount, 0u)
        << "no bucket should populate BOTH hash-mismatch and "
           "resolve-fail indices — every env-var resolves cleanly";
    EXPECT_EQ(totals.hashMismatchOnlyCount, 3u)
        << "three scanned buckets should each record hashMismatchOnly";
    // Arithmetic pin on the savings accumulator: each bucket contributes
    // (totalDeps - firstHashMismatchIdx).  A sorts first in every
    // bucket, so firstHashMismatchIdx = 0 for each.  totalDeps is
    // 1 for [A] and 2 for each of [A,C] / [A,E].  Sum = 1+2+2 = 5.
    EXPECT_EQ(totals.hashMismatchOnlySavedDeps, 5u)
        << "savings = sum over scanned buckets of (totalDeps - 0): "
           "1 (bucket [A]) + 2 (bucket [A,C]) + 2 (bucket [A,E]) = 5";
    // Log line per candidate bucket scanned.  Count-only: the map
    // iteration order for `structGroups` is
    // `boost::unordered_flat_map`-defined and not reliable for
    // per-line assertions.
    auto eeLogs = logs.matching("SV early-exit signal");
    EXPECT_EQ(eeLogs.size(), 3u)
        << "expected exactly one SV early-exit signal line per "
           "scanned candidate bucket (3 buckets after skip).  "
           "Captured:\n"
        << [&](){
            std::string out;
            for (auto & m : eeLogs) { out += "  "; out += m; out += '\n'; }
            return out;
        }();
    // Every per-candidate log line must be for the hashMismatch
    // outcome (no abort, no win) and must record
    // `firstResolveFail=none` (no resolveFail occurred).
    for (const auto & m : eeLogs) {
        EXPECT_NE(m.find("outcome=hashMismatch"), std::string::npos)
            << "expected outcome=hashMismatch on every line, got: " << m;
        EXPECT_NE(m.find("firstResolveFail=none"), std::string::npos)
            << "expected firstResolveFail=none on every line, got: " << m;
        EXPECT_NE(m.find("firstHashMismatch=0"), std::string::npos)
            << "expected firstHashMismatch=0 on every line, got: " << m;
    }
}

// Positive test for `earlierHashMismatchCount` — the core savings
// signal.  None of the other tests in this file exercise it, because
// EnvVar-only buckets never hit the resolveFail branch.  To trigger
// `bothSetCount++` and `earlierHashMismatchCount++`, a single candidate
// must encounter a hash mismatch on one dep, then a resolveFail on a
// later dep (the loop only breaks on resolveFail, not on mismatch).
//
// Setup: bucket `[EnvVar(A), TraceValueContext(<missing-node>)]`.
// - Canonical sort: EnvironmentLookup (kind=3) < TraceValueContext
//   (kind=14), so EnvVar is at index 0, TraceValueContext at index 1.
// - Mutate A: stored hash of "a1" no longer matches current "a2"
//   → `firstHashMismatchIdx = 0`.  Loop does NOT break on mismatch.
// - TraceValueContext on a nonexistent current node: trace-context resolution returns
//   nullopt → `firstResolveFailIdx = 1`.  Loop breaks.
//
// A second bucket `[A, B]` is recorded as the latest trace so its
// structHash becomes the directHash target (which misses after the A
// mutation).  SV then scans the remaining bucket.
TEST_F(TraceStoreTest, SVTelemetry_EarlyExitSignal_EarlierHashMismatch)
{
    SVTelemetryScope scope;
    DebugLogCaptureScope logs;
    evalSettings.useStructuralRecoveryMismatchTelemetry = true;

    ScopedEnvVar envA("NIX_SVT_EHM_A", "a1");
    ScopedEnvVar envB("NIX_SVT_EHM_B", "b1");

    // Use a valid vocab path that has no current node.  TraceValueContext
    // contributes to trace hashes, unlike GitRevisionIdentity, so it still
    // participates in the SV candidate loop and deterministically resolveFails.
    auto missingContextPath = vpath({"missing-trace-context-node"});
    auto missingContext = Dep::makeValueContext(
        missingContextPath,
        DepHashValue(depHash("missing-trace-context")));

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        // Bucket 1: [EnvVar(A), TraceValueContext(<missing-node>)].
        // The trace-context dep resolves to nullopt inside the SV loop →
        // firstResolveFailIdx=1.
        db->record(ea, rootPath(), string_t{"ehm-AT", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_EHM_A", "a1"),
             missingContext});
        // Bucket 2 (latest): [A, B] — directHash target after mutation.
        db->record(ea, rootPath(), string_t{"ehm-AB", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_EHM_A", "a1"),
             makeEnvVarDep(pools(), "NIX_SVT_EHM_B", "b1")});
    });

    // Mutate A.  directHash on [A, B] computes hash(a2, b1) — no
    // match.  SV scans the [A, TraceValueContext] bucket.
    setenv("NIX_SVT_EHM_A", "a2", 1);
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value())
        << "no bucket recovers — [A,TraceValueContext] aborts on TraceValueContext, "
           "[A,B] is the directHash target and skipped";

    auto totals = scope.totals();
    EXPECT_EQ(totals.succeeded, 0u);
    EXPECT_GE(totals.abortedEarly, 1u)
        << "[A,TraceValueContext] must abort on the trace-context resolveFail";
    // The gating-signal assertions: one candidate observed BOTH a
    // hash mismatch AND a resolveFail, with the mismatch earlier.
    EXPECT_EQ(totals.bothSetCount, 1u)
        << "one candidate ([A,TraceValueContext]) records both a hash mismatch "
           "(on A at idx 0) and a resolveFail (on TraceValueContext at idx 1)";
    EXPECT_EQ(totals.earlierHashMismatchCount, 1u)
        << "the mismatch at idx 0 precedes the resolveFail at idx 1";
    // Savings = (firstResolveFailIdx - firstHashMismatchIdx) = 1 - 0 = 1.
    EXPECT_EQ(totals.earlierHashMismatchSavedDeps, 1u)
        << "saving = firstResolveFailIdx - firstHashMismatchIdx";
    // `hashMismatchOnly*` tracks candidates that iterate FULLY without
    // a resolveFail.  The [A,TraceValueContext] bucket doesn't qualify (it
    // aborted).  Stays zero.
    EXPECT_EQ(totals.hashMismatchOnlyCount, 0u);
    EXPECT_EQ(totals.hashMismatchOnlySavedDeps, 0u);

    auto eeLogs = logs.matching("SV early-exit signal");
    ASSERT_EQ(eeLogs.size(), 1u) << "one scanned bucket → one log line";
    const auto & m = eeLogs[0];
    EXPECT_NE(m.find("outcome=abortedEarly"), std::string::npos) << m;
    EXPECT_NE(m.find("firstHashMismatch=0"), std::string::npos) << m;
    EXPECT_NE(m.find("firstResolveFail=1"), std::string::npos) << m;
    EXPECT_NE(m.find("totalDeps=2"), std::string::npos) << m;
}

// Log-format coverage for the `win` outcome.
// `SVTelemetry_EarlyExitSignal_HashMismatchOnly` covers the
// hashMismatch log line; the abortedEarly line is covered by
// `EarlierHashMismatch` above.  This fills the last outcome.
TEST_F(TraceStoreTest, SVTelemetry_EarlyExitSignal_WinLogLine)
{
    SVTelemetryScope scope;
    DebugLogCaptureScope logs;
    evalSettings.useStructuralRecoveryMismatchTelemetry = true;

    // Same shape as the SuccessCase test: [A] sibling bucket recovers
    // when [A, B] directHash misses after mutating B.
    ScopedEnvVar envA("NIX_SVT_WIN_A", "aval");
    ScopedEnvVar envB("NIX_SVT_WIN_B", "bval");

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"win-A", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_WIN_A", "aval")});
        db->record(ea, rootPath(), string_t{"win-AB", {}},
            {makeEnvVarDep(pools(), "NIX_SVT_WIN_A", "aval"),
             makeEnvVarDep(pools(), "NIX_SVT_WIN_B", "bval")});
    });

    setenv("NIX_SVT_WIN_B", "bval-new", 1);
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value()) << "SV should recover via [A] bucket";

    auto eeLogs = logs.matching("SV early-exit signal");
    ASSERT_GE(eeLogs.size(), 1u);
    // At least one log line must be the win line.  (Other candidates
    // might also fire before the winner is found; we assert only that
    // a win line exists with the right shape.)
    bool sawWin = false;
    for (const auto & m : eeLogs) {
        if (m.find("outcome=win") != std::string::npos) {
            sawWin = true;
            EXPECT_NE(m.find("totalDeps=1"), std::string::npos) << m;
            // [A] has 1 dep, no mismatch (A unchanged), no resolveFail.
            EXPECT_NE(m.find("firstHashMismatch=none"), std::string::npos) << m;
            EXPECT_NE(m.find("firstResolveFail=none"), std::string::npos) << m;
        }
    }
    EXPECT_TRUE(sawWin)
        << "expected at least one log line with outcome=win.  Captured:\n"
        << [&](){
            std::string out;
            for (auto & m : eeLogs) { out += "  "; out += m; out += '\n'; }
            return out;
        }();
}

// JSON-schema test for `renderSVCandidateStatsJson`.  Previously the
// emitter was inlined in `printStatistics`; factoring it out lets us
// assert the schema directly against a populated map instead of
// spawning a subprocess and parsing stderr.  Guards against silent
// field drops from the `byDepKeySet` payload that `eval-trace-bench`
// analyzers consume.
TEST_F(TraceStoreTest, SVTelemetry_JsonRender_SchemaAndSortOrder)
{
    SVTelemetryScope scope;

    // Populate the map directly: two buckets with distinct `tried`
    // counts (asserts sort-order by tried-descending) and one extra
    // bucket at the same tried count as the second (asserts secondary
    // sort by depKeySetId-ascending).
    //
    // Fields are populated with distinguishable values so we can
    // round-trip-check each.
    eval_trace::recordSVCandidate(
        /*depKeySetId*/ 42u,
        /*tried*/ true, /*succeeded*/ true,
        /*abortedEarly*/ false, /*hashMismatch*/ false,
        /*depsResolved*/ 10, /*depResolveUs*/ 50,
        /*firstHashMismatch*/ std::nullopt,
        /*firstResolveFail*/ std::nullopt,
        /*totalDeps*/ 10);
    // Record the high-`tried` bucket a second + third time so it sorts
    // first in the output.
    eval_trace::recordSVCandidate(10u, true, false, true, false,
        5, 25, std::optional<uint64_t>{0u},
        std::optional<uint64_t>{3u}, 8);
    eval_trace::recordSVCandidate(10u, true, false, false, true,
        8, 40, std::nullopt, std::nullopt, 8);
    // And a third bucket at the same `tried` count as bucket 42 — tests
    // the secondary sort by depKeySetId ascending.
    eval_trace::recordSVCandidate(1u, true, false, false, true,
        3, 15, std::nullopt, std::nullopt, 3);

    auto snap = eval_trace::snapshotSVCandidateStats();
    ASSERT_EQ(snap.size(), 3u);

    auto arr = eval_trace::renderSVCandidateStatsJson(snap);
    ASSERT_TRUE(arr.is_array());
    ASSERT_EQ(arr.size(), 3u);

    // Every entry must carry the full schema — if a future refactor
    // drops any of these, this assertion pins the contract.
    static const std::vector<std::string> expectedFields = {
        "depKeySetId", "tried", "succeeded", "abortedEarly",
        "hashMismatch", "avgDeps", "avgUs",
        "bothSetCount", "earlierHashMismatchCount",
        "earlierHashMismatchSavedDeps",
        "hashMismatchOnlyCount", "hashMismatchOnlySavedDeps",
    };
    for (const auto & entry : arr) {
        for (const auto & field : expectedFields) {
            EXPECT_TRUE(entry.contains(field))
                << "schema missing field '" << field << "' in entry: "
                << entry.dump();
        }
    }

    // Sort order: first entry is the 2-tries bucket (id=10), then
    // by depKeySetId asc for the two 1-try buckets → 1, then 42.
    EXPECT_EQ(arr[0]["depKeySetId"].get<uint32_t>(), 10u);
    EXPECT_EQ(arr[0]["tried"].get<uint32_t>(), 2u);
    EXPECT_EQ(arr[1]["depKeySetId"].get<uint32_t>(), 1u);
    EXPECT_EQ(arr[2]["depKeySetId"].get<uint32_t>(), 42u);

    // Round-trip each savings field we care about against the 10-bucket
    // entry (the one that carries early-exit-signal values).
    EXPECT_EQ(arr[0]["bothSetCount"].get<uint32_t>(), 1u)
        << "bucket 10 had one candidate with both indices set";
    EXPECT_EQ(arr[0]["earlierHashMismatchCount"].get<uint32_t>(), 1u);
    EXPECT_EQ(arr[0]["earlierHashMismatchSavedDeps"].get<uint64_t>(), 3u)
        << "saving = 3 - 0 = 3";
    EXPECT_EQ(arr[0]["hashMismatchOnlyCount"].get<uint32_t>(), 0u);
    EXPECT_EQ(arr[0]["hashMismatchOnlySavedDeps"].get<uint64_t>(), 0u);

    // Averaging arithmetic: bucket 10 recorded twice with
    // depsResolved {5, 8} → sum 13, tried 2, avgDeps = 6.5.
    // depResolveUs {25, 40} → sum 65, tried 2, avgUs = 32.5.
    EXPECT_DOUBLE_EQ(arr[0]["avgDeps"].get<double>(), 6.5);
    EXPECT_DOUBLE_EQ(arr[0]["avgUs"].get<double>(), 32.5);
    // Single-record buckets: avg = sum / 1 = sum.
    EXPECT_DOUBLE_EQ(arr[1]["avgDeps"].get<double>(), 3.0);   // bucket 1
    EXPECT_DOUBLE_EQ(arr[1]["avgUs"].get<double>(), 15.0);
    EXPECT_DOUBLE_EQ(arr[2]["avgDeps"].get<double>(), 10.0);  // bucket 42
    EXPECT_DOUBLE_EQ(arr[2]["avgUs"].get<double>(), 50.0);

    // Buckets that never recorded an early-exit signal must still emit
    // zero-valued fields (not be absent).
    EXPECT_EQ(arr[1]["bothSetCount"].get<uint32_t>(), 0u);
    EXPECT_EQ(arr[2]["bothSetCount"].get<uint32_t>(), 0u);
}

// Empty-snapshot case: the render function returns an empty array
// rather than an error or absent field.  Matches the `if (!svMap.
// empty())` guard in `printStatistics` but documents the contract
// explicitly so a future refactor can rely on it.
TEST_F(TraceStoreTest, SVTelemetry_JsonRender_EmptySnapshot)
{
    SVTelemetryScope scope;
    eval_trace::SVCandidateStatsMap empty;
    auto arr = eval_trace::renderSVCandidateStatsJson(empty);
    EXPECT_TRUE(arr.is_array());
    EXPECT_EQ(arr.size(), 0u);
}

} // namespace nix::eval_trace
