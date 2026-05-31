# Plan: cheaper producer-trace recording (the last gate for the partition direction)

Status: DRAFT (2026-05-31). Prereq: `derivation-producer-partition-rfc.md` + redesign-plan
follow-ups #12–#21. The partition direction's verify-benefit gate is SETTLED (M≈1, ~63% verify
win, #21); soundness is closed; the SOLE remaining gate is the producer-RECORD cost (§3b's
dominant net-loss term, ~1.5ms × ~thousands of producers/eval). This plan attacks that cost.

All mechanism claims cited to code; this draft will be adversarially refined before implementation.

## 1. Where the cost actually is (measured/code-grounded)

`recordCAProducer` → `TraceBackend::recordSync` (context.cc:398) → `store->record`
(`Recorder::record`, recorder.cc:25) per producer, under `withExclusiveAccess` (the store mutex).
`Recorder::record` does, synchronously, per producer:
1. **CPU**: `sortAndDedupDeps` + 3× hash (`computeTraceHash/FullTraceHash/DepKeySetHash`) +
   `serializeKeys` + `serializeValues` (steps 1–6; timed by `nrRecordHashUs` /
   `nrRecordSerialize*Us`).
2. **`storage_.flush(ea)` (step 7, recorder.cc:77)** — a full `vocab.checkpoint()` + a
   `SQLiteTxn` BEGIN…COMMIT over all pending entities (sqlite-trace-storage-lifecycle.cc:623).
   **This runs PER producer.** N producers ⇒ N checkpoints + N COMMITs.
3. **`publishRecord` (step 8)** — DB write of the Sessions/CurrentNode row + in-memory cache
   updates. NOTE the order: flush (7) runs BEFORE publish (8) because "IDs must exist before FK
   references" (recorder.cc:75) — `publishRecord`'s row FK-references the trace/result IDs.

The deep-attrset doc note ("recording is I/O-bound", redesign-plan:244) points at (2): the
per-producer flush (checkpoint + COMMIT) is the prime suspect for the dominant cost, NOT the CPU
hash/serialize (which the §3b timers can confirm/deny).

**Key feasibility fact (checked):** the `traceHash` that `recordCAProducer` needs synchronously
(to register `producerMap` so the consumer edge carries it) comes from `getCurrentTraceHash`, which
reads the IN-MEMORY caches (`currentNodeIndex`/`traceCache` via `lookupCurrentNode`/
`ensureTraceHeader`, sqlite-trace-storage.cc) populated by `publishRecord` — NOT from SQLite. So the
hash is available without the flush. The flush is purely the durability write.

## 2. Layered design (cheapest, lowest-risk first)

### Layer 1 — BATCH the flush (do NOT flush per producer)
The win: N producers currently pay N × (checkpoint + COMMIT). Batch to ONE flush (session end, or
every B producers). The trace/result entities already BUFFER in `pendingTraces` /
`getOrCreateTrace` (sqlite-trace-storage.cc:552) — only the per-call `flush` forces them out early.

The OBSTACLE (from the recorder.cc:75 ordering): `publishRecord` (step 8) writes a Sessions/
CurrentNode row that FK-references trace/result IDs, so today step 7 flushes those IDs first. If we
defer the flush, `publishRecord`'s row references unflushed IDs → FK violation at write time.
Resolution options (to be chosen in the adversarial pass):
- (1a) Defer publishRecord's DB row too — buffer the CurrentNode rows alongside pendingTraces, and
  write Sessions/CurrentNode in the SAME batched flush (after the trace/result IDs). The in-memory
  cache update (which getCurrentTraceHash needs) still happens immediately; only the DB write
  defers. Requires confirming publishRecord cleanly separates its in-memory update from its DB write.
- (1b) Make the per-producer path skip BOTH flush and the DB part of publishRecord (in-memory only),
  and add a session-end "flush all buffered producers + their CurrentNode rows" pass.
Either way: in-memory caches stay current (verify within the session works); SQLite durability is
deferred to one batched flush.

### Layer 2 — move CPU (hash + serialize) off the eval thread (ONLY if Layer 1 leaves CPU dominant)
If, after batching the flush, the §3b hash/serialize timers still dominate, move steps 1–6 to a
background thread: the eval thread captures the producer's deps + result and hands them to a queue;
a worker hashes/serializes/buffers; session end joins + flushes. The §3b note's "traceId
reconciliation deferred" caveat lives here: `recordCAProducer` needs the traceHash to register the
producerMap edge — if hashing is async, the edge can't be emitted synchronously. Mitigations:
compute ONLY the cheap traceHash synchronously (it's needed for the edge) and defer the
serialize/FullHash/DepKeySet to the worker; or emit the edge with a placeholder reconciled at flush.
Layer 2 is materially harder and is GATED on Layer 1's measurement showing CPU still dominant.

## 3. Soundness obligations (the deferred-flush must not break correctness)
- **Within-session verify**: a producer recorded earlier in the session must be verifiable by a
  later consumer in the SAME session. Since in-memory caches are updated immediately (Layer 1),
  this holds without the flush. MUST test.
- **Crash durability**: a deferred flush means a crash mid-eval loses buffered producers. Today's
  per-record flush makes each producer durable immediately. Is that a regression? For a CACHE it is
  acceptable (a lost producer = a cache miss next run, not a wrong answer) — but must be stated, and
  the session-end flush must be on the SUCCESS path (a failed eval shouldn't half-write).
- **Cross-session**: the batched flush at session end must produce byte-identical DB state to N
  per-record flushes (same rows, same FK graph). MUST verify byte-identical eval + a warm-hit test
  across the session boundary.
- **Atomicity**: one batched SQLiteTxn over all producers is MORE atomic than N txns, not less —
  a crash either has all or none. Fine. But the vocab.checkpoint ordering (vocab durable before
  traces, lifecycle.cc:623 comment) must be preserved in the batched path.

## 3a. ADVERSARIAL-PASS REFINEMENT (2026-05-31, measured — supersedes §1's flush-only framing)

Did the plan's own step 4.1 FIRST (read the §3b record timers) — and it redirects the plan.
python3Packages, 3 packages, `NIX_ENABLE_CA_PRODUCER=1` + `NIX_SHOW_STATS`: 2,117 producers,
`record.timeUs` = 1.537s ⇒ **~726 µs/producer** (NOT the ~1.5ms cited from closures.gnome; that
was a different workload). Component split of `record.timeUs`:

| component | % | batchable? |
|---|---:|---|
| flushUs (checkpoint + txn draining pending*) | 43.8% | YES (Layer 1) |
| hashUs (3× trace hashes) | 26.5% | only Layer 2 (off-thread CPU) |
| serializeKeys+Values | 5.4% | only Layer 2 |
| **unaccounted ~24%** | ~24% | **mostly a SECOND per-producer txn (see below) → YES** |

**Two findings that reshape Layer 1:**
1. **There are TWO per-producer SQLite transactions, not one.** Besides `flush()` (step 7),
   `publishRecord` → `publishStateChange` (sqlite-trace-storage.cc:694-703) opens its OWN
   `SQLiteTxn` (upsertAttr + insertHistory) PER producer. The ~24% unaccounted is largely this.
   So Layer 1 must batch BOTH txns, not just `flush`. Good news: `doInternResult`/
   `getOrCreateDepKeySet`/`getOrCreateTrace` ALL already buffer (`pending*` vectors, no per-call
   write); `publishRecord` updates in-memory caches (traceCache/depKeySetCache) + only
   `publishStateChange` writes SQLite. So the DB writes are concentrated in exactly two txns,
   both deferrable; the in-memory state (which within-session verify + getCurrentTraceHash need)
   is already synchronous.
2. **Layer 1 alone caps at ~44% + ~24% = ~68%** (the two txns), leaving hash 26.5% + serialize
   5.4% ≈ 32% CPU that only Layer 2 addresses. So Layer 1 is a MAJORITY win but not the whole
   thing; whether ~68% reduction of the ~726µs/producer (→ ~230µs/producer) flips the net
   depends on the end-to-end combination (needs the full recorder). Layer 2 (off-thread hash) is
   likely needed for a decisive win, but Layer 1 is the cheap first cut and de-risks the rest.

REVISED Layer 1: defer BOTH `flush()` AND `publishStateChange`'s DB txn for producer traces;
keep all in-memory cache updates synchronous; one batched flush + batched CurrentNode/History
write at session end (success path). The FK-ordering obstacle (§2) still applies — the batched
end-flush writes pending entities (trace/result IDs) BEFORE the batched CurrentNode rows.

## 4. Sequence
1. Confirm via the §3b timers which of {flush, hash, serialize} dominates per producer (cheap: the
   benefit-probe-style counters already exist — `nrRecordFlushUs` vs `nrRecordHashUs` vs
   `nrRecordSerialize*Us`). This decides whether Layer 1 alone suffices or Layer 2 is needed.
2. Implement Layer 1 (batch flush), env-gated, default-off. Measure on python3Packages
   (derivation-dense) the recordSync cost delta.
3. Soundness gate: byte-identical eval + within-session + cross-session warm-hit tests.
4. ONLY if CPU still dominant: Layer 2.

## 5. What this plan does NOT claim
- That the cost is the flush — §1 makes it the prime suspect from the I/O-bound note, but step 4.1
  (read the timers) decides it. If hash/serialize dominate, Layer 1 is insufficient and Layer 2 is
  required (harder).
- That deferred flush is free of durability change — §3 states the crash-loses-buffered-producers
  regression explicitly (acceptable for a cache, but real).
- A net-win number — that needs Layer 1 built + the full aggressive shape (still unbuilt) to
  measure end-to-end. This plan reduces the RECORD cost; the end-to-end net still needs the
  recorder + the §21-confirmed verify win combined.
