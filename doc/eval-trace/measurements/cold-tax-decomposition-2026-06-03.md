# Eval-trace cold + hot cost — measured decomposition (2026-06-03)

**History (this file had two WRONG framings before the version below; git history holds them):**
1. ~~"GC mark amplification +30.9s is the dominant cold tax"~~ — **RETRACTED**: that
   was Boehm parallel-marker *spin-wait* cpu (perf `%×cpuTime` artifact); forcing
   GC 7→2 cycles left the tax unchanged.
2. ~~"capture is free (+1.0s), the record is the entire tax (+46.9s)"~~ —
   **CONFOUNDED** (found by an adversarial re-review that reproduced everything
   independently): the `NIX_EVAL_TRACE_NO_RECORD` gate does NOT keep dep capture —
   it also skips per-attr `materializeResult`, so capture-at-scale is elided, not
   isolated. See "Refuted / not separable" below.

The version below is what survived independent reproduction + adversarial review.
Method: build-gated isolation (`outputs/out/bin/nix`, built via
`nix develop .#native-clangStdenv`); workload = python3Packages outPaths
(`mapAttrs`+`tryEval`, ~32,518 attrs), isolated `XDG_CACHE_HOME`; outputs verified
byte-identical to `--no-eval-trace`. **n=1/phase except where noted — cold wall
varies ~±7% (measured), so treat all cold digits as ±2–3s; only directional
claims are trustworthy.**

## BUILD VALIDITY (release-confirmed) — read this re: "did you use an optimized build?"

The numbers in this doc were first taken on the **dev** build (`nix develop` →
`mesonBuildType=debugoptimized`, **-O2**, asserts ON), NOT the release build. That
was a methodology gap. It was then RE-MEASURED on a release-equivalent build
(`-O3`, asserts ON) and the numbers match within noise:

| config | dev -O2 | release -O3 |
|---|---|---|
| notrace | 19.3s | 19.0s |
| default (defer ON) | 58.6s | 58.5s |
| defer-flush=false | 66.6s | 70.0s |
| STUB (capture+materialize) | 32.5s | 32.3s |
| closures.gnome cold / hot | 11.8 / 0.34s | 12.0 / 0.36s |

**Why they match:** Nix FORBIDS disabling asserts — `src/libutil/util.cc:13` is
`#error "Nix may not be built with assertions disabled (i.e. with -DNDEBUG)."`. So
the release build (`packaging/components.nix:129` `mesonBuildType=release`) keeps
asserts ON too; release vs dev differ ONLY by `-O2` vs `-O3`, which is negligible
for this GC-marker / SQLite / eval-bound workload (the dev shell is
`packaging/dev-shell.nix:262` `debugoptimized`). So all conclusions below hold on
release. (`-Db_ndebug=true` does not even compile — the `#error` fires.)

CAVEAT: `nix build .` (canonical release) FAILS its check phase on ONE test —
`flakes / flake-in-submodule`. ROOT-CAUSED 2026-06-04 (see
`flake-in-submodule-stale-2026-06-04.md`): a PRE-EXISTING eval-trace soundness bug
(stale serve on a dirty git-submodule flake), NOT this session and NOT deferFlush
(the old f1398d126 binary fails identically; `--no-eval-trace` passes). The
upstream test (master, Eelco 2025-09-25) passes because master has no eval-trace.
ALL 8 eval-trace functional tests + unit tests + the settings-sensitive
`eval-trace-info` pass on release; this is a separate flake-identity defect
(flake.cc:166-172 normalizes `<rev>-dirty`→`<rev>`).

## SOLID (survived adversarial review)

1. **GC is NOT the tax (decisive).** `GC_INITIAL_HEAP_SIZE=12GB` cuts GC 7→2
   cycles (verified in `.gc.cycles`); cold tax unchanged (~+47s). Nix's own
   `.time.gc` = 0.24–0.32s either way. The retracted "libgc +30.9s" was marker
   spin-wait (∝ wall, off critical path; `GC_MARKERS=1` barely moved cpu).
   Independently CONFIRMED.

2. **The cold tax (+~47s wall) is eval-trace's per-attr work at scale, dominated
   by RECORDING.** `record.timeUs` ≈ 27.7s (real, timed), `record.flushUs` ≈ 6.2s,
   `record.hashUs` ≈ 15.2s. Capture-at-scale (84,740 `DepCaptureScope`s, ~21.7M
   dep records, `valueIdentityMap` 53K) is also real and is NOT separately costed
   (see Refuted). So: recording is the dominant *named* component; capture is a
   real, entangled co-cost.

3. **`deferFlush` recovers ~8s, byte-identical.** `NIX_EVAL_TRACE_DEFER_FLUSH=1`
   (existing Layer-2a batch): cold 66.9s → 58.8–59.3s, `flushUs` 6.2→0. Sound,
   ~1 line to default on. Independently CONFIRMED.

## HOT path (the user's question — "shouldn't hot be near-instant?")

> **WORKLOAD CAVEAT (added after the user flagged it): the 3.7s below is
> python3Packages outPaths (32,518 traces / 18.7M deps) — NOT the canonical
> `eval-trace-bench` workload, and I did not use the bench. The bench evaluates
> `closures.gnome` (~7 consumer traces). Measured 2026-06-03 with the actual bench
> tool AND raw eval: `closures.gnome` HOT = 0.9s (bench, harness overhead) / 0.34s
> (raw eval), matching the existing baseline (0.85s median, 100 commits). The §3b
> code is unchanged (f1398d126) since that baseline. So HOT IS NOT REGRESSED; the
> "3.7s" is a different, ~4600×-larger workload. Hot cost is O(total deps verified
> per run): closures.gnome ~7 traces → 0.34s; python3Packages 32K traces / 18.7M
> deps → 3.7s. Both are the cache working correctly at different scales; neither is
> a regression. My earlier "warm-serve isn't fast / hot 3.7s" framing was wrong —
> it generalized from an oversized non-canonical workload.**

Cold then hot×3 on ONE warm cache (python3Packages; byte-identical to `--no-eval-trace`):

| run | wall | cpu | misses | deps re-checked | tracesLoaded |
|---|---|---|---|---|---|
| cold | 66.6s | 56.0s | 32,518 | — | — |
| hot-1 | 5.3s | 3.5s | **2** (self-heal) | 18.67M | 21,051 |
| hot-2 | **3.67s** | 2.32s | 0 | 18.68M | 21,052 |
| hot-3 | **3.61s** | 2.30s | 0 | 18.68M | 21,052 |
| (no-trace, full eval) | 19.1s | 16.1s | — | — | — |

**Hot is ~3.7s steady (hot-1 is 5.3s with 2 transient self-healing misses), NOT
near-instant — and that's a fair criticism.** Each hot run, with NOTHING changed,
still: loads 21,052 traces from SQLite (~0.9s) and **re-verifies ~18.68M
dependencies** (`verifyTrace.timeUs` ≈ 2.18s) — re-resolving + re-comparing every
recorded dep to prove the cache is still valid. cpu 2.3s + ~1.3s SQLite I/O.
File-content hashing is NOT the cost (H1 cache: 9.64M hits, `contentUs` ≈ 138µs);
the cost is the sheer COUNT of per-dep checks + trace loads. So:

- "Fast" is defensible ONLY relative to cold full-eval: 3.7s vs no-trace 19.1s =
  **5.2× faster**, misses=0 (no silent re-eval). The cache genuinely works.
- "Near-instant" is NOT achieved, and the framing "warm-serve is already fast"
  oversells it. A near-instant hot needs a COARSE short-circuit — a closure-/
  generation-level fingerprint that, when unchanged, skips the 18.68M per-dep
  re-verifications. That is the unbuilt H3-style lever (see
  `memory hot_path_rerank_2026-06-01`: hot cost = stats + blake3; H3 is the top
  lever). Today hot pays O(total deps) every invocation.

## Refuted / not separable (own it)

- **"capture = +1.0s" is a CONFOUND, not a measurement.** `NIX_EVAL_TRACE_NO_RECORD`
  makes `publish()` return false (trace-session.cc), which sets `hasBackendRecord`
  false, which makes `evaluateResolvedTarget` do `v = *target; return;` and SKIP
  `materializeResult` (trace-session.cc:~188 → `cache/materialize.cc` `installChildThunk`).
  `materializeResult` is the only thing that installs per-child `TracedExpr` thunks.
  So NO_RECORD evaluates the `mapAttrs` as ONE traced root (misses=1,
  `depTracker.scopes`=1, `ownDepsTotal`=2,513, `valueIdentityMap`=0) and the other
  32,517 attrs as PLAIN UNTRACED Values. The full run has misses=32,518,
  scopes=84,740, ownDepsTotal=21.7M, valueIdentityMap=53,344. **So the gate
  separates "build+record the traced tree" from "force the tree untraced" — NOT
  "capture" from "record."** Capture-at-scale is elided by the gate and folded into
  the +47s; it is UNCOSTED. Do not cite "+1.0s capture / +46.9s record" as a clean
  split.
- **"real record CPU ~5-6s / hashUs is GC-stall wall"** — inconsistent: GC is ~0.3s
  (SOLID #1), so `hashUs`=15.2s cannot be mostly GC-stall. Record cpu is higher
  than 5–6s and is not independently measured. The "~5–6s" perf-derived figure is
  withdrawn.

## CLEAN capture-vs-record split — DONE (the correct gate)

Built the correct gate `NIX_EVAL_TRACE_STUB_RECORD` (trace-session.cc `publish()`):
returns a valid dummy traceId ⇒ `hasBackendRecord` stays true ⇒ per-attr capture
AND `materializeResult` run at FULL scale, but the entire record pipeline +
blocking dispatch are skipped. Counter-verified it keeps capture (misses=32,518 as
in full, unlike NO_RECORD's misses=1). All outputs byte-identical. GNU-time, n=1:

| config | wall | cpu | maxRSS |
|---|---|---|---|
| notrace | 19.3s | 16.2s | 1390MB |
| STUB_RECORD (capture+materialize, no record) | 32.5s | 28.2s | 1631MB |
| full | 66.7s | 56.1s | 4374MB |

- **capture+materialize-at-scale = STUB − notrace = +13.2s wall / +0.24GB.** So the
  withdrawn "+1.0s capture" was an order of magnitude low; capture-at-scale is a
  REAL cost (~13s), but still the minority.
- **record pipeline = full − STUB = +34.2s wall / +2.74GB.** The record dominates
  BOTH time and memory: eval-trace cold is 4.37GB vs no-trace 1.39GB (+3GB), almost
  all from the record (the 669K-entry retaining `replayEpochMap` + pending entities
  + trace data). So the cold lever is unambiguously the record pipeline.
- This supersedes "+1.0s capture / +46.9s record." Correct split: capture+materialize
  ~13s, record-pipeline ~34s (of the ~47s total).

## LANDED: `eval-trace-defer-flush` setting (default true)

The flush batch is now a real setting (eval-settings.hh `evalTraceDeferFlush`,
default true), wired via the process-global pattern (hash-spec.cc, set at eval
startup), read on the record path (context.cc). Verified on `outputs/out/bin/nix`:

- python3Packages cold: **58.6s default-on vs 66.6s `--option eval-trace-defer-flush
  false`** (−8s), both byte-identical to `--no-eval-trace`.
- closures.gnome (canonical bench): cold 12.1s, **hot 0.4s** (unchanged), byte-identical.
- nixpkgs `asciidoc.nativeBuildInputs` (CRITICAL correctness test): cold==notrace and
  warm==notrace IDENTICAL.
- Crash-safe (Layer-2a: Traces-before-Sessions/History preserved). Memory tradeoff
  (+376MB at 32K records; ~0 at normal scale) documented in the setting; disable for
  very large single cold evals on constrained hosts. Bounded periodic-flush (cap the
  pending buffer) is noted as future work — it mixes deferred/non-deferred records
  and needs the flush-drains-pendingPublish ordering verified first, so NOT done here.

## Methodology lessons (logged)

- perf `%×cpuTime` over-attributes to libgc because Boehm markers spin-wait; ALWAYS
  cross-check a GC hypothesis with the app's GC timer + a GC-frequency knob.
- `recT`/`hashUs` are WALL timers (absorb stall/lock/IO/scheduling); not cpu.
- A skip-the-thing gate is only an isolation if it skips EXACTLY that thing — verify
  with counters (here: `misses`, `depTracker.scopes`) that the gate didn't also
  disable an upstream stage. The NO_RECORD gate failed this and I missed it; the
  adversarial subagent caught it via the counter deltas.
- n=1/phase under-states uncertainty when wall varies ~7%.
