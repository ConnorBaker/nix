# Eval-trace Cache Experiment Catalog

This is a decision-level extraction from `eval-trace-cache-work-log.md`. It is
not a replacement for the raw log; it captures benchmark-significant results,
rejected paths, and ideas that may transfer to the next implementation slice.

## Baseline and Harness Facts

- `92a3df1ab706cf514357ae57b367050b373ff146` is the baseline source commit used
  throughout the work.
- Early 100-commit regenerated baseline `906` showed:
  - `cold/906`: `599` hits, `101` misses, `4.036s` mean wall with stats/debug
    style reporting.
  - `hot/906`: `700` hits, `0` misses, `1.131s` mean wall.
- Later no-debug/no-stats fast baseline `938` became the main 10-commit target:
  - `cold/938`: about `6.05894s` mean wall.
  - `hot/938`: about `0.880468s` mean wall.
- After result deletion and harness hardening, baseline run `0` was regenerated:
  - 100 commits: `cold/0` `3.316277s`, `hot/0` `0.886366s`.
  - first-10 compatible baseline later measured `cold/0` `6.643640s`,
    `hot/0` `1.156355s`.
- Harness hardening was not incidental. It fixed run identity checks, positive
  finite `wallTime`, baseline-run expansion, pairwise zero-pair handling,
  stateful-prefix comparisons, source provenance normalization, and immutable
  binary identity comparison.
- Counters and stats runs can materially change wall time. Treat stats as
  causal diagnosis only; gate claims with no-debug/no-`NIX_SHOW_STATS` runs.

## Transparent `libexpr` Rewrite Findings

### 906-930: Corrected Baseline and Early Regressions

- Run `907` was sound but much slower than `906`: cold hit rate dropped from
  `85.6%` to `78.7%`, cold wall rose from `4.04s` to `8.59s`, and hot wall
  rose from `1.13s` to `1.40s`.
- Run `908` loaded-deps reuse helped hot wall (`1.399s -> 1.181s`) and reduced
  hot `loadTrace.count`, but CPU stayed high.
- Run `909` moved verified-session reuse before `loadFullTrace`, restoring hot
  `loadTrace.count` to baseline-like `7`, but wall/CPU stayed high.
- Run `910` reduced result decode calls but made hot slower. Result decode count
  was not the decisive bottleneck.
- Git reidentity in run `912` recovered cold hit rate (`602/700`, better than
  baseline `599/700`) but made recovery expensive (`95.887s` aggregate
  recovery versus baseline `15.766s`).
- Key-set grouping in runs `916`/`917` reduced structural-variant candidate
  loads (`1055 -> 485`) and recovery time, but still did not beat baseline.
- Larger probes, full dependency probes, result-id reuse, source identity
  shortcuts, and complete-probe preflight were mostly rejected because they
  moved work rather than deleting it.

Durable lesson: cold regression was not generic SQLite writeback. It was a
semantic recovery/source-proof problem plus expensive candidate validation. Hot
regression remained fixed overhead in the verifier/materialization path after
obvious duplicate trace loads were removed.

### 938-1049: Exact-session Proof and Direct-serving Work

- Runs `945`, `961`, and `973` showed recomputing whole-repo Git identity inside
  hot verification is too expensive. It can turn hot into `1.6s-2.1s`.
- Runs `963`, `967`, and similar fused verifier experiments worsened cold badly
  by serializing work behind broad storage/verifier locks.
- Candidate-row session/recovery indexes in `1030` helped cold variance
  (`6.29s -> 6.15s`) but did not move hot.
- TraceContext exact-session proof in `1038` was semantically useful and
  improved hot slightly (`0.92s -> 0.90s`) but still missed baseline.
- DerivedStorePath exact-session coverage in `1045` removed a coverage gap and
  tied hot (`0.89s` versus `0.88s`) but cold stayed poor (`6.65s`).
- Conservative v1 exact-hit sidecar export in `1049` created a small hot win
  (`0.87s` versus `0.88s`) without fixing cold (`6.62s`).

Durable lesson: isolated exact-session proof broadening can make hot close to
baseline, but broad descriptor publication or per-dep checks move too much work
to cold. A real win needs compact finalist authority consumed before full trace
load, not more general proof reconstruction.

### 1056-1062: Retained Micro-wins

- Run `1056` pointer-sort materialization in `sortAndDedupDeps` was the largest
  local win in that slice: `cold/1056 = 6.128370s`, `hot/1056 = 0.769031s`.
- Run `1057` moved the already-sorted dep vector into storage publication:
  `cold/1057 = 6.137080s`, `hot/1057 = 0.765580s`.
- Run `1061` kept dirty-index scan cleanup: `cold/1061 = 6.124750s`,
  `hot/1061 = 0.770298s`.
- Run `1062` specialized empty-probe encoding for large capsules:
  `cold/1062 = 6.114380s`, `hot/1062 = 0.769314s`.
- Uncompressed capsules, fd/writeFull payload writer, grouped payload existence
  checks, and view encoders were rejected.

Durable lesson: local copy/sort reductions can improve hot and almost recover
cold, but they are not enough to beat the `cold/938` target. These are useful
implementation techniques if reintroduced narrowly.

### 1133-1142: Cursor and Lazy Navigation Attempts

- Record-time full exact descriptors were stopped after one cold commit took
  `38.1s`.
- Disabling eager exact-hit sidecar export cut a catastrophic first cold commit
  (`30.1s -> 15.4s`) and made hot beat baseline in that slice, but cold still
  missed.
- Probe cap `512` in `1137` improved both cold and hot over `1136`:
  `cold/1137 = 6.445411s`, `hot/1137 = 0.839823s`.
- Dirty current-head-only publication in `1139`, child-keyed ParentSlot in
  `1140`, membership-authority cursor rewrite in `1141`, and direct exact
  attr-path thunk in `1142` were all rejected. They did not combine navigation
  with a preaccepted verifier token, so they still paid full verifier or parent
  proof costs.

Durable lesson: lazy positive navigation is not enough by itself. It needs
proof acceptance and a cheap token/result path at the evaluator boundary;
cached parent shells or route metadata alone are not authority.

## Command JSON / Action-cache Prototype Findings

This branch of work produced real performance wins, but it is not transparent
to `libexpr` consumers and must not be counted as the product solution.

### Aggregate Proof-action Wins

- Run `25` after store-context fallback hardening:
  - 100 commits: `cold/25 = 3.132s`, `hot/25 = 0.329s`.
  - Outputs matched reference.
  - Store-context proof fallback warned and re-evaluated when a required context
    root was missing.
- Child fragments after that hardening (`27`) did not help cold:
  - `cold/27 = 3.265s`, `hot/27 = 0.326s`.
  - Artifact count tripled: `72` certs/guards/store sidecars versus `24`.
- Run `1232` after directory-listing guard fix and dir-set caching beat the
  first-10 baseline:
  - `cold/1232 = 5.974480s` versus `cold/938 = 6.058960s`.
  - `hot/1232 = 0.389435s` versus `hot/938 = 0.880468s`.
- Exact-code rerun `1234` stayed ahead on the 10-commit window:
  - `cold/1234 = 5.998170s`, `hot/1234 = 0.390135s`.

Durable lesson: proof-authorized command-output certificates can be very fast,
and the benchmark workload is narrow. But this was command-output replay, not
transparent Nix value materialization.

### By-name and Output-boundary Diagnostics

- Child fragments inherited broad guards. In debug runs they still rejected on
  the same package files and multiplied proof work.
- Structural v4 proof retained too much material. Directory structured proof
  could make hot `~11s` until filtered, and filtered v4 still did not improve
  hit count.
- The important false negatives were exact `FileBytes` rejects on canonical
  `pkgs/by-name/<prefix>/<name>/package.nix` files whose output facts stayed
  unchanged for the demanded JSON.
- Unsafe generic by-name package `FileBytes` drop oracle:
  - focused tail `cold/87`: duplicate commit `0.41s`, hot `0.35s`.
  - 10-commit `cold/88 = 4.29s`, `hot/88 = 0.37s`, outputs matched.
  - This proves the performance budget exists, not that the rule is sound.
- Output-boundary recovery was sounder but slow:
  - duplicate case moved from full fallback to about `6.9s`, not subsecond.
  - exact-head memo fixed hot but was unsafe until demoted to an explicit
    lower-bound ablation.
- Unsafe dependency-output/current-package lower bound could make focused
  duplicate runs subsecond, but a 10-commit correctness run `114` returned stale
  JSON for several commits. That invalidated the serving rule.

Durable lesson: a sound future proof needs derivation-boundary or package
interface facts, not a package-file whitelist. It must prove the owner consumed
the changed dependency only through drv/output identity or fall back.

### Accepted-result Sidecars and Scoped Source Proof

- Accepted-result sidecar v5 removed copied authority fields and referenced the
  source certificate by size/hash. It shrank sidecars by about `99%`
  (`2262146` bytes total to `25106` bytes total in the first-ten run).
- Optimized v5 no-debug/no-stats `run 152`:
  - `cold/152 = 4.411866s`.
  - `hot/152 = 0.407983s`.
  - Outputs matched reference.
- Key building using full clean worktree status dominated hot:
  - `serveJsonBuildKeysUs` was about `2.9s` over ten hot invocations.
  - Unsafe unkeyed skip-clean oracle moved hot/stats to `0.144459s`.
- Scoped-source proof serving replaced global clean status with candidate
  guard-specific proof:
  - wall-only `cold/163 = 4.466697s`, `hot/163 = 0.236693s`.
  - dirty-source adversarial probe with an untracked root child rejected all
    scoped serves and fell back.
- Hardened scoped-source serving retained strong performance:
  - `hot-stats/169 = 0.235758s`, `cold-stats/169 = 5.023010s`.
  - observed-directory shortcut later gave wall-only `hot/175 = 0.222634s`,
    `cold/175 = 4.394021s`.

Durable lesson: scoped source proof is a valuable abstraction. It can replace
global clean-worktree scans with proof over exactly the guarded facts. This may
transfer to transparent `libexpr` serving if the source proof is expressed as
ordinary proof authority rather than command-cache routing.

## Final v53/Capsule Rewrite Rejection

After cleanup, the large transparent rewrite was compared against actual origin:

- Origin `cold/1`: `5.93s` mean.
- v53 rewrite `cold/2`: `13.42s` mean.
- Origin `hot/1`: `0.89s` mean.
- v53 rewrite `hot/2`: `1.69s` mean.
- Outputs matched reference.

Artifact inspection:

- origin state: about `343M`, DB about `19M`.
- v53 state: about `602M`, DB about `261M`.
- v53 `TraceCapsules`: `61` rows totaling about `259M` payload bytes.
- Large rows repeated for `closures.gnome.{aarch64-linux,x86_64-linux}` and
  `.outPath`.
- Per path there were only `3` distinct result hashes and `5` distinct dep-set
  hashes but `10` distinct full/payload hashes.

Durable lesson: v53 preserved too much source-specific full payload data and
did not turn repeated result/dep-set facts into cold near-hits. The next design
should separate compact reusable result/dependency authority from full
source-specific provenance.

## Transferable Ideas

- Use indexes only for routing; authority lives in validated proof/result bytes.
- Keep accepted-result sidecars small by referencing source certificates, not
  copying authority fields.
- Replace global source cleanliness with scoped current-source proof over the
  exact guard facts.
- Preserve store-context availability checks and warning/fallback behavior.
- Treat derivation output path/string-with-context as a first-class demand.
- Add semantic package/derivation-boundary proof only after instrumentation can
  show the owner consumed a dependency through drv/output identity alone.
- Keep negative membership/keyset serving separate from positive route serving;
  it needs explicit complete keyset or absence authority.

## Non-transferable or Rejected as Product Path

- Command-only `nix eval --json` action cache as the main result.
- Whole-output JSON projections as normal traces.
- Child fragment sidecars carrying broad full deps.
- Unsafe by-name package file drops.
- Unsafe current-package dependency-output boundary serving.
- Exact-head fast memo without proof.
- Recomputing whole-repo Git identity per hot hit.
- Broad record-time descriptor construction.
- Cursor routing without a verifier/session accepted token.
