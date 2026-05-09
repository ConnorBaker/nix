#include "nix/expr/eval-trace/counters.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace nix::eval_trace {

// ── Cache hit/miss ────────────────────────────────────────────────────
Counter nrTraceCacheHits;
Counter nrTraceCacheMisses;
Counter nrTraceVerifications;
Counter nrVerificationsPassed;
Counter nrVerificationsFailed;
Counter nrDepsChecked;
Counter nrRecoveryFailures;

// ── Timing accumulators (microseconds) ───────────────────────────────
Counter nrVerifyTimeUs;
Counter nrVerifyTraceTimeUs;
Counter nrRecoveryTimeUs;
Counter nrRecoveryDirectHashTimeUs;
Counter nrRecoveryStructVariantTimeUs;
Counter nrRecordTimeUs;
Counter nrRecordHashUs;
Counter nrRecordSerializeKeysUs;
Counter nrRecordSerializeValuesUs;
Counter nrRecordFlushUs;
Counter nrLoadTraceTimeUs;
Counter nrDbInitTimeUs;
Counter nrDbCloseTimeUs;

// ── Event counters ───────────────────────────────────────────────────
Counter nrRecoveryAttempts;
Counter nrRecoveryDirectHashHits;
Counter nrRecoveryStructVariantHits;
Counter nrRecords;
Counter nrLoadTraces;

// ── Key-set loading ──────────────────────────────────────────────────
Counter nrLoadKeySets;
Counter nrLoadKeySetCacheHits;
Counter nrLoadKeySetCacheMisses;
Counter nrLoadKeySetTimeUs;

// ── TracedExpr creation/forcing breakdown ────────────────────────────
Counter nrTracedExprCreated;
Counter nrTracedExprFromMaterialize;
Counter nrTracedExprFromDataFile;
Counter nrTracedExprForced;
Counter nrLazyStateAllocated;

// ── Data-file node type breakdown ────────────────────────────────────
Counter nrDataFileScalarChildren;
Counter nrDataFileContainerChildren;

// ── DepRecordingContext scope operations ───────────────────────────────
Counter nrDepContextScopes;
Counter nrOwnDepsTotal;
Counter nrOwnDepsMax;

// ── replayMemoizedDeps breakdown ─────────────────────────────────────
Counter nrReplayTotalCalls;
Counter nrReplayBloomHits;
Counter nrReplayEpochHits;
Counter nrReplayAdded;

// ── Per-dep-type hash computation timing (microseconds) ─────────────
Counter nrDepHashContentUs;
Counter nrDepHashDirectoryUs;
Counter nrDepHashExistenceUs;
Counter nrDepHashStructuredJsonUs;
Counter nrDepHashStructuredTomlUs;
Counter nrDepHashStructuredDirUs;
Counter nrDepHashStructuredNixUs;
Counter nrDepHashStorePathUs;
Counter nrDepHashStructuredOuterUs;
Counter nrDepHashGitIdentityUs;
Counter nrDepHashGitIdentityMisses;
Counter nrDepHashOtherUs;

// ── resolveCurrentDepHash cache tracking ────────────────────────────
Counter nrDepHashCacheHits;
Counter nrDepHashCacheMisses;
Counter nrDepHashStructuredMisses;
Counter nrContentSubsumptionSkips;

// ── Recovery dep recomputation breakdown ────────────────────────────
Counter nrRecoveryDepRecomputeUs;
Counter nrRecoveryDepRecomputeCount;

// ── Recovery lookup / accept breakdown ───────────────────────────────
Counter nrRecoveryLatestHistoryLookupCount;
Counter nrRecoveryLatestHistoryLookupUs;
Counter nrRecoveryDirectHashLookupCount;
Counter nrRecoveryDirectHashLookupRows;
Counter nrRecoveryDirectHashLookupUs;
Counter nrRecoveryGitIdentityLookupCount;
Counter nrRecoveryGitIdentityLookupRows;
Counter nrRecoveryGitIdentityLookupUs;
Counter nrRecoveryScanHistoryCount;
Counter nrRecoveryScanHistoryRows;
Counter nrRecoveryScanHistoryUs;

Counter nrRecoveryGitIdentityAttempts;
Counter nrRecoveryGitIdentityCandidates;
Counter nrRecoveryGitIdentityAccepted;
Counter nrRecoveryGitIdentityRejected;
Counter nrRecoveryGitIdentityTimeUs;

Counter nrRecoveryImplicitGuardCandidates;
Counter nrRecoveryImplicitGuardFullTraceLoads;
Counter nrRecoveryImplicitGuardChecks;
Counter nrRecoveryImplicitGuardFailures;
Counter nrRecoveryImplicitGuardTimeUs;

// ── StructuredProjection/ImplicitStructure dep detail breakdown ──────
Counter nrDepHashScDirSetMisses;
Counter nrDepHashScJsonParseUs;

Counter nrRecoveryGitIdentityHits;

Counter nrHistoryBootstraps;

// ── Structural variant loop breakdown ───────────────────────────────
Counter nrStructVariantCandidates;
Counter nrStructVariantDepsResolved;
Counter nrStructVariantLoadKeySetUs;
Counter nrStructVariantHashUs;
Counter nrStructVariantDepResolveUs;

// ── Per-DepKeySetId SV candidate telemetry ──────────────────────────
//
// The map is file-scope static to this TU — no global linkage.  Reads
// and writes go through the `recordSVCandidate` / `snapshotSVCandidateStats`
// / `clearSVCandidateStats` API.  No lock: SV runs under
// `ExclusiveTraceStoreAccess`, which serializes every SV mutation; the
// reset and snapshot paths are called from the single-threaded stats /
// session-open callers.
namespace {
SVCandidateStatsMap svCandidateStats;
}

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
    uint64_t totalDeps)
{
    if (!Counter::enabled)
        return;
    auto & entry = svCandidateStats[depKeySetIdValue];
    if (tried)        entry.tried++;
    if (succeeded)    entry.succeeded++;
    if (abortedEarly) entry.abortedEarly++;
    if (hashMismatch) entry.hashMismatch++;
    entry.depsResolvedSum += depsResolved;
    entry.depResolveUsSum += depResolveUs;

    // SV any-dep early-exit gating signal.  The core question is:
    // when SV aborts-early on resolveFail at dep N, did we already
    // observe a hash mismatch at dep M < N?  If yes, breaking at M
    // saves (N - M) iterations per candidate.
    if (firstHashMismatchIdx.has_value() && firstResolveFailIdx.has_value()) {
        entry.bothSetCount++;
        if (*firstHashMismatchIdx < *firstResolveFailIdx) {
            entry.earlierHashMismatchCount++;
            entry.earlierHashMismatchSavedDeps +=
                (*firstResolveFailIdx - *firstHashMismatchIdx);
        }
    } else if (firstHashMismatchIdx.has_value()
               && !firstResolveFailIdx.has_value()) {
        // Candidate iterated fully (no resolveFail) but a hash
        // mismatch was observed.  The proposal would short-circuit
        // at M, saving (totalDeps - M) iterations.  These candidates
        // are distinct from the abort-early slice and reported
        // separately.
        entry.hashMismatchOnlyCount++;
        if (totalDeps > *firstHashMismatchIdx)
            entry.hashMismatchOnlySavedDeps +=
                (totalDeps - *firstHashMismatchIdx);
    }
}

SVCandidateStatsMap snapshotSVCandidateStats()
{
    return svCandidateStats;
}

nlohmann::json renderSVCandidateStatsJson(const SVCandidateStatsMap & snapshot)
{
    std::vector<std::pair<uint32_t, SVCandidateStats>> sorted;
    sorted.reserve(snapshot.size());
    for (auto & kv : snapshot)
        sorted.emplace_back(kv.first, kv.second);
    std::sort(sorted.begin(), sorted.end(),
        [](const auto & a, const auto & b) {
            if (a.second.tried != b.second.tried)
                return a.second.tried > b.second.tried;
            return a.first < b.first;
        });

    nlohmann::json out = nlohmann::json::array();
    for (auto & [id, stats] : sorted) {
        double avgDeps = stats.tried
            ? static_cast<double>(stats.depsResolvedSum) / stats.tried
            : 0.0;
        double avgUs = stats.tried
            ? static_cast<double>(stats.depResolveUsSum) / stats.tried
            : 0.0;
        out.push_back({
            {"depKeySetId", id},
            {"tried", stats.tried},
            {"succeeded", stats.succeeded},
            {"abortedEarly", stats.abortedEarly},
            {"hashMismatch", stats.hashMismatch},
            {"avgDeps", avgDeps},
            {"avgUs", avgUs},
            {"bothSetCount", stats.bothSetCount},
            {"earlierHashMismatchCount", stats.earlierHashMismatchCount},
            {"earlierHashMismatchSavedDeps", stats.earlierHashMismatchSavedDeps},
            {"hashMismatchOnlyCount", stats.hashMismatchOnlyCount},
            {"hashMismatchOnlySavedDeps", stats.hashMismatchOnlySavedDeps},
        });
    }
    return out;
}

void clearSVCandidateStats()
{
    svCandidateStats.clear();
}

// ── Observability: setup/fallback/classification counters ───────────
Counter nrTraceBackendSetupFailed;
Counter nrResolveViaRegistry;
Counter nrResolveViaPathObject;
Counter nrResolveViaAbsolute;
Counter nrDepRecordNoActiveContext;

} // namespace nix::eval_trace
