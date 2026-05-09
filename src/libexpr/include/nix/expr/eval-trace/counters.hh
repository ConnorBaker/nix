#pragma once
///@file

#include "nix/expr/counter.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <nlohmann/json_fwd.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace nix::eval_trace {

// ── Timing helpers (no-op when NIX_SHOW_STATS is unset) ──────────────

inline auto timerStart()
{
    return Counter::enabled ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};
}

inline uint64_t elapsedUs(std::chrono::steady_clock::time_point start)
{
    if (!Counter::enabled) return 0;
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
}

// ── Cache hit/miss ────────────────────────────────────────────────────
extern Counter nrTraceCacheHits;
extern Counter nrTraceCacheMisses;
extern Counter nrTraceVerifications;
extern Counter nrVerificationsPassed;
extern Counter nrVerificationsFailed;
extern Counter nrDepsChecked;
extern Counter nrRecoveryFailures;

// ── Timing accumulators (microseconds, gated on Counter::enabled) ────
extern Counter nrVerifyTimeUs;
extern Counter nrVerifyTraceTimeUs;
extern Counter nrRecoveryTimeUs;
extern Counter nrRecoveryDirectHashTimeUs;
extern Counter nrRecoveryStructVariantTimeUs;
extern Counter nrRecordTimeUs;
extern Counter nrRecordHashUs;
extern Counter nrRecordSerializeKeysUs;
extern Counter nrRecordSerializeValuesUs;
extern Counter nrRecordFlushUs;
extern Counter nrLoadTraceTimeUs;
extern Counter nrDbInitTimeUs;
extern Counter nrDbCloseTimeUs;

// ── Event counters ───────────────────────────────────────────────────
extern Counter nrRecoveryAttempts;
extern Counter nrRecoveryDirectHashHits;
extern Counter nrRecoveryStructVariantHits;
extern Counter nrRecords;
extern Counter nrLoadTraces;

// ── Key-set loading ──────────────────────────────────────────────────
extern Counter nrLoadKeySets;
extern Counter nrLoadKeySetCacheHits;
extern Counter nrLoadKeySetCacheMisses;
extern Counter nrLoadKeySetTimeUs;

// ── TracedExpr creation/forcing breakdown ────────────────────────────
extern Counter nrTracedExprCreated;
extern Counter nrTracedExprFromMaterialize;
extern Counter nrTracedExprFromDataFile;
extern Counter nrTracedExprForced;
extern Counter nrLazyStateAllocated;

// ── Data-file node type breakdown (ExprTracedData::eval) ─────────────
extern Counter nrDataFileScalarChildren;
extern Counter nrDataFileContainerChildren;

// ── DepRecordingContext scope operations ───────────────────────────────
extern Counter nrDepContextScopes;
extern Counter nrOwnDepsTotal;
extern Counter nrOwnDepsMax;

// ── replayMemoizedDeps breakdown ─────────────────────────────────────
extern Counter nrReplayTotalCalls;
extern Counter nrReplayBloomHits;
extern Counter nrReplayEpochHits;
extern Counter nrReplayAdded;

// ── Per-dep-type hash computation timing (microseconds) ─────────────
extern Counter nrDepHashContentUs;
extern Counter nrDepHashDirectoryUs;
extern Counter nrDepHashExistenceUs;
extern Counter nrDepHashStructuredJsonUs;
extern Counter nrDepHashStructuredTomlUs;
extern Counter nrDepHashStructuredDirUs;
extern Counter nrDepHashStructuredNixUs;
extern Counter nrDepHashStorePathUs;
extern Counter nrDepHashStructuredOuterUs;
extern Counter nrDepHashGitIdentityUs;
extern Counter nrDepHashGitIdentityMisses;
extern Counter nrDepHashOtherUs;

// ── resolveCurrentDepHash cache tracking ────────────────────────────
extern Counter nrDepHashCacheHits;
extern Counter nrDepHashCacheMisses;
extern Counter nrDepHashStructuredMisses;
extern Counter nrContentSubsumptionSkips;

// ── Recovery dep recomputation breakdown ────────────────────────────
extern Counter nrRecoveryDepRecomputeUs;
extern Counter nrRecoveryDepRecomputeCount;

// ── Recovery lookup / accept breakdown ───────────────────────────────
extern Counter nrRecoveryLatestHistoryLookupCount;
extern Counter nrRecoveryLatestHistoryLookupUs;
extern Counter nrRecoveryDirectHashLookupCount;
extern Counter nrRecoveryDirectHashLookupRows;
extern Counter nrRecoveryDirectHashLookupUs;
extern Counter nrRecoveryGitIdentityLookupCount;
extern Counter nrRecoveryGitIdentityLookupRows;
extern Counter nrRecoveryGitIdentityLookupUs;
extern Counter nrRecoveryScanHistoryCount;
extern Counter nrRecoveryScanHistoryRows;
extern Counter nrRecoveryScanHistoryUs;

extern Counter nrRecoveryGitIdentityAttempts;
extern Counter nrRecoveryGitIdentityCandidates;
extern Counter nrRecoveryGitIdentityAccepted;
extern Counter nrRecoveryGitIdentityRejected;
extern Counter nrRecoveryGitIdentityTimeUs;

extern Counter nrRecoveryImplicitGuardCandidates;
extern Counter nrRecoveryImplicitGuardFullTraceLoads;
extern Counter nrRecoveryImplicitGuardChecks;
extern Counter nrRecoveryImplicitGuardFailures;
extern Counter nrRecoveryImplicitGuardTimeUs;

// ── SC/IS dep detail breakdown ──────────────────────────────────────
extern Counter nrDepHashScDirSetMisses;
extern Counter nrDepHashScJsonParseUs;

extern Counter nrRecoveryGitIdentityHits;

/// Incremented when the verifier's primary `lookupCurrentNode`
/// misses and `scanHistory(stableRecoveryKey)` finds a row to
/// bootstrap from (verifier.cc `verifyAttrImpl`).
/// Distinct from `nrRecoveryAttempts`, which only counts the 3-strategy
/// recovery fallback triggered on `verifyTrace` failure. History
/// bootstrap runs BEFORE verifyTrace, so a successful bootstrap leaves
/// nrRecoveryAttempts==0 even though the served result came from
/// History lookup, not primary session cache.
extern Counter nrHistoryBootstraps;

// ── Structural variant loop breakdown ───────────────────────────────
extern Counter nrStructVariantCandidates;
extern Counter nrStructVariantDepsResolved;
extern Counter nrStructVariantLoadKeySetUs;
extern Counter nrStructVariantHashUs;
extern Counter nrStructVariantDepResolveUs;

// ── Per-DepKeySetId SV candidate telemetry ──────────────────────────
//
// Process-scoped aggregator of SV candidate outcomes.  Populated by
// `tryStructuralVariantRecovery` (verifier.cc) as each
// candidate bucket is tried.  Emitted in `NIX_SHOW_STATS` JSON under
// `evalTrace.structVariant.byDepKeySet` when non-empty.
//
// Keyed by `DepKeySetId.value` — IDs are content-addressed by
// `struct_hash` within a DB so a given ID carries the same meaning
// across sessions on the same DB.  The map is cleared on every session
// open (`Verifier::resetVerificationState` calls
// `clearSVCandidateStats()`) so that `NIX_SHOW_STATS` output describes
// only the just-started session.
//
// No lock: SV runs inside `withExclusiveAccess` on TraceStore, which
// serializes the store's mutations.  The snapshot path runs at
// `EvalState::printStatistics()` after all evaluation has ended, and
// the clear path runs on session open before any concurrent SV can
// execute.
struct SVCandidateStats {
    uint32_t tried = 0;           ///< bucket entries considered
    uint32_t succeeded = 0;       ///< accepted → return
    uint32_t abortedEarly = 0;    ///< repComputable=false broke out
    uint32_t hashMismatch = 0;    ///< full loop, lookupCandidate missed
    uint64_t depsResolvedSum = 0; ///< cumulative deps resolved
    uint64_t depResolveUsSum = 0; ///< cumulative dep resolve time

    /// ── Any-dep early-exit gating signal ────────────────────────────
    ///
    /// Measurement-only: whether SV aborted earlier on a hash mismatch
    /// than on a resolveFail (i.e., would "break on first current !=
    /// dep.hash" save work in singleton-DepKeySetId buckets where any
    /// single-dep mismatch already guarantees the final
    /// `computePresortedTraceHash(repDeps)` does not match).
    ///
    /// Populated per candidate:
    /// - `bothSetCount`: candidates where BOTH a hash mismatch AND a
    ///   resolveFail index were observed (iteration ran at least until
    ///   both events, since hash-mismatch scanning does NOT break).
    /// - `earlierHashMismatchCount`: subset of `bothSetCount` where the
    ///   first hash mismatch strictly preceded the first resolveFail —
    ///   i.e., the proposed early-exit WOULD have saved work on this
    ///   candidate.
    /// - `earlierHashMismatchSavedDeps`: sum over
    ///   `earlierHashMismatchCount` candidates of
    ///   `(firstResolveFailIdx - firstHashMismatchIdx)`.  This is the
    ///   raw savings signal: total dep-iteration work avoided if we
    ///   had broken at the first hash mismatch.
    /// - `hashMismatchOnlyCount`: candidates that iterated fully (no
    ///   resolveFail) and had at least one hash mismatch.  The proposal
    ///   would still short-circuit these, saving
    ///   `(totalDeps - firstHashMismatchIdx)` dep iterations per
    ///   candidate, BUT these already complete the existing loop, so
    ///   the savings interacts with existing hash-mismatch outcome
    ///   accounting — reported separately for completeness.
    /// - `hashMismatchOnlySavedDeps`: corresponding cumulative saving
    ///   for the `hashMismatchOnly` slice.
    uint32_t bothSetCount = 0;
    uint32_t earlierHashMismatchCount = 0;
    uint64_t earlierHashMismatchSavedDeps = 0;
    uint32_t hashMismatchOnlyCount = 0;
    uint64_t hashMismatchOnlySavedDeps = 0;
};

/// Snapshot of the telemetry map keyed by `DepKeySetId::value`.
using SVCandidateStatsMap = boost::unordered_flat_map<uint32_t, SVCandidateStats>;

/// Record a single SV candidate outcome.  Call from within the SV
/// candidate loop.  No-op when `Counter::enabled` is false.
///
/// `firstHashMismatchIdx` / `firstResolveFailIdx` parameters carry
/// the gating-question signal for SV any-dep early-exit (see the
/// audit at `doc/eval-trace/claude-md-audit.md`, §3.4
/// Proposal 1).  Hash-mismatch indices are populated only when
/// `eval-trace-structural-recovery-mismatch-telemetry` is enabled;
/// otherwise they are `std::nullopt` so recovery can preload key sets
/// without decoding historical value blobs.  Index values are 0-based
/// positions in the candidate's dep-key iteration.  `std::nullopt` means the
/// event was not observed during this candidate's iteration.
/// `totalDeps` is the candidate's full dep-list size (not the
/// count of successfully-resolved deps); needed for
/// `hashMismatchOnly` savings computation.
void recordSVCandidate(
    uint32_t depKeySetIdValue,
    bool tried,
    bool succeeded,
    bool abortedEarly,
    bool hashMismatch,
    uint64_t depsResolved,
    uint64_t depResolveUs,
    std::optional<uint64_t> firstHashMismatchIdx,
    std::optional<uint64_t> firstResolveFailIdx,
    uint64_t totalDeps);

/// Copy the current telemetry map for emission.  Called at
/// `printStatistics()` time; returns an empty map when SV never fired.
SVCandidateStatsMap snapshotSVCandidateStats();

/// Render an SV telemetry snapshot into a JSON array (schema below).
/// The output is bucket-sorted by `tried` descending, then by
/// `depKeySetId` ascending, so cross-run diffs are stable.  Each
/// array entry has the schema:
///     {depKeySetId, tried, succeeded, abortedEarly, hashMismatch,
///      avgDeps, avgUs, bothSetCount, earlierHashMismatchCount,
///      earlierHashMismatchSavedDeps, hashMismatchOnlyCount,
///      hashMismatchOnlySavedDeps}
/// Factored out of `printStatistics` so unit tests can assert the
/// schema without spawning a subprocess.  Defined in `counters.cc`
/// to keep the `<nlohmann/json.hpp>` body out of this header (this
/// file is transitively included by a large part of libexpr; using
/// the forward-decl header keeps compile times bounded).
nlohmann::json renderSVCandidateStatsJson(const SVCandidateStatsMap & snapshot);

/// Reset the telemetry map.  Production caller:
/// `Verifier::resetVerificationState`, which fires on
/// every session open so per-session emission sees only the current
/// session's outcomes.  Test caller: the `SVTelemetryScope` RAII helper
/// in `sv-telemetry.cc`, which clears the map on setup/teardown so
/// each test starts from a known-empty state.
void clearSVCandidateStats();

// ── Observability: setup/fallback/classification counters ───────────
//
// These are diagnostic counters, not hot-path measurements. Non-zero
// values for the latter three are expected under some workloads — see
// the inline comments at their increment sites.

/// TraceSession construction caught an exception during
/// makeTraceBackend / setSessionConfig / loadAndVerifyRuntimeRoots /
/// bindSession and reset backend to null. Non-zero here means
/// the session is effectively disabled for this eval — every lookup
/// will miss. Zero under normal operation.
extern Counter nrTraceBackendSetupFailed;

/// resolveDepPathKey tier-1: registry reverseResolve returned a
/// Registered source. Hot path for all mounted-flake evaluations.
extern Counter nrResolveViaRegistry;

/// resolveDepPathKey tier-2: registry miss, PathObject provenance
/// resolved the source. Fires for runtime fetchTree subdir navigation
/// and dirty/unlocked path inputs.
extern Counter nrResolveViaPathObject;

/// resolveDepPathKey tier-3: fell through to an absolute-path
/// DepSource. Expected for /nix/store entries and NIX_PATH-relative
/// absolute paths evaluated outside a flake-input context. Spikes
/// here vs baseline are the canonical signal for session-identity
/// instability (see Known Limitations in this dir's CLAUDE.md).
extern Counter nrResolveViaAbsolute;

/// recordStructuredDep hit by a caller with no active
/// DepRecordingContext (neither fiber nor standalone). The call-site
/// comment in recording.cc labels this a legitimate "scopeless
/// internal eval" path, so non-zero is expected. Counter exists so
/// that a spike vs baseline is visible from a benchmark JSON dump.
extern Counter nrDepRecordNoActiveContext;

} // namespace nix::eval_trace
