#pragma once
///@file

#include "nix/expr/counter.hh"

#include <chrono>

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
extern Counter nrLoadTraceTimeUs;
extern Counter nrDbInitTimeUs;
extern Counter nrDbCloseTimeUs;

// ── Event counters ───────────────────────────────────────────────────
extern Counter nrRecoveryAttempts;
extern Counter nrRecoveryDirectHashHits;
extern Counter nrRecoveryStructVariantHits;
extern Counter nrRecords;
extern Counter nrLoadTraces;

// ── TracedExpr creation/forcing breakdown ────────────────────────────
extern Counter nrTracedExprCreated;
extern Counter nrTracedExprFromMaterialize;
extern Counter nrTracedExprFromOrigAttrs;
extern Counter nrTracedExprFromDataFile;
extern Counter nrTracedExprForced;
extern Counter nrLazyStateAllocated;

// ── Data-file node type breakdown (ExprTracedData::eval) ─────────────
extern Counter nrDataFileScalarChildren;
extern Counter nrDataFileContainerChildren;

// ── DependencyTracker scope operations ───────────────────────────────
extern Counter nrDepTrackerScopes;
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

// ── Recovery dep recomputation breakdown ────────────────────────────
extern Counter nrRecoveryDepRecomputeUs;
extern Counter nrRecoveryDepRecomputeCount;

// ── SC/IS dep detail breakdown ──────────────────────────────────────
extern Counter nrDepHashScDirSetMisses;
extern Counter nrDepHashScJsonParseUs;

// ── Structural variant loop breakdown ───────────────────────────────
extern Counter nrStructVariantCandidates;
extern Counter nrStructVariantDepsResolved;
extern Counter nrStructVariantLoadKeySetUs;
extern Counter nrStructVariantHashUs;
extern Counter nrStructVariantDepResolveUs;

} // namespace nix::eval_trace
