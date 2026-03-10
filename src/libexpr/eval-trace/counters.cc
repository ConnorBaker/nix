#include "nix/expr/eval-trace/counters.hh"

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
Counter nrLoadTraceTimeUs;
Counter nrDbInitTimeUs;
Counter nrDbCloseTimeUs;

// ── Event counters ───────────────────────────────────────────────────
Counter nrRecoveryAttempts;
Counter nrRecoveryDirectHashHits;
Counter nrRecoveryStructVariantHits;
Counter nrRecords;
Counter nrLoadTraces;

// ── TracedExpr creation/forcing breakdown ────────────────────────────
Counter nrTracedExprCreated;
Counter nrTracedExprFromMaterialize;
Counter nrTracedExprFromOrigAttrs;
Counter nrTracedExprFromDataFile;
Counter nrTracedExprForced;
Counter nrLazyStateAllocated;

// ── Data-file node type breakdown ────────────────────────────────────
Counter nrDataFileScalarChildren;
Counter nrDataFileContainerChildren;

// ── DependencyTracker scope operations ───────────────────────────────
Counter nrDepTrackerScopes;
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

// ── Recovery dep recomputation breakdown ────────────────────────────
Counter nrRecoveryDepRecomputeUs;
Counter nrRecoveryDepRecomputeCount;

// ── SC/IS dep detail breakdown ──────────────────────────────────────
Counter nrDepHashScDirSetMisses;
Counter nrDepHashScJsonParseUs;

// ── Structural variant loop breakdown ───────────────────────────────
Counter nrStructVariantCandidates;
Counter nrStructVariantDepsResolved;
Counter nrStructVariantLoadKeySetUs;
Counter nrStructVariantHashUs;
Counter nrStructVariantDepResolveUs;

} // namespace nix::eval_trace
