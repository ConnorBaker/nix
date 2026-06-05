# Verify execution-model spikes — synchronous + data-parallel (2026-06-01)

Living log for two measurement spikes that follow from EXPERIMENT 1 in
`eval-trace-perf-cold-and-hot.md` ("EXPERIMENT 1 RESULT (2026-06-01)"). Repo-durable
notes per request; not memory-only.

## Why these spikes (from Experiment 1, perf-grounded)
Warm `-f closures.gnome` hot eval ≈ 0.94s wall = **user 0.45s + sys 0.40s**, effectively
**single-threaded** ((user+sys)/wall ≈ 0.9 cores). `taskset -c 0` (1 CPU) == 32 CPU wall.
On-CPU: blake3 13.6% + tbb 13.2% + malloc/gc 13% + stat 4.6% + futex/yield/asio ≈3%.
sys 0.40s = 177,290 `newfstatat` (per-dep stat walk + 35,931 ENOENT path-probes).
The multi-thread apparatus (asio io_context[2] + BlockingThreadPool[2] + TBB-blake3 +
parallel-GC) delivers ZERO wall benefit because it parallelizes intra-hash (TBB, useless
at these file sizes) and overlaps page-cached I/O (nothing to hide) but SERIALIZES the
128K-dep walk — the actual bottleneck.

Two questions:
- **Spike 1 (synchronous verify):** is the async apparatus removable with no warm
  regression? (Pre-justified by taskset; confirm as a real code path + no correctness drift.)
- **Spike 2 (data-parallel verify):** does sharding the dep walk across cores give a REAL
  speedup (the parallel ceiling), justifying the thread-safety work?

Both env-gated, default-off, A/B in one release binary.

## Spike 1 — synchronous verify (DISPATCH GATE, not a rewrite)

**Discovery:** a synchronous verify path ALREADY exists and is result-equivalent to the
async one:
- async: `TraceBackend::verify` (context.cc:299) → `ctx.syncAwait(verifier->verifyAttr())`
  → `verifyAttrImpl` (verifier.cc:2165): prefetch-check + each step wrapped in
  `coroBlock(blockingPool, …)`.
- sync: `TraceBackend::verifySync` (context.cc:444) → `Certifier<BlockingTag>::withProof`
  + `withExclusiveAccess` + `verifyAttrSync` → `SqliteTraceStorage::verify` (verifier.cc:2022):
  identical `lookupCurrentNode → (history bootstrap) → loadTraceKeysAndHeader → volatile
  check → verifyTrace → decode | recovery`, MINUS the speculative prefetch-pool check and
  the coroBlock wrapping.

Minting `BlockingTag` on the calling (eval) thread is sound: the tag exists to keep blocking
I/O OFF the io_context WORKER threads (shutdown-deadlock avoidance), not off the eval thread —
the eval thread blocks anyway under `syncAwait`. `flush`/`recordSync`/`recordRuntimeRoot`/
`verifySync` already do exactly this.

**Change:** env-gate `TraceBackend::verify` — when `NIX_EVAL_TRACE_SYNC_VERIFY=1`, return
`verifySync(pathId)` (inline, no io_context/coroBlock/prefetch). Default off = async path.
Warm-hit caller is `trace-session.cc:611 cache->runtime_->verify(*ctx, pathId)`.

### Adversarial questions to settle (before trusting the A/B)
1. Result-equivalence: does the prefetch-pool skip change any RESULT (vs just timing)? The
   prefetch pre-verifies speculatively; the sync path recomputes the same verifyTrace →
   same bool → same decode/recovery. Must confirm byte-identical eval output + identical
   hit/miss/recovery counters async-vs-sync on the real workload.
2. Does anything DOWNSTREAM depend on the async side effects (prefetch pool state, io_context
   liveness, counters like nrVerifyTimeUs which the sync path may not bump)? Check counter
   deltas; a missing timer is cosmetic, a missing state change is not.
3. Re-entrancy / nested verify: can verify recurse (a verify that triggers another)? The
   sync path holds `withExclusiveAccess` (storeMutex_) for the whole verify; if verify can
   re-enter and re-acquire storeMutex_ → deadlock. The async path releases between coroBlocks.
   MUST check whether verifyTrace/recovery re-enter verify or acquire storeMutex_ again.

### Adversarial resolution (pre-build, code-grounded)
- **Q3 re-entrancy/deadlock: RESOLVED, no deadlock.** `verifyTrace` (verifier.cc:1891) takes
  `ea`/`bs` (the held-mutex proof) and threads it through `loadFullTrace`/`runPass1/2`/
  `applyOutcome` — it NEVER re-acquires `storeMutex_`. Its cross-trace recursion
  (TraceValueContext/TraceParentSlot → resolveTraceContextHash → verifyTrace) is
  cycle-detected (`inProgressTraceIds`) and runs SYNCHRONOUSLY under the same single mutex
  acquisition in BOTH paths (the async path does the whole recursion inside ONE
  `coroBlock(verifyTrace)`, verifier.cc:2253). The sync path merely also holds the mutex
  across the cheap lookup/decode steps — fine single-threaded.
- **Q2 counters: sync `SqliteTraceStorage::verify` does NOT bump `nrVerifyTimeUs`** (that's
  only in `verifyAttrImpl`:2287); it bumps `nrVerifyTraceTimeUs` (1942). So `verify.timeUs`
  will read 0 in sync mode — COSMETIC. Compare `verifyTrace.timeUs` + wall + hit/miss.
- **Q1 result-equivalence:** to be confirmed empirically (byte-identical output + identical
  hits/misses/recovery counters async-vs-sync on the real workload).

### Results (2026-06-01, release `result-syncspike/bin/nix`, warm `-f closures.gnome`)
| mode | wall (3 trials) | output vs `--no-eval-trace` | hits/misses | depsChecked | verifyTrace.timeUs | verify.timeUs |
|---|---|---|---|---|---|---|
| async (default) | 0.94/0.95/0.94 | IDENTICAL | 7/0 | 128028 | 282598 | 291113 |
| sync (`NIX_EVAL_TRACE_SYNC_VERIFY=1`) | 0.95/0.92/0.93 | IDENTICAL | 7/0 | 128028 | 290714 | **0** |

**CONCLUSION: async apparatus is REMOVABLE with zero warm regression.** Byte-identical eval
output (sync == async == truth), identical verification counters, same wall (sync marginally
faster, within noise — matches on-CPU coordination ≈3%). `verify.timeUs=0` in sync PROVES the
gate switched paths (non-vacuous; `nrVerifyTimeUs` is only bumped by `verifyAttrImpl`). The
win is **simplicity + a clean single-threaded substrate for spike 2**, not wall time — the
bottleneck (177K stats + hashing) is the same work regardless of dispatch.

### Adversarial review of the Spike-1 result
- A/B is fair: same binary (env-gated), same warm cache, same workload. ✓
- Non-vacuous: `verify.timeUs` 291113→0 proves the sync branch executed. ✓
- Correct: byte-identical output + identical hits/misses/depsChecked/contentUs/recovery. ✓
- **Residual gap:** `recovery.attempts=0` here — the RECOVERY branch of the sync path was NOT
  exercised (clean PASS). It calls the SAME `store_.recovery` the async path does (verifier.cc
  2055 vs 2282), so it's equivalent by construction, but UNTESTED in this A/B. If spike 1 ever
  goes to production, add a source-edit→fail→recovery A/B. For the measurement goal (async
  removable, same wall) this is sufficient.
- Honest: sync is NOT meaningfully faster (0.93 vs 0.94) — removing coordination buys ~nothing
  in wall because the path is already ~0.9-core-bound. The value is the substrate + simplicity.

### LANDED 2026-06-01 — sync is now the DEFAULT; async behind an escape hatch
`TraceBackend::verify` (context.cc:299) defaults to `verifySync`; `NIX_EVAL_TRACE_ASYNC_VERIFY=1`
restores the async orchestrator (still used by `record`/`loadFullTrace`/`getCurrentTraceHash`,
and kept as a regression fallback). The async verify code (`verifyAttr`/`verifyAttrImpl`/prefetch)
is now reachable only via the hatch — left in place (documented, reversible); a future cleanup
can delete it.

**Validation (all green):**
- Full `nix-expr-tests` suite with sync-default: **1859 passed / 4 skipped (documented) / 0 failed**
  (test binary links the fresh `z6gq…-nix-expr`). No regression vs the bee93db baseline (1858/4).
  Covers recovery, soundness, all dep kinds — closes the Spike-1 recovery-branch gap.
- Real nixpkgs `asciidoc.nativeBuildInputs`: cold + warm **byte-identical** to `--no-eval-trace`;
  warm `verify.timeUs=0` + `verifyTrace.timeUs=85638` proves the SYNC path ran by default.
- Escape hatch: `NIX_EVAL_TRACE_ASYNC_VERIFY=1` → `verify.timeUs=123208` (async), output identical.
- closures.gnome: sync==async==truth byte-identical (the A/B above).

**Counter caveat (documented, not fixed):** `verify.timeUs` (Verifier-orchestrator timer) reads 0
on the sync default — honest (no orchestrator), but bench/analysis must read **`verifyTrace.timeUs`**
for the verify cost from now on. (Did not fake-bump it; a follow-up could if continuity matters.)

## Spike 2 — data-parallel verify (parallel ceiling)

**Feasibility (code-grounded).** The per-dep `resolveCurrentDepHash` (verifier.cc:253) touches:
read-only `pools`/`registry`/`resolver` (concurrent-read safe); per-thread file I/O
(stat/read/hash); H1 (SKIPPED on `-f` — `h1StorePathKey` returns nullopt for non-store paths);
and the **shared L1 write** `session.cacheComputedHash`/`lookupDepHash` on `currentDepHashes_`.
The L1 is LOAD-BEARING within one pass — it dedups the 607× flattened duplicates
(`cacheHits=42423` vs `cacheMisses=23192`), so it can't be skipped (skipping recomputes 42K).
⇒ a faithful parallel verify needs a **concurrent L1** (+ concurrent `session` mutation for
`markFileVerified` etc., + the rare EvalEnvironment dep kinds touch `EvalState` — ~0% here).
That is the documented thread-safety wall.

**Engineering order: measure the ceiling before paying for the refactor.** Standalone proxy
micro-bench of the dominant op (stat+read+hash over the real warm file population, 41,960
nixpkgs `.nix` files), serial vs N-thread (`xargs -P -n800`):

| threads | stat+read+hash | speedup | pure stat |
|---:|---:|---:|---:|
| 1 | 0.64s | 1.0x | 0.12s |
| 2 | 0.31s | 2.1x | — |
| 4 | 0.17s | 3.8x | — |
| 8 | 0.12s | 5.3x | 0.03s (4.0x) |
| 16 | 0.08s | 8.0x | — |
| 32 | 0.07s | 9.1x | 0.03s (4.0x) |

**Read+hash scales ~5–8x; pure STAT plateaus at ~4x** (VFS/dentry-cache lock contention) — and
stat (sys 0.40s) is the single biggest verify cost, so ~4x is the binding constraint on it.

**Mapping to the full verify (Amdahl).** Hot wall 0.94s = user 0.45 + sys 0.40. The
parallelizable dep-walk (stat+read+hash) is most of it; the serial floor is parse release.nix
+ materialize + decode (~0.25s) + L1 coordination. Optimistic: walk ~0.65s → ~0.10–0.15s
(stat-bound ~4x), floor ~0.25–0.30s ⇒ **hot ~0.94s → ~0.40–0.45s, a ~2x ceiling.** Real, but
bounded.

### Adversarial review of Spike 2
- **It is a PROXY, not the verify.** Honest caveats, all pushing the real ceiling BELOW the
  micro-bench's 8x:
  1. The micro-bench has ZERO shared state (independent processes) → it's the IDEAL ceiling.
     The real verify's concurrent-L1 + session coordination will shave the speedup (esp. since
     65K L1 ops/pass would contend). Real core speedup likely ~3–5x, not 8x.
  2. sha256 ≠ blake3 (verify's hash is faster) → micro-bench OVERSTATES the hash share; the
     real serial baseline's hash is smaller, so less to gain there. Stat (sys) dominates anyway.
  3. The micro-bench does 42K stats; the verify does **177K** (incl. 35,931 ENOENT
     path-resolution probes) — the ENOENT probing is extra serial-ish path logic the proxy
     omits. The stat plateau (~4x) is the real cap and it's captured. ✓
  4. xargs process-spawn (~52 spawns) slightly understates scaling — minor.
  5. Page-cache warm (matches warm hot). ✓ A COLD run (real disk) would be I/O-bound and
     parallelize differently — and is the regime where the EXISTING async overlap actually pays.
- **Conclusion: parallelism IS a real lever (~2x hot ceiling) but (a) needs the concurrent-L1
  thread-safety refactor, and (b) is capped by the ~4x stat plateau + the parse/materialize
  Amdahl floor.** It does not change that **H3 (cut the 128K-dep count) is more fundamental** —
  fewer deps shrinks the stat walk AND the hashing AND removes the need to parallelize, serially.

## SYNTHESIS — answering "parallel? async? both?" (2026-06-01)
- **Async** (current design): pays only when blocking I/O is SLOW (cold disk). On the warm hot
  path the reads are page-cached (`pread` 0.28%), so it overlaps nothing — Spike 1 proved it
  **removable with zero warm regression** (byte-identical, same wall). Keep it for the cold path
  only; it's pure overhead on warm.
- **Parallel** (data-parallel dep walk): the warm path's right tool — the work is genuinely
  parallel (~5–8x core, ~2x hot ceiling), but needs the concurrent-L1 refactor and is
  stat-plateau + Amdahl bounded.
- **Both, split by regime:** async for cold I/O overlap; data-parallel for warm CPU+stat.
  The current design applied the cold tool (async) to the warm path, where it neither overlaps
  (nothing to hide) nor parallelizes (serializes the walk) → the 0.9-core result.
- **But the deeper lever is H3** (the 128K is the 607× flattening; cutting the count beats
  parallelizing the inflated count) and the COLD recording cost (6.7s, where async DOES help).

### Decision point (before committing code)
Three independent, ranked options surfaced; none yet committed:
1. **Land Spike 1** (remove/simplify async on warm) — lowest risk, simplifies, no wall win.
2. **H3** (cut the dep walk) — biggest, input-agnostic, soundness-hard. Attacks the root.
3. **Parallel verify** (concurrent-L1 refactor) — ~2x hot ceiling, real but bounded; medium
   surgery + the thread-safety wall. Worth it only if a 2x WARM win matters more than cold.
