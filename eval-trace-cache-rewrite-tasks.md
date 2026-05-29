# Eval-trace Cache Work Plan

This is the actionable index. The raw chronological work log was moved to
`doc/eval-trace-cache-work-log.md`.

## Current State

- Branch source is intentionally reset to
  `origin/vibe-coding/file-based-eval-cache` at
  `92a3df1ab706cf514357ae57b367050b373ff146`.
- `src/` should remain empty relative to origin until the next isolated,
  benchmark-gated implementation slice.
- The only current branch delta is benchmark harness, docs, and the functional
  harness tweak committed as `43c08ead1`.
- Restored-source benchmark matched origin within noise:
  - origin `cold/1`: `59.31s` total, mean `5.93s`
  - restored `cold/3`: `59.34s` total, mean ratio `1.004x`
  - origin `hot/1`: `8.88s` total, mean `0.89s`
  - restored `hot/3`: `8.98s` total, mean ratio `1.011x`
- The abandoned v53/capsule rewrite was correct on the 10-commit run but too
  slow:
  - `cold/2`: mean `13.42s`
  - `hot/2`: mean `1.69s`
  - all outputs matched reference

## Goal

Implement transparent eval caching that benefits ordinary `libexpr` consumers
such as `nix-eval-jobs`, while beating the established pre-schema SQLite
baseline:

- cold/938: about `6.06s` mean wall time
- hot/938: about `0.88s` mean wall time

Do not optimize a command-only `nix eval --json` path and count it as success.
That path is useful demand-shape evidence, but it does not solve transparent
evaluation.

## Soundness Invariants

- Cache acceptance is proof authorization:
  `Verify(proof, current_world, demand) == true`.
- Storage indexes route candidates. They are not semantic authority.
- Sessions are telemetry and batching scopes. They do not validate hits.
- Git revisions, paths, DB rows, source hints, and cache keys are locators until
  a proof descriptor makes them authority.
- Positive membership, absence, `attrNames`, missing-attr errors, and
  suggestions must not use cached shape data unless the relevant shape/keyset is
  authorized.
- Missing store dependencies must force fallback to evaluation, with a warning
  when a cached result is otherwise available.
- Benchmark gate runs must be no-debug and no-`NIX_SHOW_STATS`; counters require
  explicit ablation because they can move wall time.

## Retained Evidence

- Raw chronology: `doc/eval-trace-cache-work-log.md`
- Distilled findings: `doc/eval-trace-cache-findings.md`
- Experiment catalog: `doc/eval-trace-cache-experiment-catalog.md`
- Storage/writeback findings: `doc/eval-trace-sqlite-writeback-findings.md`
- Backend/design research: `doc/eval-trace-storage-backend-research.md`
- Larger redesign notes: `doc/eval-trace-cache-redesign-plan.md`

## Rejected Directions

- Command-level JSON action cache: fast, but not transparent to `libexpr`
  consumers.
- Whole-output JSON projection API in `TraceSession`: same transparency
  problem.
- Standalone authorized projection index: storage/verifier surface without a
  transparent evaluator-level caller.
- Git/native generation pack backend as implemented: extra backend algebra that
  did not solve hot validation.
- v53 full capsule rewrite: inflated persistent payloads and lost origin's cold
  near-hit behavior.
- Serving by candidate route, source key, or recovery key without proof
  authorization: unsound.
- Unsafe dependency-output/current-package serving: performance-relevant, but
  falsified by stale JSON on a broader 10-commit correctness run.

## Promising Leads

1. **First-class evaluator demand for derivation output path**

   The workload is mostly computing `outPath` on derivations. The useful shape
   from the rejected JSON action path should be re-expressed as an ordinary
   `libexpr` demand for a derivation output path/string-with-context, not as
   command JSON serving.

2. **Compact proof/result authority instead of source-specific full capsules**

   The rejected v53 run recorded repeated `closures.gnome.*.outPath` facts with
   few distinct result hashes and dep-set hashes but many full payload hashes.
   That suggests separating reusable result/dependency authority from
   source-specific full capsules.

3. **Positive navigation only after proof coverage**

   Lazy positive route navigation may still help, but only if expressed through
   existing demand/proof descriptors and installed as normal evaluator thunks.
   Do not add cached absence or suggestions until complete keyset authority is
   proven.

4. **Store-dependency availability fallback**

   If proof and result authority exist but required store dependencies are
   missing, the evaluator must fall back and warn. This is a soundness/UX
   requirement independent of the performance path.

5. **Scoped source proof**

   The command prototype showed that global clean-worktree checks dominated hot
   time. Scoped source proof served after validating exactly the guard facts and
   rejected injected untracked source changes. The useful abstraction is a
   generic current-source proof descriptor, not command-cache routing.

6. **Derivation boundary classification**

   Unsafe by-name package-file oracles showed enough performance headroom to
   beat the baseline, but the naive current-package/dependency-output rule was
   unsound. A future implementation needs producer-side evidence that a changed
   dependency was consumed only through drv/output identity.

## Next Tasks

- [ ] Keep `src/` identical to origin until choosing one small implementation
  slice.
- [ ] Regenerate a clean benchmark reference when needed using:
  `nix build -L --builders '' ".?rev=92a3df1ab706cf514357ae57b367050b373ff146"`
  followed by `eval-trace-bench generate`.
- [ ] Add or restore tests for missing store dependencies causing warning plus
  fallback when a cached result is otherwise available.
- [ ] Design the minimal transparent `libexpr` demand for derivation output
  path/string-with-context.
- [ ] Before implementing, define the exact proof obligation and what existing
  descriptors/deps/results already provide.
- [ ] Map scoped source proof and store-context availability checks into that
  proof obligation if they are needed for serving.
- [ ] Treat derivation-boundary/package-file discharge as a separate proof
  project; do not reuse unsafe by-name or current-package oracles.
- [ ] Prototype only that demand path, then run the 10-commit no-debug/no-stats
  gate against origin.
- [ ] Stop if hot does not move toward or below `0.88s`, cold materially
  regresses from `6.06s`, outputs diverge, or the implementation requires broad
  new cache authority.

## Benchmark Rules

- Use origin run 0 or 1 as the reference after regenerating from a clean result
  symlink.
- Use the same 10-commit window when comparing slices.
- Run wall-only first. Stats/counters are diagnostic runs, not benchmark gates.
- Record subject binary/store path, `result/bin/nix` hash, command line, output
  match status, cold/hot totals, and pairwise deltas.
- Treat wins under about 1% as noise unless repeated.
