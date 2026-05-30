# Eval-trace cache performance research — consolidated index

**Read this first.** The eval-trace cache performance work is spread across six
documents written over a long research arc. They use **three different
benchmark ledgers** and describe **two abandoned experiment lineages** plus one
living plan. Landing on any single doc without this map has already caused one
reader (and one assistant) to conflate lineages and quote the wrong numbers.
This file is the authoritative entry point: it says which doc describes the
current tree, reconciles the ledgers, and carries the canonical lever list.

## TL;DR — what is true of the current tree

- **Shared baseline commit:** `92a3df1ab` ("File-based eval-trace cache",
  squashed from 39 pre-rebase commits) is the first eval-trace commit on branch
  `vibe-coding/file-based-eval-cache` (its parent `616df9797` is the master
  merge-base), AND it is the `92a3df1ab…` baseline cited by the
  findings/catalog docs. They are the same commit. The eval-trace
  implementation (SQLite-backed capsule store, ~2100-line verifier, 27
  `eval-trace/*.cc`) lives here. HEAD adds only doc/test commits on top.
- **What is NOT in the tree:** the abandoned experiment code — the
  command-JSON / `nix eval --json` action cache (removed 2026-05-27), the v53
  full-capsule rewrite, the Git-manifest/generation-pack backends. Their
  *conclusions* are durable and recorded; their *code* was abandoned because
  the experiments were tightly coupled and had degraded, **not** because the
  ideas failed.
- **Authoritative forward synthesis:** the last two sections of
  `eval-trace-cache-redesign-plan.md` —
  *"2026-05-29 deep-research synthesis"* and *"2026-05-29 CORRECTION after
  thorough work-log read"*. The CORRECTION supersedes everything earlier where
  they conflict. The canonical lever ordering below is taken from it.
- **No perf code has changed since the squash.** HEAD = baseline + doc/test
  commits only (the keyset-provenance harness, the cross-trace keyset-escape
  tests, and these docs).
- **A current-tree baseline now exists (2026-05-30, HEAD `9f7311129`).** See
  the redesign-plan's *"2026-05-30 CURRENT-TREE baseline — Ledger D"* section.
  100-commit `closures.gnome`: soundness PASS on all 100; cold mean 3.72 s
  (median 1.11 s — bimodal, ~23 catastrophic outliers), hot mean 0.96 s
  (0.15× reference, flat). This is the fixed reference point for future lever
  work; the older ledgers (A/B/C) still predate HEAD.

## Document map

| Doc | Role | Lineage | Describes current tree? |
|---|---|---|---|
| **`eval-trace-cache-README.md`** (this file) | Entry point / index / ledger reconciliation | — | — |
| `eval-trace-cache-redesign-plan.md` | **Living plan.** Forward direction + the authoritative 2026-05-29 corrected synthesis | storage lineage (SQLite→Git-manifest→generation-pack) | Yes — its closing synthesis is current |
| `eval-trace-cache-findings.md` | Distilled durable decisions | transparent-rewrite / command-JSON / v53 lineage (abandoned) | Conclusions yes; framing ("back on origin src") is lineage-historical |
| `eval-trace-cache-experiment-catalog.md` | Decision-level extraction of benchmark-significant results | same abandoned lineage | Conclusions yes; code no |
| `eval-trace-cache-work-log.md` | Raw running log (16k lines) underlying BOTH lineages | both | Raw evidence; not a conclusion source |
| `eval-trace-sqlite-writeback-findings.md` | SQLite writeback refactor checkpoint | storage lineage | Partially — SQLite is the current backend |
| `eval-trace-storage-backend-research.md` | Backend survey (Git pack / LSM / mmap) + ledger | storage lineage | Direction only; backend not replaced |

There is also `doc/eval-trace/audit/perf-clarity-followups.md` (13 micro-opt
items, all closed against commits in this branch) and the OR-N "Open research"
section in `src/libexpr/eval-trace/CLAUDE.md`. Those describe the current tree's
code directly and are the right home for code-level soundness/precision gaps.

## Benchmark-ledger reconciliation

The three ledgers are NOT directly comparable. Always state which one a number
comes from.

| Ledger | Where | Workload / metric | Rough scale | Notes |
|---|---|---|---|---|
| A — abandoned transparent-rewrite | `findings.md`, `catalog.md` | 100-commit `closures.gnome`, **per-commit mean** | cold ~3–6 s, hot ~0.3–0.9 s | Best wins (cold 3.13/hot 0.33) were **command-JSON, above libexpr** → disqualified as product path |
| B — storage lineage | `redesign-plan.md`, `storage-backend-research.md` | 100-commit `closures`, **run totals + mean** | cold 432–845 s **total** (mean ~4–8 s), hot 94–257 s total (mean ~0.9–1.5 s) | run 121 = pre-schema SQLite = "baseline to beat" |
| C — instrumented single-shot | `redesign-plan.md` "2026-05-15 instrumented" | one `closures` eval, wall | cold 98 s, hot 1.6 s, no-trace 44 s | Used to attribute phase costs, not to rank levers |

Mean-to-mean, A and B roughly agree (hot ~0.9 s, cold ~4–6 s) because they
share baseline `92a3df1ab`. They diverge in *which experiments* sit on top.
Do not compare an A "mean" against a B "total".

## Canonical lever list (authoritative ordering)

Taken verbatim in intent from the redesign-plan's *2026-05-29 CORRECTION*
"Corrected sequence", which supersedes the earlier 4-finding synthesis and the
findings-doc "Promising Directions". Each lever traces to repo text; the
"source" column points at the durable record.

| # | Lever | Status / evidence | Source |
|---|---|---|---|
| **1** | **Observed-key / unobserved-change PRUNING proof**, made sound-by-construction and transparent to `libexpr` | The single biggest measured win in the whole log (cold 3.32→1.88 s, hot 0.88→0.38 s, v6→v11). Works by **slicing/removing coarse deps**, NOT adding finer ones. **Caveats:** (a) was command-JSON-only (disqualified layer); (b) gated on Nixpkgs path heuristics + env vars, not sound by construction; (c) coverage stops at the observed-key universe — `attrNames`/negative-membership/recursive-attrset are UNSOLVED. It is a tail-rescue (kills ~9 catastrophic outliers; median/p90 barely move), not a hot-path median mover. | redesign-plan §2026-05-29 CORRECTION finding 2; work-log v6→v11 |
| **2** | **Semantic derivation-boundary caching** (Bazel/Skyframe strict dependency capture + facet mask) | **NOT VIABLE as scoped — feasibility spike hit a material architectural blocker (2026-05-30).** Outliers re-eval ~20 M thunks; the 6,419 derivations inside (99.66 % unchanged) have no reuse boundary, ~58 % of cost is cacheable derivation eval. Spike (branch `spike/lever2-derivation-feasibility`) measured 10,455 derivations evaluated vs 6 traces recorded, then hit the blocker: to verify-before-force a derivation it must be a `TracedExpr`, but `TracedExpr` identity is **attr-path-tree-shaped** (Root or Child-with-parent-and-name), and a `derivationStrict` thunk created deep in `make-derivation.nix` has no attr-path from the eval root — `makeChild` cannot construct it. Caching derivations needs a **second content-addressed `TracedExpr` identity model** threaded through evaluator thunk creation = foundational redesign on the hot path, RFC-scale, not a lever. Verdict: leave the cold tail bounded + sound. Full chain: `plans/lever2-derivation-boundary-caching.md` §7-11. | redesign-plan §2026-05-30 spike; findings.md "Derivation Boundary Proof" |
| **3** | **Certificate-before-payload** fast path (fixed-size `FullTraceHash` compare before `loadFullTrace` + dep walk) | LOW priority. Every *sound* form was already refuted on this workload (runs 909/980/987/1032/1042/1133/1107/1108…). The dep walk IS the hot cost for true exact hits (unsound oracle run 1015: hot 0.68 vs 0.88 s), but the residual hot cost is decode/startup, not the walk, once the `verifiedTraceIds` memo is in place. Only un-refuted shape: a cheap per-current-node eligibility bit/index that clears the run-993 coverage bar. | redesign-plan §2026-05-29 CORRECTION finding 1 |
| **4** | **Custom immutable-segment store** (generation packs, mmap fixed-width indexes, lock-free readers, atomic `CURRENT`) | Deferred until 1–3 prove the proof model wins. Storage format is **downstream of authorization**: run 137 showed lazy-payload-over-immutable-objects does NOT beat SQLite without the authorization fix. Do NOT re-abstract `TraceStorage` (the vptr was added per rearch-proposal §2.1, measurably hurt the hot loop, and was reversed). | redesign-plan "Architectural direction" + §2026-05-29 finding 4; storage-backend-research.md |

## DEEP-ATTRSET WORKLOAD FINDING (2026-05-30) — ⚠️ METHODOLOGY UNSOUND, NUMBERS RETRACTED, RE-RUN PENDING

> **RETRACTION (2026-05-30, self-audit):** the magnitude numbers below were
> produced with a **`-O0` debug meson build** (`build/`, `buildtype=debug,
> optimization=0, b_ndebug=false`), whereas the GNOME Ledger-D baseline used a
> **release** `nix build` binary. The eval-trace recording path is exactly the
> code `-O0` penalizes most, so **"12× cold" is inflated by an unknown factor and
> the deep-attrset-vs-GNOME comparison is invalid** (two different compilers'
> output). Additionally: (a) the cross-commit A→B run captured only wall time, NOT
> hit/miss counters, so "reuse delivers nothing" cannot be distinguished from "B
> matched ~nothing in A's cache"; (b) n=1, no repetition; (c) shared
> `NIX_CACHE_HOME`/system store vs GNOME's isolated `_state` harness; (d) the
> workload's `tryEval`+`?outPath` guard may short-circuit an unknown fraction.
> **The DIRECTIONAL structural fact survives** (a deep attrset records ~11 K
> per-package traces vs 6 for GNOME — build-independent, from `record.count`), but
> every wall-time magnitude and the "net-negative / disable it" conclusion are
> RETRACTED. **Corrected by the release-binary re-run below.**
>
> **RELEASE-BINARY RE-RUN (2026-05-30, `result/bin/nix` = the Ledger-D release
> binary; same workload/commits; hit/miss + soundness captured; n=1).**
> `python3Packages` outPaths (~11,062 entries, 91 % produce real outPaths):
>
> | Scenario | wall | hits / misses | sound? |
> |---|---:|---|---|
> | reference (no-trace) | **0:18** | — | — |
> | cold (trace, empty cache) | **2:34** | 0 / 11,062 | — |
> | hot (same commit) | **0:14** | 11,062 / 0 | served == truth ✓ |
> | incremental (parent cold → `streamz` bump) | **0:17** | 11,062 / **1** | served == truth ✓ |
> | incremental reference (no-trace) | 0:18 | — | — |
>
> **Corrected, sound conclusions:**
> 1. Cold is **~8.6× slower** (154 s vs 18 s), not 12× — `-O0` inflated it. Still a
>    real, large recording cost (~11 K per-package traces; I/O-heavy).
> 2. **The cache is SOUND and PRECISE here** (the unsound run never checked this):
>    a one-package bump invalidates **exactly 1** trace and reuses 11,062; served
>    values byte-identical to ground truth in hot AND incremental.
> 3. **Hot/incremental ≈ break-even with no-trace** (14–17 s vs 18 s) — with ~100 %
>    hits, verification overhead roughly equals the eval cost it avoids. The
>    retracted "net-negative, disable it" verdict was WRONG; the corrected reading
>    is "no net win on this workload, but sound and precise."
> 4. A win would require amortization (many warm re-evals per cold) and/or lower
>    cold cost (option 2''). Workload-dependent, NOT "disable it."
>
> **ACCESS-PATTERN FOLLOW-UP (2026-05-30): `mapAttrs` instead of `attrNames` +
> per-key select changes the verdict from break-even to a CLEAR WIN.** Same
> workload semantics (11,061 identical entries, byte-verified), release binary,
> same commits. `builtins.mapAttrs (n: p: …outPath…) python3Packages` iterates via
> the C++ Bindings API — no `attrNames` keyset dep, no per-package `ExprSelect`:
>
> | Scenario | `listToAttrs` (attrNames+select) | **`mapAttrs`** (C++ iter) |
> |---|---:|---:|
> | reference (no-trace) | 0:18 | 0:18 |
> | cold | 2:34 | **1:15** (~2× cheaper) |
> | hot (same commit) | 0:14 | **0:05** (~3.3× vs ref) |
> | incremental (`streamz` bump) | 0:17 (≈ break-even) | **0:08** (~2.2× vs ref) |
> | deps recorded (ownDepsTotal) | 110.9 M | **36.5 M** (3× fewer) |
> | record.hashUs | 91 s | **30 s** |
> | sound / precise | ✓ misses=1 | ✓ misses=1 |
>
> Why: `attrNames`+`${name}` select recorded ~3× more deps per trace (keyset dep +
> per-access deps); `mapAttrs`'s C++ iteration records only each package's intrinsic
> deps. That over-recording was BOTH the bulk of cold cost AND what dragged the
> `listToAttrs` warm path to break-even. **Corrected meta-conclusion: the
> deep-attrset verdict is access-pattern-dependent. On the C++-iteration pattern
> (closer to how `nix-eval-jobs` actually walks the set), the cache delivers a
> clear, sound, ~2× incremental win** — the eval-jobs value case the earlier runs
> failed to demonstrate. The `listToAttrs` "break-even" was partly a
> workload-authoring artifact, not just the `-O0` artifact.
>
> Open caveats (honest): n=1, no repetition; `python3Packages` specifically; warm
> OS disk cache; cold cost still ~4× reference even with `mapAttrs` (recording is
> the lever, option 2''). `nix-eval-jobs`'s real iteration pattern should be
> confirmed against these two bracketing cases.
>
> Original (unsound `-O0`) text retained below struck-through for the audit trail.

~~Option 2 below (benchmark a deep-attrset workload) was run. **Result: on the
workload that matters most for the cache's purpose, the cache is all cost and no
benefit.**~~ *(retracted — see above)* Measured on `python3Packages` outPaths (a ~3,000-package deep attrset,
the `nix-eval-jobs` shape), nixpkgs commit `9b9f7241`, wall clock:

| Mode | Wall | vs reference |
|---|---:|---|
| reference (`--no-eval-trace`) | **1:38** (98 s) | 1.00× |
| cold (trace, recording) | **19:43** (1,183 s) | **12× SLOWER** |
| hot (warm cache) | **1:36** (96 s) | 0.98× (break-even) |

Two facts, both the opposite of `closures.gnome` (where hot was ~7× faster):
1. **Cold recording is catastrophic: 12× slower.** It records ~3,000 per-package
   traces (vs 6 for GNOME — because a package set IS an attr-path-addressable
   deep attrset, so `materialize.cc` wraps each child as a `TracedExpr` and each
   records its own trace). Cold *wall* (19:43) ≫ cold *CPU* (388 s from the stats
   run) ⇒ the cost is **I/O wait: SQLite writes + per-package dep-blob
   serialization**, not CPU.
2. **Hot is merely break-even (96 s vs 98 s) — NO net speedup.** The warm cache
   recovers exactly the recording overhead and nothing more.

**Strategic consequence:** the eval-trace cache's measured profile is workload-
shape-dependent and currently inverted for the most important consumer:
- `closures.gnome` (deep *computation*, shallow data): hot 7× faster, cold tail
  bounded. Cache is a win.
- `python3Packages` (deep *attrset*, the `nix-eval-jobs` shape): cold 12× slower,
  hot break-even. **Cache is a net loss.**

**Cross-commit verdict — SUPERSEDED by the release re-run above; the unsound
`-O0` cross-commit run additionally picked commits (`d9e9`→`9b9f`) whose delta
(a NixOS module) does not touch `python3Packages` at all, so it tested a no-op
delta, not real incremental reuse. The corrected release re-run uses a
`python3Packages.streamz` bump (`2b93ef`→`f37d`) and finds `misses=1` (precise,
sound) with incremental wall ≈ no-trace. Struck-through original below.**

~~Cross-commit verdict (2026-05-30, the decisive follow-up — option 2'):~~ ran
commit A cold (populate cache) → commit B reusing A's cache → B no-trace.
A-cold **19:49**, B-hot-reuse-A **1:36**, B-reference **1:40**. **B-hot ≈
B-reference** — cross-commit reuse delivers essentially nothing even in the
intended N+1-reuses-N case. ~~**So the 12× cold cost never amortizes: the cache is
all cost, no benefit, on the `nix-eval-jobs` shape, full stop.**~~ *(WRONG — `-O0`
artifact + no-op delta; see corrected re-run)*

~~This reframes the option list.~~ The deep-attrset case does NOT want Lever 1
(finer pruning) — it already has maximal per-package granularity and that
granularity is precisely what makes cold catastrophic ~~while the hot side proved
worthless~~ *(corrected: hot ≈ break-even, not worthless)*. The only lever that
could help is **drastically cheaper / fewer per-trace recording** (cold-write
path) plus amortization across many warm re-evals. ~~Practical
consequence: **eval-trace should likely be disabled (or never enabled) for
`nix-eval-jobs`-shape evaluation**~~ *(retracted — the cache is sound and
break-even, not net-negative; "disable it" was unsupported)* until a redesign
demonstrates a hot win on this shape. Caveat: `python3Packages` specifically; another deep attrset could
differ, but it is the canonical shape.

## Where the research stands (2026-05-30) and the remaining options

After establishing the first current-tree baseline (Ledger D) and diagnosing
the cold tail, the lever picture resolved as follows. The cold cost is
concentrated in ~23 outlier commits (mean 3.72 s vs median 1.11 s; hot is a flat
~0.95 s and needs no work). Each outlier re-evaluates ~20 M thunks because a
change to a few system-assembly derivations fails one of only ~7 coarse closure
traces, and **that re-eval is semantically correct** (the failed root traces
genuinely changed — recovery is not buggy; verified via the outlier's recovery
counters: 4 failures = the 4 changed system-assembly roots). So the cold tail can
only shrink by caching at a **finer granularity than the ~7 closure roots** — and
the only finer boundary inside the computation is the derivation, which Lever 2's
spike proved is blocked by the attr-path-shaped `TracedExpr` identity model.

**The honest standing: no cheap lever remains for the `closures.gnome` cold tail.**
The bounded cost (23 outliers, soundness-correct) is the rational thing to leave
in place. The remaining options, in rough order of effort/payoff:

1. **Accept the cold tail (recommended default).** It is bounded, sound, and the
   hot path — the case that matters for repeated CI/`nix-eval-jobs` use — is
   already ~7× faster than no-cache and flat. Spend effort elsewhere.

2. ~~Benchmark a deep-attrset workload~~ **DONE (see finding above).** Outcome
   **CORRECTED (release re-run):** cold **~8.6×** slower (not 12×); hot AND
   incremental ≈ break-even with no-trace; the cache is **sound and precise**
   (incremental `misses=1` on a 1-package bump). The deep-attrset case is
   dominated by per-package RECORDING cost; the warm path neither wins nor loses
   much. See the corrected re-run table at the top of this section.

2'. **DONE + CORRECTED (2026-05-30).** Both a same-commit and a real incremental
   (`python3Packages.streamz` bump, `2b93ef`→`f37d`) cross-commit run, release
   binary, with hit/miss + soundness:
   - parent cold: **2:34**; incremental (reuse parent): **0:17**, `hits=11062
     misses=1`, served == truth; incremental no-trace: **0:18**.
   - Incremental reuse WORKS and is SOUND/PRECISE (exactly the one changed package
     re-evaluated), but incremental wall ≈ no-trace — **no net win, not a net
     loss.** (The earlier "net-negative, full stop" used an `-O0` build AND a
     no-op delta; retracted.)

2''. **Cheaper per-trace recording (if 2' shows cross-commit value but cold cost
   blocks adoption).** The cold path serializes a fat dep blob + SQLite write per
   package trace (~3,000 for python3Packages); cold wall ≫ cold CPU ⇒ I/O-bound.
   Reducing per-trace recording cost (batch writes, compact dep encoding, or
   recording FEWER traces) is the lever for the deep-attrset shape — the opposite
   of Lever 1's finer granularity.

3. **The content-addressed trace-node RFC** (unblocks Lever 2 / Lever 5). A second
   `TracedExpr` identity keyed by content (derivation-input hash) rather than
   attr-path, with `navigateToReal` → re-invoke the producer. RFC-scale,
   hot-path, with the v53/vptr precedent warning. Only justified if (2) shows the
   derivation boundary is the dominant cost across real workloads, not just GNOME.

4. **Lever 3 (certificate-before-payload), low priority.** Targets hot, which is
   already flat — every sound form was refuted (see table). Not worth it absent a
   latency-sensitive small-query workload.

5. **Lever 4 (custom storage backend), deferred.** Downstream of authorization;
   does not address the cold tail (which is re-eval, not storage). Out of scope
   until 1–3 prove a proof-model win.

The keyset-downgrade harness work (landed) is the soundness scaffolding option 2
would build on. See "How the keyset work fits" below.

### Rejected / disqualified (do not re-propose without new evidence)

Command-only `nix eval --json` action cache (above libexpr); whole-output JSON
projections; v53 full-capsule rewrite (602 M state, measured regression);
unsafe by-name package-file drops; unsafe current-package dependency-output
boundary serving (served stale JSON on the 10-commit gate); exact-head memo
without proof; recomputing whole-repo Git identity per hot hit; raw/external-LZ4
capsule envelopes; fused load+verify; full-trace-hash verification memo;
re-abstracting the storage backend behind a vptr. (Sources: catalog.md
"Non-transferable", redesign-plan "Rejected experiment" sections.)

## How the keyset work fits

Lever 1 is split across two design docs and a soundness scaffold:

- **`plans/lever1-observed-key-pruning.md`** — the lever-1 design draft. Frames
  it as greenfield on HEAD (the v6→v11 machinery was removed with the
  command-JSON layer, 2026-05-27), names the two sub-levers (2a attrset `#keys`,
  2b by-name `DirectoryEntries`), and gates the build on the Step-1 diagnostic.
  The **by-name directory case (2b)** is the net-new surface here — no other doc
  covers it, and it is the likely driver of the cold outliers.
- **`plans/keyset-downgrade-sound-by-construction.md`** + **`…-differential-
  harness.md`** — the attrset-`#keys` sub-lever (2a) design + test harness
  (prototype deferred). Pruning a construction-only `#keys` observation is
  exactly the "prune coarse deps" move.
- **`src/libexpr-tests/eval-trace/store/keyset-escape.cc`** — pins the
  cross-trace soundness floor any prune must respect (a coarse dep can escape
  via `TraceValueContext`/`ParentSlot` to a consumer that doesn't exist at the
  producer's finalization). Lever 1's 2b case needs its own by-name escape test.

Lever 1's unsolved `attrNames` / negative-membership / recursive-attrset
coverage is the same complete-keyset authority gap tracked in those docs and in
OR-4.

## Discipline (applies to every lever)

From `findings.md` "Implementation Discipline" and the redesign-plan, unchanged:

1. Start each slice from baseline-equivalent `src`; one behavioral change at a time.
2. Build/check first; benchmark with **no debug and no `NIX_SHOW_STATS`**.
3. Use counters only to explain a result, then ablate their overhead.
4. Gate every change on the **10-commit correctness run** (byte-identical
   `nix eval` output vs `--no-eval-trace`) plus a no-debug benchmark.
5. Revert/archive anything that fails the gate before building on top of it.
6. ~~Establish a fresh baseline for the current tree before claiming any lever
   win~~ — **DONE 2026-05-30** (Ledger D in the redesign-plan, HEAD
   `9f7311129`). Future lever work compares against that anchor via
   `eval-trace-bench pairwise` on paired same-commit medians.

## Maintenance

When perf direction changes, update the redesign-plan (the living plan) and, if
the lever ordering or doc lineage changes, this index. Do not start a new
top-level perf doc; extend the redesign-plan and point this index at it.
