# Eval-trace Cache Findings

> **Provenance — read `eval-trace-cache-README.md` first.** This doc records
> durable conclusions from the **abandoned transparent-rewrite / command-JSON /
> v53 lineage** (Ledger A). The conclusions are valid and the baseline commit
> (`92a3df1ab`) is the current tree's base, but the experiment *code* this doc
> discusses is not in the tree, and its "back on origin src" framing is
> lineage-historical, not a description of HEAD. The authoritative forward
> ordering lives in the README's canonical lever list and in
> `eval-trace-cache-redesign-plan.md` (the 2026-05-29 CORRECTION supersedes the
> "Promising Directions" below where they conflict). Numbers here are per-commit
> **means** on `closures.gnome`; do not compare them against the redesign-plan's
> run **totals**.

This document extracts durable decisions from the raw work log. It is backed by
the more detailed experiment catalog in `eval-trace-cache-experiment-catalog.md`.
Routine command transcripts belong in `eval-trace-cache-work-log.md`.

## Executive Summary

The branch is currently back on the origin `src/` implementation because the
large transparent rewrite was slower than origin and much slower than the
historical pre-schema SQLite baseline. The useful output of the abandoned work
is not the v53 code; it is the benchmark harness, failure analysis, scoped
source/proof ideas, and a narrower target: transparent derivation-output replay
in `libexpr`.

The core lesson is that storage format changes alone are not enough. Hot
performance depends on avoiding repeated expensive proof/dependency validation
for a narrow demand, while cold performance depends on not inflating writeback
or materialization for source-specific facts.

## Benchmark Ledger

| Subject | Run | Cold | Hot | Outcome |
| --- | --- | ---: | ---: | --- |
| Historical pre-schema SQLite baseline | `938` | `~6.06s` mean | `~0.88s` mean | Target to beat. |
| Origin branch build | `cold/1`, `hot/1` | `59.31s` total, `5.93s` mean | `8.88s` total, `0.89s` mean | Current reference for cleaned branch. |
| Abandoned v53/capsule rewrite | `cold/2`, `hot/2` | `13.42s` mean | `1.69s` mean | Correct outputs, unacceptable performance. |
| Restored origin `src` after cleanup | `cold/3`, `hot/3` | `59.34s` total | `8.98s` total | Matches origin within noise. |
| Proof JSON action prototype | `cold/25`, `hot/25` | `3.132s` mean | `0.329s` mean | 100-commit win, but command-only. |
| Scoped-source action serving | `cold/163`, `hot/163` | `4.467s` mean | `0.237s` mean | Strong command-path win with scoped source proof. |
| Unsafe by-name drop oracle | `cold/88`, `hot/88` | `4.29s` mean | `0.37s` mean | Shows budget exists; not sound authority. |

## What We Know

- The active branch should not carry the v53/capsule rewrite. It is a measured
  regression, not just a messy implementation.
- The command JSON path can make the benchmark very fast by serving before
  normal evaluator consumers enter their ordinary demand path. That proves the
  benchmark demand is narrow, but it does not help `nix-eval-jobs` or other
  `libexpr` consumers.
- The repeated workload is centered on `closures.gnome.*.outPath`. In the
  rejected run, repeated candidate rows shared few result hashes and dep-set
  hashes but had many full payload hashes. The reusable fact is smaller than
  the full source-specific trace payload.
- SQLite writeback matters, but it was not the dominant remaining problem in
  the final rejected branch. Hot time was dominated by validation/loading shape;
  cold misses were dominated by lost near-hit behavior and oversized payloads.
- Counters and debug instrumentation can perturb wall time. Use them for
  explanation, then confirm with no-debug/no-stats runs.
- Several command/action-cache runs really did beat the baseline, but that data
  must be interpreted as proof-shape evidence. Those wins were not transparent
  to `libexpr`.
- Scoped source proof is one of the most useful abstractions discovered: it can
  replace global clean-worktree scans with proof over the exact guarded source
  facts. That idea may transfer.
- By-name package-file false negatives are a real performance lever, but the
  unsafe current-package/dependency-output boundary rule was falsified by a
  10-commit correctness run. Any future rule needs producer-side boundary or
  interface classification.

## Rejected Approaches

### Command JSON Action Cache

Rejected because it lived above `libexpr`. It could speed up `nix eval --json`
while leaving transparent consumers unchanged. The retained lesson is that a
whole-result demand can be cheap if the evaluator can prove exactly the demand
being served.

### Whole-Output JSON Projection

Rejected for the same transparency reason. A JSON projection is a command
output contract, not a general evaluator materialization contract.

### Standalone Authorized Projection Index

Rejected as an active implementation because it introduced storage/verifier
surface without a transparent `TracedExpr` or materialization caller. Positive
navigation remains plausible only when expressed as ordinary evaluator demand
and proof descriptors.

### Git/Native Generation Pack Backend

Rejected as implemented because it became a second durable backend algebra. It
could improve writeback constants, but it did not remove repeated hot proof
work. It should not return until the proof model already demonstrates a win.

### v53 Full Capsule Rewrite

Rejected because it inflated persistent state and lost origin's near-hit cold
behavior. Representative inspection:

- origin cache state: about `343M`, SQLite DB about `19M`
- v53 cache state: about `602M`, SQLite DB about `261M`
- v53 `TraceCapsules`: `61` rows totaling about `259M` payload bytes
- large rows repeated around `closures.gnome.{aarch64-linux,x86_64-linux}` and
  `.outPath`

## Promising Directions

### Transparent Derivation-Output Demand

The next useful implementation slice should target the evaluator fact the
benchmark actually asks for: derivation output path/string-with-context. It
must be usable by `libexpr` consumers, not just `nix eval --json`.

Open design questions:

- What is the smallest existing `TraceDemandDescriptor` shape that can name a
  derivation output path?
- Which existing deps already prove the output identity?
- What store-path availability facts must be checked before serving?
- Can the result be installed as a normal `TracedExpr` thunk so downstream
  consumers remain transparent?

### Compact Proof/Result Authority

The v53 failure suggests full trace payloads are too large and too precise for
common serving. A better design would route by candidate, authorize by proof,
and serve a compact result/dependency fact without decoding or validating a
large source-specific capsule on every hit.

### Scoped Source Proof

The command prototype showed that global clean-status checks were a dominant
hot cost. Scoped source proof served the same accepted-result sidecars after
checking exactly the source facts named by the guard, and rejected a deliberately
introduced untracked root child. This is worth carrying forward as a proof
abstraction, but it must be expressed below command JSON serving.

### Derivation Boundary Proof

Unsafe by-name package-file oracles showed large potential wins, but the
current-package dependency-output boundary rule served stale JSON on a broader
10-commit run. The next sound version needs evidence that owner construction
consumed the changed dependency only through drv/output identity and did not
observe arbitrary attrs, `meta`, `passthru`, shape, absence, or stringification
effects.

### Positive Navigation With Existing Proofs

Positive route navigation is still viable if each step has proof coverage for
parent membership and child path. Negative membership, absence, suggestions,
and `attrNames` should remain fallback-only until complete keyset authority is
available.

### Missing Store Dependency Fallback

If cached proof/result authority is valid but the required store dependencies
are not present, serving must fall back to evaluation and warn. Store closure
properties reduce some intermediate-GC concerns, but the top-level unavailable
dependency case still needs tests.

## Implementation Discipline

- Start each slice from origin-equivalent `src`.
- Make one behavioral change at a time.
- Run a focused build/check before benchmarking.
- Benchmark with no debug and no `NIX_SHOW_STATS`.
- Use stats/counters only to explain a result, then ablate their overhead.
- Record benchmark provenance: subject store path, binary hash, commit window,
  exact command, output-match status, cold/hot totals, and pairwise deltas.
- Revert or archive any implementation that fails the 10-commit gate before
  building more structure on top of it.

## Current Action Items

1. Restore or write tests for missing store dependencies causing warning plus
   fallback when a cached result is otherwise available.
2. Specify the derivation-output demand in existing descriptor vocabulary.
3. Identify the smallest proof obligation that authorizes serving an `outPath`.
4. Decide whether scoped source proof can be reused as a generic proof
   descriptor instead of command-action cache logic.
5. Prototype that path transparently in `libexpr`.
6. Run the 10-commit gate against origin and stop if it does not move hot/cold
   in the right direction.
