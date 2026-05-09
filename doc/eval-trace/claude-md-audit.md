# CLAUDE.md Adversarial Audit

This document tracks adversarial review of the eval-trace CLAUDE.md corpus
and adjacent design/plan/research documents:

- `src/libexpr/eval-trace/CLAUDE.md`
- `src/libexpr-tests/eval-trace/CLAUDE.md`
- `src/libexpr-tests/eval-trace/property/CLAUDE.md`
- `tests/functional/CLAUDE.md`
- `doc/eval-trace/design.md`, `doc/eval-trace/implementation.md`
- `plans/*.md` (concatlists, property-testing framework/impl, lean proof,
  compare-runs analytics)
- `research/*.md` (eval-environment refactor, order-invariant persistence,
  SQLite-centered / foundation)

Per project convention, no `file:line` references — drift kills them.
Every citation is a file + symbol.

What this document tracks:

- §1 — open design work (OR-N entries still needing decisions).
- §2 — a flaky functional test unrelated to eval-trace, recorded so it
  isn't re-chased.
- §3 — structural-variant recovery optimisation landscape: current
  measured state, landed optimisations, active proposals, and retired
  proposals with their rationale.

Baseline framing (reconciled with current code):

- `CanonicalHashBuilder` is the structured multi-field hash path used by
  trace/struct/result hashes, session/recovery keys, directory listings,
  NixBinding hashes, runtime-root source keys, and policy digests.
  Aggregated DirSet hashes remain a legacy raw active-backend digest over
  NUL-delimited fields.  Single-payload dep values (file bytes, NAR
  dumps, env/current-system strings, JSON/TOML scalar canonical strings,
  sentinels) are still raw active-backend digests.
- `eval-trace-hash-algorithm = blake3|sha256` selects the active backend.
  Persistent cache identity embeds both the schema epoch and the backend
  slug in the DB filename: `eval-trace-v<kSchemaEpoch>-<algorithm>.sqlite`.
  `kSchemaEpoch` is declared in `trace-store.hh`; it is the source of
  truth for the version number and is bumped on every schema-breaking
  change.  Historical benchmark prose in §3 that says "BLAKE3" describes
  the default backend used in those measurements; current-code
  descriptions should be read as "the active eval-trace hash backend"
  unless the backend is explicitly part of the measurement.
- The old `generate-runs` / `compare-runs` apps have been replaced by
  `eval-trace-bench` subcommands (`generate`, `runs`, `pairwise`,
  `series`, `classify`, `sv`, `logs`, `simulate`, `db-inspect`,
  `export`).
- The old Bundle-4 protocol shell — `TraceStoreService`,
  `trace-store-protocol.hh`, `verify-pipeline.hh` — has been removed.
  `verification-protocol.hh` (shared `VerifyOutcome` enum + `RecoveryState`
  stage tags) and `VerificationOrchestrator` (owns `PrefetchPool`, drives
  `verifyAttrImpl`) remain.
- The process-global `TraceRuntime::activeRegistry_` and `activeBackend_`
  side channels are gone.  Cold-eval mutation is session-scoped via
  `TraceSession::registerRuntimeRootMount` (adds mount points to the
  per-session `SemanticRegistry`) and `backend->recordRuntimeRoot`
  (delegates to `TraceStore::recordRuntimeRootExclusive`).  Fully
  eliminating the two-call publication in favour of a single
  `publishRuntimeRoot` returning a replacement `EvalSessionHandle` is
  still open.
- Subsumption (`session.isFileVerified`) is `TraceId`-scoped, not
  session-wide.  `OriginDep` carries `std::optional<TraceId>`; the
  `canSubsumeShortcut` gate in `dep-resolution-service.cc` requires both
  `QueryBehavior ∈ {Structural, ImplicitStructural}` and
  `Origin == CurrentTrace` (via `if constexpr` + SFINAE on
  `grantVerifiedSubsumption<O>`).

Several DEF-N framings in the tests CLAUDE.md carry corrections
inline: DEF-1's "implementation-dependent miss" was actually two HIT
scenarios plus one miss; DEF-2's "libfetchers not linked" framing was
wrong (it is transitively linked via `nix-expr.pc`'s public
`Requires:`); DEF-8's "resolved" artifact names a counter and file
absent from the current tree.  Consult
`src/libexpr-tests/eval-trace/CLAUDE.md` §D for the authoritative
per-DEF status.

---

## 1. Open design work

These items require design decisions or architectural RFCs, not
mechanical edits. Each has a full entry in
`src/libexpr/eval-trace/CLAUDE.md` under "Open research"; this list
is a pointer, not a duplication.

**Relationships (updated 2026-04-30).**

- **OR-1, OR-3, OR-4 all shielded by the same FileBytes backstop
  in practice.** `ExprParseFile::eval` (eval.cc:1422-1435) calls
  `maybeRecordImportedFileContent` whenever `traceActiveDepth > 0`
  and a TraceSession is bound, unconditionally recording
  `FileBytes(source_file)` into the active trace. Edits to a `.nix`
  file invalidate that FileBytes → root's verify fails → fresh
  eval. This backstop makes OR-1 and OR-4 not demonstrably
  reachable in normal evaluation — the storage-layer gaps they
  pin are real but shielded. OR-3's cache-side concern was
  reframed 2026-04-30 as "per-leaf lazy verification is correct";
  only the benchmark stale-serve (different shape, unreproduced)
  remains open.
- **OR-7 is BUG-8.**  Same bug, two names.  OR-7 tracks it under
  Open Research; BUG-8 is the label in the `recording.cc` comment
  and the BUG-N Index.

**Recently closed.**

- **OR-2 (bare-import FileBytes backstop) closed.**  Landed as the
  minimal `else`-removal at `ExprParseFile::eval` (in `eval.cc`) and
  `scopedImport` (in `primops.cc`): the FileBytes-emission helper
  `maybeRecordImportedFileContent` now fires for every traced import,
  including NixBinding-eligible attrset bodies.  `registerNixBindings`
  continues to populate the per-binding SP precision layer on top;
  when an accessed binding emits SP, the existing pass-2 structural
  override subsumes the FileBytes on warm verify.  The originally
  recommended "Option B" (eager `ImplicitStructure(Nix, Keys)` at
  parse time) was deliberately NOT taken — it would over-invalidate
  `aliases.nix`-style additive key edits that are currently served
  via per-binding SP override for consumers that do access specific
  bindings; a precision-preserving extension is left as a separate,
  consumption-site follow-up (emit the Keys dep from
  `prim_attrNames` / formals destructuring, not from the import
  site).  Pins: `oracle-nested.cc::BareImport_{ContentChange,
  ValueChange,UnrelatedFileUnchanged}`,
  `verify/integration.cc::MapAttrs_ValueChange_CacheMiss`, and
  Tests 6-8 of `eval-trace-impure-soundness.sh`.  See "Bare-import
  FileBytes backstop (was OR-2)" in the production CLAUDE.md
  Closure notes for the full landed-fix rationale.  OR-2 was
  independent of OR-1/OR-3/OR-4 despite all four touching the
  dep-recording model.

**Still open.**

- **OR-3 reframed (2026-04-30).** The prior framing treated
  "source-copy deps land on child thunks" as a soundness bug
  requiring child-to-parent dep propagation. Empirical check
  (enabling the historically-DISABLED
  `ParentChild_ChildInheritsStaleStorePath` test) showed the
  failure was in the test's assertion, not the cache's behavior.
  The cache operates per-leaf: each leaf's trace records its own
  deps; forcing a specific leaf verifies against its own deps and
  re-evaluates or serves cached as appropriate; siblings with
  unaffected deps continue to serve cached (precision preserved).
  `forceRoot`'s `loaderCalls==0` after a source change is the
  correct behavior — root's trace has no source-tree deps, so
  root's verify trivially passes; child re-evaluation happens
  lazily when children are forced.

  The previously-enumerated design options (A: hoist to
  `scopeStack.front()`; B: new `EnclosedCopy` CQK on parent;
  C: eager-force; D: split DSP) are all retired under the
  project's hard constraint on zero precision/performance loss —
  each would force root invalidation to cascade to all siblings
  via ParentSlot, breaking selective sibling invalidation, or
  would defeat Nix's laziness. The cache-side OR-3 concern is
  closed.

  The test was reformulated as
  `ParentChild_PerLeafLazyVerification` and enabled; the sibling
  `SharedThunk_Siblings_TraceValueContext_InvalidatesBoth`
  (formerly `DISABLED_BenchmarkRepro_SecondChildServesStaleValue`)
  was also enabled as a regression guard for the
  `SiblingReplayCaptureScope::maybeCapture` → `TraceValueContext`
  path.

  **Remaining OR-3 concern (open).** `closures.gnome.x86_64-linux`
  at `nixpkgs@6b74cf77bde9` genuinely stale-serves under
  subprocess-per-commit eval — this is a real soundness failure
  but its shape is distinct from what the historically-DISABLED
  in-process tests reproduced. The original DISABLED-test comment
  hinted at an asymmetric shape ("one sibling reads files
  directly, one reads through a memoized thunk"), but no
  in-process reproducer exists. Unblocking requires
  subprocess-per-commit dep-graph diff to identify which specific
  leaf is missing which specific dep. Fix (if one is needed) will
  be at the recording site for that shape — not at the
  scope-attribution layer.

  *Warm-hit-no-epochMap asymmetry (probed 2026-04-30, ruled out).*
  An adversarial code trace found an asymmetry between cold-record
  and warm-hit paths: cold `forceThunkValue` populates `epochMap`
  for the thunk's Value slot, warm-hit through `TracedExpr::eval`
  → `materializeResult` does not. Downstream,
  `replayMemoizedDeps` returns early when `replayBloom.test(&v)`
  is false, skipping both direct replay and
  `SiblingReplayCaptureScope::maybeCapture`. Investigation
  (four probe tests in `dep/source-tree-soundness.cc`) confirmed
  the mechanism-level asymmetry but showed it is benign under the
  current architecture: cold re-eval traverses `realRoot`'s fresh
  thunks via `navigateToReal`, never the materialized tree.
  Documented at `memo-replay-store.hh::recordThunkDeps` and
  `trace-session.cc`'s warm-hit branch as a regression guard. The
  asymmetry is NOT the benchmark root cause.

- **OR-4 storage-layer gap, reachability not demonstrated
  (re-audited 2026-04-30).**  `TraceParentSlot` encodes the parent's
  INPUT fingerprint, not its output shape. The synthetic test
  `Integration_ParentSlot_DoesNotCaptureKeySetRemoval` pins the
  storage-layer property by using raw `db->record()` calls to
  construct the "parent has no deps, child has ParentSlot pointing
  at parent" shape, then shows `verifyTrace(childB)` returns true
  even after parent is re-recorded without childB.

  Reachability from real evaluation is not demonstrated. The
  `ExprParseFile::eval` backstop (eval.cc:1422-1435) unconditionally
  records `FileBytes(source)` whenever `traceActiveDepth > 0`, so
  edits to a `-f foo.nix` source fail root's FileBytes verify and
  trigger fresh eval. The new session's parent trace reflects the
  new key set; `findAlongAttrPath` on a removed key raises
  `AttrPathNotFound` rather than resolving through a stale
  CurrentNode row.

  Earlier audit (2026-04-30 first pass) claimed OR-4 was reachable
  in non-git `-f foo.nix`. That claim was wrong: it missed
  `maybeRecordImportedFileContent`'s backstop. Without a
  demonstrated in-process reproducer, there's no motivation to
  land the originally-proposed Option B (verifier-side key-set
  check) — the storage-layer gap is real but shielded by the
  evaluation pipeline.

- **OR-7 epoch-log truncation on exception (= BUG-8).**  Performance-
  only gap.  The option-(a) sketch (give `DepCaptureScope` an
  `EvalState &` and mirror `PublicationWarmupScope`'s
  `rollbackReplayEpoch(epochStart)` + `epochLog.truncate(epochStart)`
  call from `eval.cc`) is plausible.  No benchmark currently motivates
  landing; the fixtures in `benchmarks/eval-trace/` don't exercise
  exceptions.

---

## 2. Known flaky functional test — `main / build` (non-blocking)

Recorded after a non-deterministic failure on a commit that only touched
eval-trace code. Not an eval-trace issue. Documented here so a future
reviewer who sees this test fail on an unrelated commit doesn't go
chasing eval-trace phantoms.

**Symptom.** `nix-functional-tests:main / build` occasionally fails
with `exit status 1` under `nix build --builders "" -L .`. Observed
once on commit `6ced7ecd0` (a docstring-only commit); a rebuild of
the same derivation passed. Independent retries pass reliably.

**Root cause.** `tests/functional/build.sh` contains a regression
guard for "cancelled builds should not be reported as failures" (the
last ~60 lines of the script, guarded by `isDaemonNewer "2.34pre"`
and `canUseSandbox`). The fixture at
`tests/functional/cancelled-builds/flake.nix` uses an on-disk FIFO to
synchronize two concurrent builds running under `nix build -j2`:

- `slow` writes `"started"` to the FIFO, sleeps 10s, then `touch $out`.
- `fast-fail` reads from the FIFO (waiting for `slow` to signal),
  then `exit 1`.
- `depends-on-slow` / `depends-on-fail` are downstream of the above
  and should be cancelled when their parent fails, not reported as
  failures.

The test asserts that only `fast-fail` appears in the error output
and that the cancelled goals do NOT. Under CPU load, scheduler
variance, or if `nix build` happens to serialize the two goals
instead of running them concurrently, the FIFO read can deadlock
or stderr ordering can break the `grepQuietInverse` assertions.

**Why this is NOT eval-trace.** The fixture never calls `nix eval`
or uses the eval-trace subsystem. The failure mode lives in libstore
goal scheduling / cancellation reporting. Commits to eval-trace
cannot affect this test path.

**Upstream context.** The regression guard itself was added by
commit `68f549def` ("buildPathsWithResults: don't report cancelled
goals as failures"), landed well before the eval-trace rewrite.
The FIFO synchronization is an attempt at determinism but is not
airtight under load.

**Triage recipe.** If a commit under review shows `main / build`
failing:

1. Retry the build once or twice. If it passes on retry, the failure
   is the known flake and nothing else need be done.
2. If it persistently fails across 3+ retries, check whether the
   commit touched `libstore/build/*`, `libstore/goal*`, or the
   daemon's goal-scheduling paths. If not, it's still likely flake-
   related but deserves a deeper look.
3. To reproduce locally: `nix build --builders "" -L .` and inspect
   the meson testlog at the derivation's build dir
   (`/build/source/tests/functional/build/meson-logs/testlog.txt`).

**Durable fix direction.** A robust rewrite of this test would
replace the FIFO-plus-sleep scheme with a polled file-existence
sentinel and explicit per-goal `--timeout` bounds, plus an
accept-either-outcome matcher on the stderr assertions. Out of
scope for the eval-trace work this audit tracks; filed here so it
doesn't get lost.

---

## 3. Structural-variant recovery optimisation landscape

Landscape document for SV optimisation work. Contains the current
measured baseline, landed optimisations, live proposals with their
measured ceilings and remaining uncertainty, and retired proposals
with their rationale so they aren't re-surveyed.

### 3.1 Current measured baseline

Benchmark: 500 consecutive nixpkgs commits (newest→oldest) evaluating
`closures.gnome.x86_64-linux`, run triple `reference,cold,hot`. Cold
run starts from an empty cache; each commit sees the state left by
the previous commit. This is a strictly monotonic workload — no
commit reverts any earlier state.

Current post-`is_avalanching`-fix numbers:

- **Cold wall 3,635 s** vs reference 5,241 s. Cold is 0.69× reference
  on the mean — the cache saves evaluation cost overall.
- All outputs match reference byte-for-byte (soundness PASS).
- Pre-fix baseline was 7,337 s cold. The `is_avalanching` fix halved
  cold wall.

Commit-class breakdown (classes are computed from the partition
321 hit-only + 64 with 2 recovery failures + 115 with 4 recovery
failures; the "partial" and "full" rows overlap):

| Class | Count | Mean wall | Mean ref wall | Cache ratio |
|---|---|---|---|---|
| Hit-only (all recovery succeeded) | 321 (64%) | 2.02 s | 10.48 s | **5.2× speedup** |
| Partial miss (1-3 leaf attrs re-eval) | 170 (34%) | 16.60 s | 10.45 s | 1.6× slowdown |
| Full miss (all 4 leaf attrs re-eval) | 115 (23%) | 19.34 s | 10.49 s | 1.8× slowdown |

The partial-miss class shows the per-attr penalty cleanly: `nrThunks`
on 2-fail commits is ~10.6 M (half the reference's 20.9 M), and on
4-fail commits is ~20.9 M (= reference). Each failed leaf attr adds
~3.8 s of cold wall.

**Worst full-miss commit decomposition** (cold 17.4 s, reference
10.8 s — 6.6 s overhead):

| Component | Cost | Scales with DB size? |
|---|---|---|
| Eval-side tracing (dep-recording during re-eval) | ~4.0 s | No (flat across the run) |
| `verify()` wasted on miss attrs | ~1.0 s | No (flat) |
| `loadTrace` (values_blob SQLite read + deserialise) | 0.12 → 1.71 s | **Yes (14×)** |
| SV dep-resolve (scan candidates) | 0.19 → 2.93 s | **Yes (15×)** |
| `record()` of new traces | ~0.1 s | No (flat) |

**Two distinct residual costs, only one scales with history:**

1. **Eval-side tracing overhead (flat ~4 s).** Running the evaluator
   with `TracedExpr` / `DepRecordingContext` active adds ~37% to
   reference eval time regardless of cache state. Sources: per-primop
   dep-record calls, epoch log maintenance (1.38 M replay calls on
   the worst commit, 97 K `ownDepsMax` per scope), replay-time bloom
   filter lookups (1.34 M). This is the cost of "evaluator
   instrumented for tracing", not a cache-design question.

2. **Trace-machinery overhead (growth ~3×).** `verify` + `recovery` +
   `loadTrace` + `record` together scale with candidate-set size,
   which scales with DB population.

Per-dep and per-load costs are flat across the run:

- Per-dep SV resolve cost: ~170 ns/resolve (161 ns at bucket 200-249,
  172 ns at 450-499).
- Per-load cost: ~3,000 µs/load (2,786 µs at bucket 0-49, 3,024 µs
  at 450-499).

The `is_avalanching` fix removed the pathological per-lookup
clustering; what remains is the **linear growth in how many
lookups/loads we do per commit** as the DB population grows. The
residual degradation is not a bug but a direct consequence of SV's
scan-all-candidates architecture.

### 3.2 SV internals: how the loop actually works

Relevant for reading every proposal below.

**Structure.** `tryStructuralVariantRecovery` (in
`trace-store-verify.cc`) scans `structGroups`, a `{DepKeySetId →
StructuralVariantGroup}` `boost::unordered_flat_map`. Representatives are
selected while reading `scanHistoryForAttr`'s DESC-trace_id rows, but bucket
iteration order is unordered. For each bucket, it:

1. `loadKeySet(ea, depKeySetId)` — reads/deserializes only the key set.
   If `eval-trace-structural-recovery-mismatch-telemetry` is enabled, this
   deliberately falls back to `loadFullTrace(ea, repTraceId)` so the loop can
   compare current hashes against stored historical values.
2. For each dep key in the representative key set, calls `resolveCurrentDepHash`
   with a `CandidateDep` origin tag. Accumulates the freshly-resolved
   `(key, current_hash)` pairs into `repDeps`. Breaks on
   Volatile / TraceContextMiss / ResolveFailed.
3. If the loop completes: computes `computePresortedTraceHash(repDeps)`
   (sorted hash over `(key, value)` pairs, via the active eval-trace backend)
   and calls `lookupCandidate` against the bucket's history.
4. If `lookupCandidate` finds a match and `acceptRecoveredTrace`
   accepts it, SV wins.

**Load semantics — what the loop actually reads.** Tracing the inner
loop (both branches, Volatile-kind check and the general
`resolveCurrentDepHash` branch): the recovery decision reads `dep.key`
for each dep (to call `resolveCurrentDepHash`,
`resolveTraceContextHash`, `scope.tag`, and
`StorePathBatch::collect`). The winning-candidate path computes
`computePresortedTraceHash(repDeps)` from `repDeps` -- freshly built
from `(key, current_hash)` pairs resolved in the loop -- not from the
preloaded values. Full value decode is therefore not part of candidate
acceptance. It is now opt-in diagnostic work, used only when
`eval-trace-structural-recovery-mismatch-telemetry` is enabled to populate
the `firstHashMismatchIdx` measurement counters.

**`abortedEarly` semantics.** The counter in `SVCandidateStats`
(declared in `counters.hh`, updated via `recordSVCandidate` in
`counters.cc`) flips true when the loop breaks on Volatile /
TraceContextMiss / ResolveFailed. Paired with `abortedEarly`, the
counter `depsResolvedSum` accumulates `repDeps.size()` at the break
point — this is the number of deps **successfully resolved BEFORE**
the break, not the break index. Given that the push onto `repDeps`
happens only on the success path, `depsResolvedSum` equals the zero-based
failure index for each aborted candidate. Stated plainly: the count is not
"how deep did we get" in one-based terms, it's "how many deps did we already process".
Any reasoning over SV abort depth must go through `depsResolvedSum`,
not a mental model of "index at which we broke".

### 3.3 Landed optimisations (with measured impact)

**Drop `is_avalanching` from misused hashers.** Several hashers
across eval-trace declared `using is_avalanching = void` despite
computing their output via `hashValues(...)` (`hash_combine` over
`std::hash<size_t>`). On libstdc++ `std::hash<size_t>` is identity;
`hash_combine` over identity inputs does not avalanche — output
bits remain strongly correlated with input bit patterns. The
marker tells `boost::unordered_flat_map` to skip its post-mixer,
exposing the raw hash to the bucket index. For multi-field keys
with shared prefixes (the L1 cache holds ~40K `Dep::Key` entries
sharing 5 of 8 fields), the result was pathologically long probe
sequences — every lookup became O(probe-walk) rather than O(1).

Markers dropped from:
- `Dep::Key::Hash` (`types.hh`) — the L1 cache key; dominant
  contributor
- `DataPathNodeKey::Hash` (`interning-pools.hh`) — structural dep
  path trie; many entries share a `parentId`
- `FileCacheKey::Hash` (`parse-caches.hh`)
- Three `*DepKeyId::Hash` wrappers (`ids.hh`) that delegate to
  `DepKeyId::Hash` (identity on libstdc++)
- `TraceSessionReuseSlotKey::Hash` (`session-types.hh`)

Measurement (100-commit `nixpkgs-release` benchmark vs baseline):
- Cold wall: 969 s → 822 s (−15.2%, −148 s).
- SV depResolve: 97 s → 4.5 s (−95.4%).
- Recovery.SV total: 120 s → 18 s (−84.9%).
- Soundness: PASS.

Microbenchmark confirmation (L1 lookup only):
- Pre-fix: `verifyTrace` dispatch 515.7 ns/op, direct
  `lookupDepHash` 557.2 ns/op.
- Post-fix: `verifyTrace` dispatch 37.7 ns/op, direct
  `lookupDepHash` 6.8 ns/op.

**Cross-cutting hazard retained in `Tagged<T, uint32_t>::Hash`.**
`libutil/tagged.hh` still declares `is_avalanching = void` and
returns `std::hash<T>(x.value)`. Every `Tagged<_, uint32_t>`
(AttrPathId, TraceId, DepKeySetId, etc.) inherits this false claim.
Most maps keyed on a single monotonic ID are unaffected — identity
hash distributes `1,2,3,...,N` uniformly. The hazard is new
multi-field hashers that combine multiple `Tagged` IDs via
`hashValues` and claim avalanching by copy-pasting the marker.
Audit new hashers for this pattern on sight.

**SV candidate telemetry + debug-gated diagnostics.** Landed in
commit `6d8eb9a22`. Two surfaces:

*Per-DepKeySetId telemetry* (`NIX_SHOW_STATS` →
`evalTrace.structVariant.byDepKeySet`, outcome fields always on when
`NIX_SHOW_STATS=1`):

- Per-bucket: `{depKeySetId, tried, succeeded, abortedEarly,
  hashMismatch, avgDeps, avgUs}`. Absent when SV never fired.
- `avgDeps = depsResolvedSum / tried` — see §3.2 for semantics.
- `succeeded / tried` per bucket — bucket success distribution.
- The any-dep early-exit savings fields require
  `eval-trace-structural-recovery-mismatch-telemetry`; otherwise they remain
  zero because SV uses key-only preloads.

*Debug-gated logs* (`--debug` / `NIX_LOG_LEVEL=debug`, grep
`eval-trace/`):

- `eval-trace/recovery: SV candidate depKeySet=X traceId=Y abort
  reason=... kind=... key='...'` — per-candidate abort forensics.
- `eval-trace/recovery: SV candidate depKeySet=X traceId=Y
  hash-mismatch: candidate=... (N deps resolved)`.
- `eval-trace/recovery: DirectHash miss: target=... no trace in
  History matched`, preceded by `recovered N/M dep hashes for ...`.
- `eval-trace/verify: pass2 outcome=... failedContentFiles=N
  structCoverage=K implicitCoverage=M uncovered=P`.
- `eval-trace/verify: file verified kind=... key='...'`.

**SV any-dep early-exit gating instrumentation.** `SVCandidateStats`
in `counters.hh` was extended with five fields (populated by
`recordSVCandidate` from the SV loop in `trace-store-verify.cc`):

- `bothSetCount` — candidates where BOTH a hash mismatch AND a
  resolveFail index were observed.
- `earlierHashMismatchCount` — subset where the first hash mismatch
  strictly preceded the first resolveFail.
- `earlierHashMismatchSavedDeps` — Σ `(firstResolveFailIdx −
  firstHashMismatchIdx)` over those candidates. The raw savings
  signal for the any-dep early-exit proposal.
- `hashMismatchOnlyCount` — candidates that iterated fully (no
  resolveFail) with at least one hash mismatch.
- `hashMismatchOnlySavedDeps` — Σ `(totalDeps − firstHashMismatchIdx)`
  over those.

The loop itself is unchanged — when the telemetry setting is enabled it scans
both axes without breaking on mismatch. This is measurement only: taking the
early-exit now would confound the signal (the loop shape determines what we can
measure). A debug log line at each outcome site emits the indices for external
analysis, also only when the telemetry setting is enabled:

```
eval-trace/recovery: SV early-exit signal depKeySet=X traceId=Y
    firstHashMismatch=<N|none> firstResolveFail=<M|none>
    totalDeps=K outcome=<abortedEarly|hashMismatch|win>
```

Decision criterion from a future benchmark run: sum
`earlierHashMismatchSavedDeps` across all buckets, multiply by
measured per-dep-resolve wall (~170 ns post-avalanching), compare
to cold wall. Proposal is worth landing if the delta is
significantly above zero; not worth it if ≈0.

### 3.4 Active proposals

Three proposals remain live after adversarial review. Each is
characterised by its attack surface (what fraction of cold wall
it targets), its mechanical cost (implementation shape and risk),
and what experiment would unblock it.

**Confidence note.** Ceiling estimates attached to individual
proposals below are model-based reasoning, not measurements.
Previous iterations of this document attached specific percentages
(20%, 27%, 30%) that proved wrong by 2–5× when measured. Treat each
"~X% of cold" figure as "between 0 and the slice it attacks," not
as a committed target. A proposal's strength comes from (a) its
attack surface being clearly measured and (b) its implementation
cost being bounded — not from a specific ceiling number.

#### Proposal 1 — SV any-dep early-exit (measurement-landed, waiting on data)

**Shape.** Inside the SV candidate loop, break as soon as a resolved
current-state hash disagrees with `dep.hash`. For singleton
`DepKeySetId` buckets (98.3% of the distribution) any single-dep
mismatch guarantees the final `computePresortedTraceHash(repDeps)`
will not match, so the candidate can be rejected without iterating
the remaining ~32K deps. For multi-trace buckets (1.7%) pre-load all
sibling traces' stored hashes and break only when current matches
none of them.

**Attack surface.** Abort-early candidates are 89.3% of SV tries
(55,087 of 61,705 across 500 commits). `abortedEarly` reasons are
100% `resolveFailed, kind=structuredProjection` across all 500
commits of debug logs. The failing dep is overwhelmingly (>90% of
sampled lines) a `#len` or `#keys` shape-suffix query on a TOML/JSON
path whose current DOM kind mismatches the query's expected kind.

**What we don't know.** Whether the first `current != dep.hash`
mismatch typically occurs EARLIER than the first resolveFail, or at
the SAME dep. Two regimes:

- Hash mismatch significantly earlier than resolveFail → savings
  could be large.
- Hash mismatch at the same dep as resolveFail → savings ≈ 0 (the
  existing break already fires there).

The two are both consequences of "this file changed vs recording",
so they may correlate closely. The gating instrumentation (§3.3)
answers this. The decision criterion is above; no prior guess
substitutes for the measurement.

**Correctness.** Sound for singleton buckets because
`computePresortedTraceHash` is a sorted hash over `(key, value)`
pairs — under the active backend's collision resistance, any single value
mismatch changes the final hash.

**Next step.** Run one full 500-commit `nixpkgs-release` benchmark
with the landed instrumentation, sum
`earlierHashMismatchSavedDeps` across all buckets, compute expected
wall-time savings. If the signal is material, ship the early-exit
behind a flag and bisect against baseline.

#### Proposal 2 — Keys-only load in SV preload (landed, telemetry opt-in)

**Shape.** SV preload now uses `loadKeySet(ea, depKeySetId)` by default and
iterates `Dep::Key` entries, constructing transient `CandidateDep` values only
at the point of `resolveCurrentDepHash`. This is stronger than
`loadTraceKeysAndHeader`: it avoids the Traces join and does not stash
`values_blob` at all.

**Telemetry escape hatch.** Candidate acceptance still does not use historical
`dep.hash`; `computePresortedTraceHash(repDeps)` operates on freshly-resolved
hashes in `repDeps`. The only consumer of stored candidate values was
`firstHashMismatchIdx`. That diagnostic path is now gated by
`eval-trace-structural-recovery-mismatch-telemetry`, which deliberately uses
`loadFullTrace(ea, repTraceId)` and emits the any-dep early-exit fields/logs.
Default recovery avoids full value decode.

**Blob format** (`trace-serialize.cc::serializeValues` /
`deserializeValues` and `serializeKeys`): values are zstd-compressed
in a columnar single-envelope format (magic `vals2`, hash-algorithm
tag, entry-type bitmap, contiguous 32-byte eval-trace digest block,
length-prefixed strings). Zero-dep traces are the exception: they store an
empty values blob, so backend isolation comes from DB filename/session identity
rather than an in-blob tag. You can't skip a value mid-stream. Keys are in a
separate blob on the DepKeySets table. So "lazy decode" practically means
either "don't zstd+parse the values blob at all" if stored hashes are no longer
needed, or "parse only the digest block and pair it with keys" if the mismatch
telemetry stays. `loadTraceKeysAndHeader` implements only the first variant
today via `deferredTraceBlobs`.

**I/O vs CPU split.** `nrLoadTraces` / `nrLoadTraceTimeUs` wrap DB
read + zstd decompress + deserialize + zip. 3 ms / 650 KB ≈ 4.6
ns/byte. SSD at 2 GB/s → ~0.33 ms I/O for 650 KB. So I/O is ~10% of
load time, CPU is ~90%. Lazy decode attacks the right slice.

**Attack surface.** Upper bound is `nrStructVariantLoadKeySetUs` =
280.8 s across 500 commits on the pre-fix baseline — the entire SV
preload wall. That's ~3.8% of pre-fix cold wall or ~8% of post-fix
cold wall (3,635 s). Lower bound after subtracting I/O floor and
DB-round-trip overhead: **2–40 s across 500 commits (~0.1–1% of
post-fix cold).** Wide band because the split between zstd+deserialize
CPU and DB overhead is not directly measured.

**Next step.** Re-run the 100-commit cold benchmark with default telemetry-off
SV and compare `depHash.structVariantLoadKeySetUs` / cold wall against the
previous blake3 run. Use the telemetry setting only for targeted diagnostics.

**Priority.** Landed. Remaining work is measurement.

#### Proposal 3 — Inverted dep-key → trace-id index (architectural)

**Shape.** Today SV iterates every historical bucket for the current
attr path and resolves ~32,500 deps per candidate. Inverted: maintain
an index `Dep::Key → [TraceId]`. Changed-dep intersection would load
only candidates that depend on at least one changed key.

**Why this is the unresolved high-ceiling target.** The 89%
`resolveFailed` abort slice is not meaningfully attacked by any other
live proposal. Proposal 1 eliminates tail iteration; Proposal 2
eliminates values decode. Neither prevents the load itself. The
residual degradation in §3.1 is dominated by miss-path SV loading
O(trace count) candidates per commit — each candidate paying 3 ms of
loadTrace + 5 ms of 32K-dep iteration. An index that prunes the
candidate set before load attacks that structure directly.

**Concrete ceiling.** Bounded above by the full SV wall (~44% of
pre-fix cold; post-fix, SV is a smaller absolute wall but a larger
fraction of residual overhead). Bounded below by index-maintenance
overhead. Actual savings unknowable without implementation.

**The blocker.** Storage layout. 705 records × ~32,500 deps each ≈
23 M (not 90 M — per-leaf-attr deps overlap heavily across leaves
within the same trace). At SQLite row overhead ~50 bytes, ~1.1 GB
naively. Needs a compact representation — bitmap per key, or
`DepKeySetId` grouping (exploit that 98.3% of `DepKeySetId`s are
singletons: index at the `DepKeySetId` level rather than the
`Dep::Key` level reduces the row count by ~the mean deps-per-key-set
factor).

**DirectHash overlap.** Early drafts assumed DirectHash already
catches the wins OPP-3 targets. It does not: DirectHash computes
`hash(current_trace.oldDeps)` against history, which misses
candidates whose deps happen to all match but whose structure
differs from the current trace's. The index hits both the abort
slice and the hash-mismatch slice.

**Mechanical cost.** High. Schema bump, index reader/writer,
maintenance on `record`, GC considerations for orphaned index
entries, test-fixture churn. RFC-level design.

**Next step.** Sketch the compact layout and re-estimate index size
before committing any implementation time. If the compact layout
brings overhead to ~500 MB or below, prototype behind a feature flag.

### 3.5 Rejected / retired proposals (with rationale)

These are obvious enough that future readers will re-propose them;
knowing why they were rejected saves the round-trip. Organized by
rejection reason.

**Rejected after microbenchmark — effect at or below the measurement
floor:**

- **L1-hit fast path in `resolveCurrentDepHash`.** Proposed hoisting
  the L1 check above `FileStrandGate::ifPassed`, skipping proof
  construction on hit. A microbenchmark populated L1 with 10,000
  env-var deps and timed two dispatch paths: `verifyTrace` full
  dispatch (Path A: gate + `DepResolution<Unchecked>` typestate +
  `checkCache()` + variant) versus a direct `lookupDepHash` loop
  (Path B: flat_map find only). Post-`is_avalanching` fix: Path A
  ~37.7 ns/op, Path B ~6.8 ns/op. Dispatch costs ~31 ns per hit.
  Bounded aggregate: 2.03 B L1 hits × 31 ns = ~63 s cold, most of
  which is intrinsic `boost::unordered_flat_map::find` the fast path
  can't eliminate. Pre-fix (with the bad hasher), Path A was actually
  ~7% faster than Path B — the dispatch cost was invisible in the
  noise because the bad hasher made the map lookup itself ~500 ns.
  The original 10–20% ceiling estimate was based on dispatch overhead
  that the compiler already eliminates. Retired.

- **Batched `loadFullTrace`.** Observed
  `nrStructVariantLoadKeySetUs = 280.8 s`, but `loadFullTrace` is
  dominated by values_blob deserialisation (~3 ms per 650 KB blob),
  not SQL per-statement overhead. Per-statement overhead saved by
  batching is ~50 µs × ~670 rows × ~200 SV-heavy commits ≈ 7 s ≈
  0.1% of cold. Cheap enough to ship but not worth bisecting
  against. Proposal 2 (keys-only load) has now landed for the default
  path; the old full-value preload remains only behind mismatch telemetry.

- **DirectHash any-dep early-exit.** DirectHash iterates `oldDeps`
  to recompute `newFullHash`, then compares against history.
  Debug-log analysis: N=M in 90.3% of recompute events — DirectHash
  cannot short-circuit; it must compute the full trace hash for
  lookup. Partial events' aggregate savings: ~2.5 s across 500
  commits.

**Rejected after implementation — benchmark regression or
measurement-methodology issue:**

- **Order-preserving `structGroups` vector (try newest first).**
  `scanHistoryForAttr` produces `ORDER BY trace_id DESC`, but the
  `boost::unordered_flat_map<DepKeySetId, TraceId>` in
  `tryStructuralVariantRecovery` destroys insertion order.
  Projected 5–8% savings from processing newest-first. A 100-commit
  benchmark with the change applied showed: cold wall +10.6%, SV
  depResolve +12.1%, SV recovery hits 58 → 6 (90% drop), ~10 s each
  added on ~10 regressed commits. Soundness PASS. Root cause:
  cold-run benchmarking is path-dependent — different iteration
  orders cause SV to pick different winning candidates per attr,
  `publishRecovery` seeds subsequent commits' SV searches with a
  different history population, DB state meaningfully diverges
  after 100 commits. Not a correctness bug; a measurement-
  methodology problem. Retired pending a benchmark methodology that
  isolates iteration-order effects from cache-state drift
  (candidates: pre-populate-at-K measure-K+1 paired runs; replay a
  fixed recorded cache against different orders; N ≥ 5 runs per
  variant and take mean ± stddev).

**Rejected after data analysis — attack surface collapses:**

- **Persistent bucket-rejection index (OPP-1 in earlier drafts).**
  Initial framing: 96.3% of abort events fire on buckets that
  succeed zero times across the 500-commit run. A consecutive-fail
  counter with a skip threshold would avoid most abort work. T=20
  simulation on the monotonic benchmark: saves 78.2% of tries,
  loses 2.1% of wins, 1,973 s of 2,516 s SV depResolve wall.

  The monotonic premise does not generalise. An iterative-edit
  harness (a small `builtins.fromJSON` fixture cycled through 5
  distinct shapes with ~55% revisit bias, 100 iterations, 5 seeds,
  capturing per-iteration `byDepKeySet` via `NIX_SHOW_STATS`)
  measured:

  | Metric | Iterative | Monotonic |
  |---|---|---|
  | Bucket-death rate | **1.8%** | 94.3% |
  | Resurrection rate (k=3 fails) | **93.3%** | n/a |
  | Resurrection rate (k=5 fails) | 50.0% | — |
  | SV success rate | **64.9%** | 0.6% |

  T-threshold simulation on the iterative workload:

  | T | saved tries | missed wins | saved µs |
  |---|---|---|---|
  | 3 | 22.0% | 21.5% | 19.4% |
  | 5 | 4.8% | 4.1% | 6.6% |
  | 10 | 0.0% | 0.0% | 0.0% |
  | 20 | 0.0% | 0.0% | 0.0% |

  Consecutive-fail skipping works when dead buckets stay dead.
  Under iterative editing, dead buckets overwhelmingly resurrect.
  At T=3, skipping is near break-even (loses 22% of wins to save
  22% of tries); at T≥10, the threshold never fires because reverts
  rescue buckets before they accumulate enough consecutive fails.
  SV success rate inverts from 0.6% to 64.9% because reverts bring
  state back to a shape an older bucket recorded exactly. The
  consecutive-fail trigger shape is wrong.

  Caveats: fixture is 5 shapes × 14 probes, not nixpkgs-scale ~32K
  deps/bucket. Percentages are qualitative; absolute µs figures are
  tiny because the workload is tiny. All five seeds converge on
  the same qualitative answer.

  The proposal as framed is retired. A workable variant would need
  a workload-discriminating trigger (age-of-last-success, not
  consecutive-fail count) or must accept that it trades iterative-
  editing recovery latency for cold-wall savings on monotonic
  workloads.

- **Runtime-adaptive candidate scheduling** (reorder candidates by
  learned success probability). 0.6% success rate across all tries
  on the 500-commit monotonic benchmark, and top-20 most-tried
  buckets are all 0%. Not a learnable distribution in the "reorder
  by success probability" sense. The "skip known-dead buckets"
  shape was the persistent-rejection proposal above, which is
  retired on iterative workloads.

**Rejected after code review — redundant with existing mechanism:**

- **Recording-side shape tag on StructuredProjection deps** (per-dep
  kind tag to pre-reject with a 1-byte check). `resolveShapeSuffix`
  already returns `nullopt` when the DOM kind doesn't match the
  suffix query, and the DOM parse is cached in `jsonDomCache` /
  `tomlDomCache` / `nixAstCache` after first hit. A per-dep tag
  would save only the navigation cost (few µs per dep), not a
  parse. Ceiling <1% of cold.

- **Session-scoped DOM cache extension** (cache
  `(FileCacheKey, StructuredPath, ShapeSuffix) → DepHashValue` on
  `ParseCaches`). Redundant with existing L1.
  `VerificationSession::currentDepHashes_` keys on the full
  `Dep::Key`, which includes `(sourceId, filePathId, dataPathId,
  suffix, hasKeyId, dirSetHashId)`. Two candidate buckets querying
  the same `package@t$["package"]#len` produce identical `Dep::Key`
  values (because `dataPathId` is interned from the structural
  path), which means they map to the same L1 entry. OPP-4 would
  help only if two different `Dep::Key` instances navigated to the
  same DOM path — but that contradicts the interning invariant.

**Rejected on architectural grounds:**

- **Variant / always_inline micro-optimisations** separate from
  L1-hit fast-path inline. Subsumed by that proposal's rejection.

- **Debug-only `Linear<>` dtor abort** — weakens a cross-cutting
  invariant for ~50–100 ms. Asymmetric release-vs-debug safety
  regression.

- **Counter batching** — counters are already zero-cost when
  `NIX_SHOW_STATS=0` (`Counter::enabled` false branches out).

- **Dense `Dep::Key` ID interning** — ~5% ceiling, touches the L1
  invariant barrier. Not worth the integration risk.

- **Roaring bitmap / columnar values_blob** — schema bump for
  speculative gain; `traceFullCache` amortises blob loads across
  candidates. Storage is ≤8% of SV on current data.

- **Memoisation: session-scoped rejection cache, session-scoped
  positive result cache, persistent dep-hash cache, persistent
  rejection ledger.** Within-session SV calls are over disjoint
  `pathId`s so no same-session reuse exists; the positive result
  cache is already implemented at the `CurrentNode` layer via
  `publishRecovery`; persistent fingerprint variants either cost
  the same work SV was trying to skip (sound fingerprint) or are
  unsound (cheap fingerprint).

- **Prefix / Bloom / Merkle / per-candidate MinHash.** Candidates
  grouped by `DepKeySetId` in `structGroups` produce identical
  `computePresortedTraceHash(repDeps)` under the `CandidateDep`
  origin (the stored `dep.hash` is overwritten to
  `hash(op(current F))` before hashing), so per-candidate
  fingerprint structures operate at the wrong granularity.
  `History.trace_hash` already provides exact per-candidate hash
  matching via `lookupCandidate`.

- **"Invert the loop" — iterate current-state keys and do
  membership tests.** The existing L1 cache is already a map keyed
  on `Dep::Key`; once populated, per-candidate lookups are O(1)
  map accesses. Inverting renames the same work.

- **Batching for non-StorePathAvailability CQKs** (FileBytesBatch,
  DirectoryEntriesBatch, GitRevisionIdentityBatch,
  EnvironmentLookupBatch). `StorePathBatch` exists because
  `queryValidPaths` is a bulk daemon RPC with real latency
  amortisation. The other CQKs already have per-kind deduplication
  layers (`fileContentHashCache`, `dirListingCache`,
  `gitIdentityCache`, L1 itself).

- **Parallel candidate evaluation.**
  `boost::unordered_flat_map` is not thread-safe; SV runs under
  `ExclusiveTraceStoreAccess` which forbids releasing the store
  mutex across `co_await`. Architectural rewrite + per-thread L1
  shards would defeat the 98% L1 hit-rate that makes the current
  serial loop cheap.

- **SIMD canonical hash combine.** `computePresortedTraceHash`
  uses the active eval-trace backend through `HashSink`; the default
  BLAKE3 backend is already SIMD-vectorised internally. Total hash
  time in the measured default-backend run was ~0.27 s on the worst
  commit (<1% of SV).

- **Try-last-success-first scheduling, size-order scheduling.**
  Dominated by newest-first DESC vector reorder (itself retired,
  above).

- **DepKeySet canonicalisation / values_blob dedup / delta
  encoding.** 98.3% of DepKeySets are already singletons; all 683
  values_blobs are distinct; storage is 5.7–8.3% of SV. All attack
  ≤8% of SV time.

- **Whole-candidate shape signature pre-filter.** Unsound as a skip
  heuristic: SV's success condition depends on whether the
  candidate's own deps resolve correctly under current state, not
  on query-shape overlap with the current trace. A candidate
  recording `#len` on `package[0]` can still succeed if
  `package[0]` is still an array in current state, regardless of
  whether the current trace also queries that path. The sound
  version — "given that dep D resolves-fails in current state,
  skip candidates whose dep set includes D" — reduces to iterating
  candidate dep sets to check for D, which IS the 32,500-dep loop
  the pre-filter is trying to avoid. The only way to avoid the
  loop is Proposal 3's inverted index.

- **values_blob content-addressed dedup.** 705 traces × avg 648 KB
  = 458 MB of 514 MB DB. Ceiling 2–3% of cold (I/O + deserialize
  savings on caches that page out). For the current 500 MB
  benchmark DB that fits easily in page cache, near-zero. Lever
  scales with DB size — deferred indefinitely; revisit if
  benchmarks start showing DB size impact on workloads beyond 500
  commits.

- **Debug-log argument-evaluation cost.** 55,087 abort-reason lines
  each call `resolveDep(*abortDep)` to stringify the key. The outer
  `if (verbosity >= lvlDebug)` guard (already in place) prevents
  this when `--debug` is off. Only a cost when running debug-mode
  analyses of benchmark runs. No code change.

- **`depTracker.ownDepsMax = 88,561` investigation.** One scope in
  the worst commit holds 88K deps directly — 2.7% of the total
  3.3M dep observations. Likely a large attrset comprehension
  (nixpkgs `all-packages.nix`) bulk-recording. Unknown if reshaping
  this scope changes anything measurable; would require
  evaluator-level instrumentation. Investigation-only, unknown
  ceiling, not scheduled.

### 3.6 Residual cost not addressed by any current proposal

The **eval-side tracing overhead (~4 s flat per full-miss commit)**
is separate from the cache-design space. Reducing it requires
optimising the dep-recording hot path itself: fewer epoch-log
appends, cheaper scope push/pop, or deferring instrumentation for
primops that can't produce deps. This is a larger architectural
question than the SV landscape this section covers.

### 3.7 Analysis cycle

1. Run `nix run .#eval-trace-bench -- generate --nixpkgs ~/nixpkgs --nix
   <checkout> --num-commits 500 --runs reference,cold,hot --run-number <N>`.
2. Run `nix run .#eval-trace-bench -- runs --nix <checkout>
   --runs reference,cold/<N>,hot/<N> --reference reference`.
3. Inspect `byDepKeySet` from the cold-run stats JSON.
4. For Proposal 1, also sum `earlierHashMismatchSavedDeps` and
   `hashMismatchOnlySavedDeps` across buckets.
5. For any proposal whose decision criterion names specific
   telemetry or log signals, answer the decision question from
   the data. A proposal whose ceiling evaporates under real data
   does not ship.

For Proposal 1 specifically: one cycle suffices; decision is
arithmetic on `earlierHashMismatchSavedDeps`.

For iterative-editing characterisation (of the shape that retired
the persistent-bucket-rejection proposal): a harness needs to
write a `builtins.fromJSON` fixture with several shape-mutating
variants, cycle them with a revisit-biased PRNG, run `nix eval`
per iteration with `NIX_SHOW_STATS=1`, and compute deltas against
the cumulative `evalTrace.structVariant.byDepKeySet` between
iterations. Key measurements: bucket-death rate (buckets ever
tried but never succeeded), resurrection rate at k consecutive
fails (buckets that hit k fails but succeed later), and
T-threshold simulations of skip-on-consecutive-fail policies.

---

## Appendix A: verification method

For each claim in the CLAUDE.md corpus and the plans/research
documents, the sweep:

1. Located the cited file and symbol in current HEAD.
2. Read the surrounding context.
3. Cross-referenced against the claim's textual description.
4. Searched for symbols named in the claim (e.g.,
   `patchTraceHashInMemory`, `governingRepoId`, `canSubsumeShortcut`,
   `nrGitIdentitySkips`, `TraceStoreService`, `activeRegistry_`).
5. For silent-fix hunting: read commit messages across the
   commits between `90e908ad6` (the CLAUDE.md baseline) and
   HEAD, cross-referenced commit diffs against whether the relevant
   CLAUDE.md entry was updated.
6. For missing-index detection: compared `find <dir> -name '*.cc'`
   output against backtick-wrapped citations on the CLAUDE.md
   contents (script in Appendix B).
7. For fixture hierarchy: listed each `class X : public Y` in
   `helpers.hh` and in-test fixture classes, checked each appears in
   §2.
8. For count-drift detection: ran `grep -c '^# Test ' tests/functional/**/*.sh`
   and cross-checked against CLAUDE.md enumerations.

Coverage was ~100% of claims in OR-1..OR-10 and DEF-1..DEF-8, plus
all §N items and all closure notes.

---

## Appendix B: evidence pointers for reviewers

Direct verification commands:

```bash
# Confirm current schema epoch:
grep 'kSchemaEpoch\s*=' src/libexpr/include/nix/expr/eval-trace/store/trace-store.hh

# Confirm DB filename pattern:
grep 'eval-trace-v' src/libexpr/eval-trace/store/trace-store-lifecycle.cc

# Confirm VerifyContext field count against the code:
grep -A 10 'struct VerifyContext {' \
  src/libexpr/eval-trace/store/trace-store-verify.cc

# Confirm PathCountersSnapshot still flips Counter::enabled:
grep -A 20 'struct PathCountersSnapshot' \
  src/libexpr-tests/eval-trace/helpers.hh

# Find the origin-gated subsumption:
grep -B 2 -A 15 'canSubsumeShortcut' \
  src/libexpr/eval-trace/store/dep-resolution-service.cc

# Confirm srcToStore fingerprint guard references the OR-3 DISABLED test:
grep -B 1 -A 12 'srcToStore.*cache keys by' src/libexpr/eval-environment.cc

# List tests in source-tree-soundness.cc:
grep 'TEST_F(' src/libexpr-tests/eval-trace/dep/source-tree-soundness.cc

# List all BUG-N references in production (BUG-6 should not appear):
grep -rn 'BUG-[0-9]' src/libexpr/eval-trace/ \
  src/libexpr/include/nix/expr/eval-trace/

# Confirm SV early-exit instrumentation fields are populated:
grep -A 4 'earlierHashMismatchCount' \
  src/libexpr/include/nix/expr/eval-trace/counters.hh

# Confirm keys-only load path exists in the store:
grep -A 5 'loadKeySet' \
  src/libexpr/include/nix/expr/eval-trace/store/trace-store.hh

# Confirm TraceStoreService / verify-pipeline.hh are gone:
grep -r 'class TraceStoreService\|verify-pipeline\.hh\|trace-store-protocol\.hh' \
  src/libexpr/eval-trace/ src/libexpr/include/nix/expr/eval-trace/

# Confirm activeRegistry_ / activeBackend_ are gone:
grep -rn 'activeRegistry_\|activeBackend_' src/libexpr/

# Confirm nrGitIdentitySkips does NOT exist:
grep -rn 'nrGitIdentitySkips' \
  src/libexpr/include/nix/expr/eval-trace/counters.hh \
  src/libexpr/eval-trace/counters.cc \
  src/libexpr-tests/eval-trace/

# Confirm isFileVerified is trace-scoped:
grep 'isFileVerified(' src/libexpr/include/nix/expr/eval-trace/store/verification-session.hh

# Confirm eval-trace-deps.sh test count:
grep -c '^# Test ' tests/functional/flakes/eval-trace-deps.sh

# Missing-from-index survey (run from src/libexpr-tests/eval-trace/):
for d in dep store verify; do
  echo "=== $d ==="
  comm -23 \
    <(find $d -maxdepth 1 -name '*.cc' -printf "%P\n" | sort) \
    <(grep -oE "\`$d/[a-z0-9._-]+\.cc\`" CLAUDE.md \
      | sed "s|^\`$d/||;s|\`$||" | sort -u)
done
```

