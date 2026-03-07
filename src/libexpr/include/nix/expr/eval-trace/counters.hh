#pragma once
///@file

#include "nix/expr/counter.hh"

namespace nix::eval_trace {

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

} // namespace nix::eval_trace
