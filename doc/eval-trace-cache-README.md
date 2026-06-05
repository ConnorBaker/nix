# Eval-trace cache performance research — consolidated index

> **⛔ STATUS BANNER (2026-06-02) — READ BEFORE THE REST OF THIS FILE.**
>
> **0. MEASURED 2026-06-02 — the entire arc below was benched on closures.gnome
> (7 traces) ONLY. First off-closures bench (python3Packages, ~66K traces): §3b
> WARM-THRASHES at scale — never converges, 6–21× slower than no-§3b, sound,
> mechanism unrooted; SCALE-dependent (stable ≤~8K traces). Data:
> `doc/eval-trace/measurements/3b-at-scale-2026-06-02.md`. So the closures-derived
> verdict (items 1–2 below) does NOT generalize; treat it as closures-only.**
>
> **0b. MEASURED 2026-06-03 (adversarially re-reviewed) — cold tax is eval-trace's
> per-attr work at scale, recording-dominated; GC is NOT it; hot is NOT regressed
> (canonical bench 0.34–0.9s).** Cold python3Packages: no-trace 19.1s → eval-trace 66.9s (+47s).
> Clean split via the CORRECT gate (STUB_RECORD: keep materialize, stub only the
> record pipeline): **capture+materialize +13.2s / record-pipeline +34.2s** (of
> ~47s); memory is also record-dominated (eval-trace cold 4.4GB vs no-trace 1.4GB,
> +3GB). **GC is not the tax** (7→2 GC cycles leaves it unchanged; "libgc +30.9s"
> was marker spin-wait — RETRACTED). **LANDED: `eval-trace-defer-flush` setting
> (default true)** — batches per-record flushes (Layer-2a, crash-safe): −8s cold
> (58.6 vs 66.6s), byte-identical (python3Packages + closures.gnome + asciidoc
> cold/warm), unit tests pass; +376MB at 32K-record scale (disable for huge cold
> evals). **Two earlier claims were withdrawn**: (a) "capture=+1.0s/record=+46.9s"
> was a CONFOUND (NO_RECORD also skipped `materializeResult`) — corrected to the
> +13.2/+34.2 split above; (b) "record cpu ~5–6s" is inconsistent with GC≈0.3s. **Hot is
> NOT regressed**: canonical `eval-trace-bench` `closures.gnome` HOT = 0.9s (bench) /
> 0.34s (raw), matching the 0.85s baseline; code unchanged (f1398d126). My earlier
> "hot 3.7s" was python3Packages (32K traces / 18.7M deps) — a ~4600×-larger workload
> I did NOT run through the bench. Hot is O(total deps verified/run); 3.7s is the
> large-workload point, not a regression. Full corrected decomposition + methodology:
> `doc/eval-trace/measurements/cold-tax-decomposition-2026-06-03.md`.
>
> Two things below this banner are STALE and have caused a duplicated effort:
> 1. **The producer-partition / CA-producer / compositional-trace-DAG /
>    "content-addressed trace identity" / re-keying direction is UNPROMISING —
>    but NOT cleanly "structurally dead."** Chain:
>    `plans/derivation-producer-partition-rfc.md` §16–§18 +
>    `plans/compositional-trace-dag-implementation-sketch.md` §10/§10.7. The
>    §3b "3.6× cold / 14× hot" numbers quoted throughout this file are
>    **pre-Fix-1a** (commit `82713fd91`); Fix 1a (`868038369`) made conservative
>    §3b hot-neutral and showed the 14× was 93% a cache-defeat bug. **Not pursued
>    because the measured *proxy* leans negative — "don't invest," not "proven to
>    lose":** post-Fix-1a, conservative §3b is hot-neutral (1.09s ≈ baseline — NO
>    win) and the as-built **mis-keyed** aggressive edge is hot-negative (1.81s vs
>    1.09s, loading ~12K separate producer-trace blobs that inline conservative
>    avoids; capped per producer, so re-keying wouldn't reduce them — projected,
>    not benched). **OSPI's own re-keyed shape was never benched.** **CAUTION:**
>    §17b/§18's "97% singly-consumed
>    → no sharing → structural catch-22" is **confounded** (it reads the mis-keyed
>    gate's 306 fires as fan-out, contradicting §11's 200–2000 estimate; the
>    correctly-keyed shape was never benched — §10.7). (§3b also adds a cold cost,
>    but the "+6.7s recording" figure once cited here is STALE — the live perf doc
>    re-measured store-write at 0.2s; cold is the always-on tax + dep-capture, not
>    recording. Don't cite +6.7s.) **Do not pursue Option 3 / lever 2 / the
>    "Architectural root cause" CA-edge prescription / re-keying below**, and do
>    not chase a high-fan-out workload on the confounded reason. A 2026-06-02 pass
>    re-derived this as "OSPI" and retracted it (§10).
> 2. **The LIVE perf direction is `plans/eval-trace-perf-cold-and-hot.md`** (cold
>    recording cost + hot verification), orthogonal to producer-partition. HOT
>    levers landed (persisted content-hash cache H1; HOT-1 posix tier now
>    **DEFAULT-OFF** 2026-06-04 — coarse-mtime stat-race soundness bug, see
>    `doc/eval-trace/measurements/property-test-overinvalidation-2026-06-04.md`;
>    H1 store-path tier unaffected/sound; productionize-(b), #2 git-clean marker,
>    Spike-1 sync-verify). COLD is MEASURED at ~57% BLAKE3
>    hashing of the 607×-flattened dep vectors / ~4% I/O — so "async recording"
>    is low-leverage and off-thread hashing is thread-safety-blocked; H-cold-1
>    landed (−10%). The doc's named next cold lever is **C3, reduce the
>    flattened dep count via record-time dedup** (unexplored; the 607× root
>    again, needs fragment-hash composition which the positional `dep.ordinal`
>    blocks). Read its COLD section before acting; do not start blind.
>
> The ledger reconciliation, discipline, and "rejected/disqualified" sections
> below remain valid. The lever table and Options are valid EXCEPT where they
> point at the CA-edge/re-keying direction (struck by item 1).

**Read this first.** The eval-trace cache performance work is spread across six
documents written over a long research arc. They use **three different
benchmark ledgers** and describe **two abandoned experiment lineages** plus one
living plan. Landing on any single doc without this map has already caused one
reader (and one assistant) to conflate lineages and quote the wrong numbers.
This file is the authoritative entry point: it says which doc describes the
current tree, reconciles the ledgers, and carries the canonical lever list.

## Picking this up — start here (2026-05-31)

If you're a new agent or contributor inheriting this branch, this is the
shortest path to context.

**Branch state.** `vibe-coding/file-based-eval-cache`, ~17 commits ahead of
origin since the §3b implementation slice. Tree clean. Run `git log --oneline
master..HEAD | head -25` for the chronological story.

**The active work (CORRECTED 2026-06-02).** RFC §3b
(content-addressed producer-trace boundary) is **implemented, fully measured,
and a CONCLUDED DEAD END** — see the status banner at the top of this file and
`plans/derivation-producer-partition-rfc.md` §16–§18. The "3.6× cold / 14× hot"
figure repeated throughout this file is **pre-Fix-1a**; Fix 1a (`868038369`)
made conservative §3b hot-neutral and proved the 14× was 93% a cache-defeat bug,
and the post-Fix-1a re-bench showed the *edge* direction is inherently
hot-negative and structurally blocked. §3b stays in tree DEFAULT-OFF behind
`NIX_ENABLE_CA_PRODUCER=1` as gated scaffolding; **do not invest in re-enabling
or re-keying it.** The live work is `plans/eval-trace-perf-cold-and-hot.md` —
HOT levers landed (persisted content-hash cache); COLD is measured at ~57%
BLAKE3 hashing of the flattened deps (not I/O), H-cold-1 landed (−10%), and the
named next lever is C3 (reduce the flattened dep count) — read that doc's COLD
section before acting.

**Where production code lives.** `src/libexpr/eval-trace/CLAUDE.md` has a
"RFC §3b" section near the top describing the code surface
(`producerMap`/`producerBloom`/`recordSync`/`recordCAProducer`/the gate),
the bench outcome, and the conditions for re-enabling.

**Where tests live.** `src/libexpr-tests/eval-trace/CLAUDE.md` indexes the 28
§3b-related tests (4 test files in `dep/` + 2 in `store/`). All probe-verified.

**Reproducing the bench.** From this directory:
```bash
nix build -L .                                           # build result/bin/nix
NIX_CONFIG="builders =" nix run .#eval-trace-bench -- generate \
  --nix . --nixpkgs ~/ext-sources/nixpkgs \
  --num-commits 100 --run-number 3                       # generate run-3
nix run .#eval-trace-bench -- runs \
  --runs reference,cold/1,hot/1,cold/3,hot/3 \
  --reference reference --allow-provenance-mismatch      # compare
```
Run-1 is the pre-§3b baseline (2026-05-30). Run-2 is the post-§3b
measurement that drove the default-off decision. Pick a fresh run-number
≥ 3. Both run dirs and analysis tools document themselves via `--help`.

To toggle §3b on for a specific eval:
```bash
NIX_ENABLE_CA_PRODUCER=1 result/bin/nix eval -f ~/nixpkgs/default.nix \
  --system x86_64-linux asciidoc.outPath
NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH=/tmp/s.json $abovecmd
python3 -c 'import json; print(json.load(open("/tmp/s.json"))["evalTrace"]["replay"])'
```
The `producerEdges` counter under `evalTrace.replay` shows gate fire count.

**Decision tree for what to do next** — pick one based on what's true:

1. **"§3b is dead weight, rip it out."** Defensible if you accept the bench
   verdict and don't want code paths gated on env vars in production. The
   cleanup is mechanical: revert commits `13cf303d2`, `82713fd91` (the hook
   in `prim_derivationStrict`), `baa35bbf9` (`recordCAProducer` +
   `recordSync`), `9241013bc` (the gate in `replayMemoizedDeps`),
   `70c4591cc` (the `producerMap` API), and clean up the tests in
   `dep/{producer-side-table,replay-producer-gate,ca-producer-scope,
   trace-session-record-ca-producer}.cc`. Keep the consume-side tests
   (`store/ca-trace-key-routing.cc`, `store/ca-producer-boundary-recording.cc`)
   as a soundness floor for any future shape — they're scope-level synthetic
   and don't depend on the production hook.

2. **"§3b's idea is right; the cost shape is wrong — try (1)/(2)/(3) below."**
   Defensible if you believe a future cost reduction unlocks the win. The RFC
   §8 + production CLAUDE.md "RFC §3b" section both list:
   - **(1) Async/batched producer recording** — `recordSync` runs synchronously
     per derivation. Batching at session end (or running on a background
     thread with `traceId` reconciliation deferred) collapses ~6,419 SQLite
     transactions into one. Lowest-risk improvement.
   - **(2) Suppress on warm-served derivations** — when the consumer's
     `TracedExpr` is serving from cache, derivation thunks inside re-run as
     part of `materializeResult`. The hook firing here costs 100% (every
     record) for 0% benefit (the consumer isn't recording a new trace). A
     thread-local flag set by `materializeResult` and read by the hook would
     skip cleanly. Higher-impact.
   - **(3) Measure on a sibling-share-heavy workload** — `closures.gnome` is
     deep-computation, shallow-data; the gate fires only ~614 times/commit.
     `nix-eval-jobs`-style deep attrset enumeration may exercise the gate
     much more heavily. Run the bench on `python3Packages` outPaths (see
     `MEMORY.md` / `project_eval_trace_perf_baseline_lever5.md` for the
     workload definition).

3. **"§3b is the wrong abstraction — different design."** Defensible if you
   believe the underlying flattening (607×) needs a different fix entirely.
   Read `plans/architecture-trace-model-vs-CA.md` (the architectural
   diagnosis) + `plans/compositional-trace-dag-design.md` (the broader
   compositional-trace-DAG direction the RFC was a slice of). The
   prerequisite for any of these is **the keyset-provenance soundness
   floor** (`plans/keyset-downgrade-sound-by-construction.md`,
   `store/keyset-escape.cc`).

4. **"Stay where we are; switch focus."** Defensible: §3b is dormant +
   correctness-safe, the broader cache is solid (1844/1847 unit tests pass,
   byte-identical eval). The 23 cold outliers in run-1 are the natural next
   target if perf is the goal — they're not §3b-related; see the
   redesign-plan's outlier diagnosis and the lever 1 / 2 / 3 / 4 ranking
   below.

**Reading order** for a new contributor unfamiliar with this whole arc:
1. This README (you are here) — full lever list + reconciliation.
2. `src/libexpr/eval-trace/CLAUDE.md` "Architecture Overview" + "RFC §3b" —
   what production code looks like.
3. `doc/eval-trace-cache-redesign-plan.md` — chronological synthesis,
   including the 2026-05-31 follow-up #4 with bench table.
4. `plans/content-addressed-trace-identity-rfc.md` — the §3b proposal +
   its §8 outcome.
5. Specific test files when looking at specific behaviors.

`MEMORY.md` (in `~/.claude/projects/.../memory/`) has session-spanning
context if you're using Claude Code with the same user; otherwise the
in-tree docs above are self-contained.

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

### `plans/` docs (not in the table above — index added 2026-06-02)

| Doc | Role | Status |
|---|---|---|
| **`plans/eval-trace-perf-cold-and-hot.md`** | **THE LIVE perf doc.** Cold recording + hot verify avenues, grounded in code + the §17 bench. | **CURRENT — start here for perf work** |
| `plans/derivation-producer-partition-rfc.md` | The producer-partition arc, §9→§18. §16 (Fix 1a re-bench: conservative hot-neutral); §17 (as-built edge hot-negative via loadTrace); §17b/§18 ("no sharing / catch-22" — CONFOUNDED, see sketch §10.7). | CONCLUDED UNPROMISING (measured hot non-wins); NOT cleanly "structurally dead" |
| `plans/content-addressed-trace-identity-rfc.md` | The §3b CA-producer proposal (§8 = pre-Fix-1a net-loss). | Superseded by the §16–§18 conclusion |
| `plans/compositional-trace-dag-design.md` | The 607× → edge architectural prescription. | Diagnosis sound; "edge fixes it" refuted by §17b (leaf-dep ≠ producer-consumer sharing) |
| `plans/compositional-trace-dag-implementation-sketch.md` | 2026-06-02 second-pass re-derivation ("OSPI") + 3 adversarial reviews + **§10 retraction**. | RETRACTED — the record of the duplicated effort |
| `plans/architecture-trace-model-vs-CA.md` | The flatten-vs-CA-edge diagnosis. | Diagnosis valid; prescription dead (see above) |
| `plans/async-producer-recording-plan.md` | Async/batched recording (Layer 1/2a landed, 2b deferred). | Layer mechanism is reusable for the live cold lever |
| `plans/perf-levers-cold-and-hot.md`, `plans/lever1-observed-key-pruning.md`, `plans/lever2-derivation-boundary-caching.md`, `plans/keyset-*.md` | Lever-specific designs/harnesses (see lever table). | Mixed; lever 2 (derivation boundary) is dead per §16–§18 |

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
| **2** | **Semantic derivation-boundary caching** (Bazel/Skyframe strict dependency capture + facet mask) | **NOT VIABLE as scoped — feasibility spike hit a material architectural blocker (2026-05-30).** Outliers re-eval ~20 M thunks; the 6,419 derivations inside (99.66 % unchanged) have no reuse boundary, ~58 % of cost is cacheable derivation eval. Spike (branch `spike/lever2-derivation-feasibility`) measured 10,455 derivations evaluated vs 6 traces recorded, then hit the blocker: to verify-before-force a derivation it must be a `TracedExpr`, but `TracedExpr` identity is **attr-path-tree-shaped** (Root or Child-with-parent-and-name), and a `derivationStrict` thunk created deep in `make-derivation.nix` has no attr-path from the eval root — `makeChild` cannot construct it. Caching derivations needs a **second content-addressed `TracedExpr` identity model** threaded through evaluator thunk creation = foundational redesign on the hot path, RFC-scale, not a lever. Verdict: leave the cold tail bounded + sound. Full chain: `plans/lever2-derivation-boundary-caching.md` §7-11. **UPDATE 2026-05-31 (numbers STALE — pre-Fix-1a; see the top status banner + §16–§18 + sketch §10.7): the §3b producer-trace boundary IS implemented + benchmarked. The often-quoted "cold 3.6× / hot 14× slower" is PRE-Fix-1a — Fix 1a (`868038369`) made conservative §3b hot-neutral and showed the 14× was 93% a since-fixed cache-defeat bug. Post-Fix-1a verdict: UNPROMISING (the measured hot non-wins — conservative neutral, aggressive negative; the "+6.7s cold recording" figure once cited is STALE, see §10.7), not "structurally dead" (the "no sharing" reason is confounded). Ships DEFAULT-OFF behind `NIX_ENABLE_CA_PRODUCER=1`. Soundness PASS.** | redesign-plan §2026-05-30 spike + §2026-05-31 follow-up #4 bench; findings.md "Derivation Boundary Proof" |
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

2''. **Make cold AND hot faster — see `plans/perf-levers-cold-and-hot.md`
   (REORDERED after a 6-lever deep-dive + independent file:line verification).**
   Root cause (DB-confirmed): 36.5 M deps recorded from only ~60 K distinct atoms
   ⇒ each shared-closure dep (stdenv/glibc/python) recorded & re-hashed **~607×**.
   **Verified verdicts (most prior "cheap wins" demoted):** L-C (fuse 3 hashes) =
   ~30-45% of `record.hashUs` only, not 2/3, + needs a new multi-sink builder
   (the 3 hashes cover different dep subsets); **L-B REFUTED** (the L1 cache
   already memoizes distinct deps once per shared session; per-dep residual is
   already a hashmap find + compare); **L-A** = schema-epoch RFC with two blockers
   (global sort + positional `dep.ordinal` prevent fragment-hash composition);
   **L-D REFUTED here** (bimodal but lopsided — the cheap ~30% of traces hold 0.2%
   of deps); **L-E** real but small (`flush` is per-trace but `synchronous=off` ⇒
   no fsync; bounded by ~3.8s); **L-F** reframed (the 3× came from the workload
   expr's `attrNames`+select, not the printer — guidance applies to access idioms
   anywhere in the evaluated Nixpkgs tree). **No cheap high-impact lever exists for
   the cold cost.** The clearest actionable items: **L-F** (zero-code consumer
   guidance) and **attributing the unmeasured ~3.6s of hot `verify.timeUs`**
   (coroBlock / `withExclusiveAccess` hops + `loadFullTrace` blob deserialize) —
   that, not L-B, is the real hot opportunity, and it needs a `runs --verbose`
   measurement first.

3. **The content-addressed trace-node RFC — IMPLEMENTED, then FULLY MEASURED +
   CONCLUDED (`plans/content-addressed-trace-identity-rfc.md`).**
   > **⚠ THIS WHOLE OPTION IS SUPERSEDED — see the top status banner +
   > `plans/derivation-producer-partition-rfc.md` §16–§18 + sketch §10.7.** The
   > "§7 measurement / net loss / 3.6×/14×" numbers below are **PRE-Fix-1a**;
   > Fix 1a (`868038369`) made conservative §3b hot-neutral (the 14× was 93% a
   > cache-defeat bug). The "re-enable when the cost profile changes —
   > async/batched recording / suppress-on-warm / sibling-share workloads"
   > conditions below are **all refuted** (§5 falsified sibling-share; §16
   > neutralized hot; the re-key that would fire the gate is unpromising + the
   > "no sharing" reason is confounded). Net: don't re-enable, don't re-key — the
   > measured proxy (mis-keyed aggressive) is hot-negative, conservative gives no
   > win, and §3b adds a cold cost; that justifies "don't invest", not "proven to
   > lose" (OSPI's re-keyed shape was never benched). (The "+6.7s recording" figure
   > once cited is STALE — store-write is 0.2s; §10.7.)
   > The bullets below are kept as the implementation record only.
   The infrastructure
   shipped in tree, gated on `NIX_ENABLE_CA_PRODUCER=1`.
   **State of the proof (pre-Fix-1a framing — see marker above):**
   - **Consume side DONE** (`store/ca-trace-key-routing.cc`, 3 tests): Design A (a
     synthetic `"__ca:<drvHash>"` vocab key via `internName`) round-trips through
     the existing pipeline.
   - **The edge must be a trace-hash edge, not the existing SPA dep**
     (`store/derivation-input-flattening.cc`, 2 tests): SPA is an inert existence
     check; load-bearing carrier is the flattened `FileBytes`.
   - **Producer side WIRED into `prim_derivationStrict`** with synchronous
     `TraceSession::recordCAProducer` API (mirrors `recordRuntimeRoot`'s shape — no
     `EvalContext<Suspendable>` threading, no C-extension API breakage). Producer
     keyed by `Bindings *` (stable across the `vRes = vCur` copy in `callFunction`;
     `Value *` was tried first and gave `producerEdges = 0` always). Bloom-fast-
     rejected lookup on the hot path. 22 production tests (all probe-verified).
   - **§7 measurement DONE — net loss on closures.gnome.** Bench (Ledger-D anchor,
     100 commits): cold 3.72s → 13.44s (3.6× slower); hot 0.96s → 13.47s (14× slower;
     cache mostly bypassed because derivation thunks re-run during materialize and
     trigger the hook even on warm-verify). Soundness PASS (byte-identical eval).
     The conservative shape's cost (~6,419 producer trace records/commit) far
     exceeds its benefit (~614 gate fires/commit).
   - **Decision: default-OFF via `NIX_ENABLE_CA_PRODUCER=1`.** The infrastructure
     stays in tree as proven scaffolding. Re-enable becomes attractive when the
     cost profile changes — async/batched producer recording, suppress-on-warm-
     verify-served, or sibling-share-heavy workloads (`nix-eval-jobs`-style)
     where the gate fires frequently enough to amortize. Full bench detail +
     adversarial-fix history: redesign-plan "2026-05-31 follow-up #4". Production
     code documented in `src/libexpr/eval-trace/CLAUDE.md` "RFC §3b" section.

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

## Architectural root cause (2026-05-30) — `plans/architecture-trace-model-vs-CA.md`

The deepest finding of the whole arc, and it unifies the others. Verified
against code + the cold DB:

- **eval-trace flattens the transitive dep closure into every trace node**
  (`replayMemoizedRange` copies a child's full dep range into the parent,
  dep-recording-context.hh:307). A trace stores ~95 % leaf observations (decoded
  dep-kind histogram: StructProj 48 % + StorePathAvail 35 % + FileBytes 7 % + …),
  avg 3,376 deps/trace = a transitive closure, not direct deps. The cross-trace
  EDGE dep kinds (`TraceValueContext`/`ParentSlot`) are **~0 % used**.
- **The build layer does the opposite:** a derivation references its inputs **by
  hash/edge** (`Derivation::inputDrvs` keyed by input drvPath), and
  `hashDerivationModulo` recursively substitutes input-drv hashes **memoized in
  `drvHashes`** — shared stdenv is hashed once and referenced N times. That is
  the short-circuit eval-trace lacks: cold re-hashes / hot re-walks the shared
  closure ~607× instead of referencing one shared sub-result.
- **The unifying root cause:** a `TracedExpr` (hence a hash-identified trace) is
  created ONLY for attr-path-addressable values (root + attrset/list children in
  `materialize.cc`). Shared closure reached by function application / `import`
  has **no trace identity to edge-reference**, so it gets flattened. This SAME
  constraint blocks Lever 2 (derivations aren't attr-path-addressable) and leaves
  the (fully-built, memoized) edge-verify machinery `resolveTraceContextHash`
  unused. **eval-trace ties identity to syntactic position in the output tree;
  the build layer ties it to content of inputs.** That is the architectural gap.
- **Concrete design:** `plans/compositional-trace-dag-design.md` — what to
  capture and why it's tractable (you do NOT hash every value; you edge to the
  ~thousands of boundary-eligible nodes that already have stable identities) — and
  its productized form `plans/content-addressed-trace-identity-rfc.md` (the RFC,
  with the consume side now proven by `store/ca-trace-key-routing.cc`; see Option 3).
- **Direction — IMPLEMENTED + MEASURED + CONCLUDED UNPROMISING (see top banner).**
  The "lift the build layer's CA model to evaluation — edge, not flatten"
  prescription was built (RFC §3b) and benchmarked post-Fix-1a: the as-built edge
  is hot-NEGATIVE (a separate-trace `loadTrace` on warm verify costs more than
  inline deps, `…rfc.md` §17), and conservative §3b gives no hot win — the
  measured non-wins are the reason. **The diagnosis above (the 607× flattening, the build-layer contrast)
  is sound; the "edge fixes it" prescription does NOT pay off** on closures.gnome.
  (`…rfc.md` §17b/§18's "no sharing" *reason* is confounded — sketch §10.7 — but
  the directional outcome stands.) Do not pursue this as a live lever.
  Soundness prerequisite = the keyset-provenance / cross-trace-escape work (a node
  can observe a child's shape, not just value — unlike builds).

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
