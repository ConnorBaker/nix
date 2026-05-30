# Eval-trace cache rewrite task list

> **Provenance — read `eval-trace-cache-README.md` first.** This is the raw
> 16k-line running log underlying BOTH research lineages (storage and
> transparent-rewrite). It is primary evidence, not a conclusion source — the
> distilled conclusions live in `eval-trace-cache-findings.md`,
> `eval-trace-cache-experiment-catalog.md`, and the authoritative 2026-05-29
> synthesis in `eval-trace-cache-redesign-plan.md`. Run numbers here span all
> three benchmark ledgers; always check which ledger a number belongs to before
> quoting it.

## Goal

Replace mutable SQLite/session-row cache authority with immutable,
content-addressed proof/result authority while preserving soundness and
improving hot and cold eval performance.

## Invariants

- Cache acceptance is a theorem: `Verify(proof, current_world, demand) == true`
  permits replay.
- Storage indexes are candidate routers, not semantic authority.
- Sessions are batch manifests/telemetry only; they never validate a hit.
- Git revisions, paths, DB rows, and source hints are locators unless a proof
  descriptor explicitly makes them authority.
- Persistent exact hits reject volatile observations.
- Every on-disk format is explicit-endian and fail-closed; native C++ structs
  are never persisted directly.
- Old SQLite caches are ignored after cutover. No migrations or compatibility
  shims.

## Recent implementation notes

- 2026-05-27 direction correction: the command-level `nix eval --json` action
  cache/proof path was removed from the active branch. It lived in `libcmd`
  (`InstallableValue::tryServeJsonOutput`, `JsonInstallableOutput`, and
  `proof-json-action-*` helpers/tests), so it could not transparently help
  `libexpr` consumers such as `nix-eval-jobs`. The prior hot win remains useful
  demand-shape evidence, but it is not product-path evidence.
- The dormant `TraceSession` JSON projection API was removed after the command
  caller was removed. `JsonOutputProjection`, installable-demand helpers, and
  direct JSON output serving described command output, not a general evaluator
  materialization contract.
- The in-memory authorized projection route/index prototype was also removed
  from the active code. Without a transparent `TracedExpr`/materialization
  caller, it was dead storage/verifier surface. The idea can return only if it
  is expressed as ordinary `TraceDemandDescriptor`/`TraceProofObligation`
  authority and used by evaluator-level navigation.
- Causal interpretation of the deleted fast path: the low hot times were caused
  by serving a whole command result before ordinary evaluator consumers entered
  their normal demand path. That proves the `outPath` workload is narrow and
  cacheable, but it does not prove transparent Nix evaluation is fast yet.
- Current product direction: keep the transparent `libexpr` substrate only -
  exact-session/direct-serving acceptance, descriptor/proof/result capsules,
  store-dependency availability checks, and missing-store-dependency fallback
  warnings. The Git/native trace-store snapshot path has now been removed from
  the active backend; SQLite plus trace capsules is the single durable backend
  for this branch slice.
- Benchmark status after this correction: historical numbers from the removed
  command path must not be compared as active branch performance. Deleted
  command-path runs around `0.23s`-`0.24s` hot are now historical lower-bound
  evidence only. A fresh no-debug/no-`NIX_SHOW_STATS` 10-commit run was
  regenerated from an empty `eval-trace-bench-results` tree on 2026-05-27 with
  run number `0`: `reference` mean `6.515678s`, `cold/0` mean `13.356975s`,
  and `hot/0` mean `1.777396s`. Outputs matched reference for all 10 commits.
  This transparent branch slice is sound on that run but is not
  performance-competitive yet; it is worse than the established pre-schema
  SQLite baseline on both cold (`~6.06s`) and hot (`~0.88s`).
- Verification after the cleanup/soundness slice: full `nix-expr-tests`,
  focused `eval-trace-deps`, benchmark harness check, and `git diff --check`
  pass after deleting the command JSON path, the JSON projection API, the
  authorized-projection storage API, and stale by-name diagnostic hooks.
- 2026-05-27 storage cleanup: removed the Git/native trace-store manifest and
  payload-pack implementation from `sqlite-trace-storage-lifecycle.cc`, removed
  the libexpr `libgit2` dependency, restored the generated SQLite schema header
  and SQLite startup/writeback lifecycle, and deleted now-dead Git manifest
  counters from `NIX_SHOW_STATS`. This cleanup removed roughly 2.3k more lines
  from `src` on top of the staged branch.
- Build/benchmark note for that cleanup: the broad `nix build -L --builders ''
  .` still fails in unrelated flake functional tests, but the component needed
  by the benchmark (`nix build -L --builders '' .#nix-cli`) succeeds and
  populates `./result/bin/nix`. The benchmark was run against that binary.
- Causal bug found during that cleanup: re-enabling SQLite exposed that
  `TraceCapsules` were inserted from a hash-keyed staging table without an
  order, so reload could assign different process-local `TraceId`s. The failing
  test was `TraceCacheIntegrationTest.Integration_ParentSlot_CapturesKeySetRemoval`.
  Adding `ORDER BY local_trace_id` to the final capsule insert made the targeted
  test and then the full `nix-expr-tests` target pass. The semantic lesson is
  that durable authority is content-addressed, but local trace ids still need a
  deterministic projection order for tests and in-process APIs that carry them
  across a reopen.
- 2026-05-27 source reset: restored `src/` to
  `origin/vibe-coding/file-based-eval-cache` in both the index and worktree
  while keeping benchmark harness/docs notes. `git diff
  origin/vibe-coding/file-based-eval-cache -- src` is empty; `git status` still
  shows staged `src` paths only because local `HEAD` contains the abandoned
  rewrite commits and the staged patch now reverses them. Focused verification
  `env -u NIX_SHOW_STATS -u NIX_SHOW_STATS_PATH nix build -L --builders ''
  .#nix-cli` passed. The restored-source subject was
  `/nix/store/dig6n4acgx29z0hyc2vvwpvvrklmcbmq-nix-2.35.0pre20260515_dirty`
  with `result/bin/nix` SHA256
  `11adc435d8c8945c7d81519e74724784fe1b1443d02f8bb7e555d4cda66bbdf7`.
- Restored-source benchmark result: no-debug/no-`NIX_SHOW_STATS`
  `cold/3`/`hot/3` on the same 10-commit window matched reference outputs.
  `cold/3` total wall time was `59.34s` versus origin `cold/1` at `59.31s`
  (mean ratio `1.004x`). `hot/3` total wall time was `8.98s` versus origin
  `hot/1` at `8.88s` (mean ratio `1.011x`). This recovers origin behavior and
  confirms the v53/capsule rewrite was the performance regression, not the
  benchmark harness.

## Active cleanup/refactor checklist

Status legend: `[ ]` not started, `[~]` in progress, `[x]` done for the current
branch slice.

- [x] Recognize the existing descriptor layer as the canonical abstraction:
  `TraceQueryDescriptor`, `TraceDemandDescriptor`, `TraceProofDescriptor`,
  `TraceProofObligation`, `TraceResultDescriptor`, and
  `TraceCacheEntryDescriptor` already exist under `libexpr`. The task is to
  reuse and extend them, not create a second demand/proof descriptor system.
- [x] Remove the non-transparent command JSON action cache from active code:
  `tryServeJsonOutput`, `JsonInstallableOutput`, `proof-json-action-*`, and the
  JSON-action functional tests are gone from the worktree.
- [x] Remove runtime authority for measurement oracles and rejected unsafe
  experiments first. These are useful benchmark notes, not proof rules:
  package-file dropping, unsafe by-name semantic recovery, unsafe skip-clean
  keying, direct-output memo/proof-capture shortcuts, exact-head fast memo,
  output-boundary trace reuse, and child-output witness shadow.
- [x] Remove the uncalled JSON projection and authorized-projection route APIs
  from `libexpr`. They are recorded as design evidence, not kept as dead code.
- [~] Audit the remaining large `src/libexpr` diff and keep only transparent
  evaluator machinery. Current large surfaces are exact-session acceptance,
  persistent descriptors, trace/result capsules, generation manifests, and
  storage/verifier refactors.
- [ ] Express the useful parts of the deleted JSON-action vocabulary as generic
  proof obligations only where they are needed by evaluator-level demands:
  source cleanliness, observed by-name keys, store-path availability,
  derivation-output identity, and complete/precise result coverage.
- [ ] Generalize the `outPath` workload around a first-class evaluator demand
  such as derivation output path/string-with-context, not command JSON output.
- [ ] Reduce default counter/reporting surface. Keep stable attempts/hits/misses,
  coarse timings, verifier/store-path/observed-key outcomes, and fallback
  reasons. Move diagnostic micro-counters behind explicit debug/stat runs or
  delete them after their ablation is recorded. Measure counter overhead when
  counters are enabled.
- [x] Remove the Git/native trace-store snapshot backend from the active SQLite
  storage lifecycle. It was a second durable backend with its own manifest,
  payload-pack format, counters, and lifecycle semantics. Keeping it obscured
  whether the transparent SQLite/capsule path was sound and performant.
- [ ] After each code cleanup slice, run at least a focused build/check and
  record whether performance changed. Full no-debug/no-stats bench runs are
  reserved for behavior-affecting changes.

## Semantic algebra consolidation task list

The current branch carries several overlapping semantic models: canonical
deps/observations, cached-result payloads, proof descriptors, trace capsules,
and a generation/serving-object backend. The product target is a single
transparent evaluator cache algebra with one active backend:

Current survey:

- Canonical deps/observations: `Dep`, `CanonicalQueryKind`, canonical dep
  hashes, shape deps, store-path availability deps. This is the evaluator-side
  semantic source of truth and remains active.
- Results: `CachedResult`, `EncodedResultPayload`, and `ResultHash`. These are
  the value facts produced by libexpr and remain active.
- Capsules: trace/result payload capsules bind canonical deps to result
  payloads for durable replay. This is the active persisted object algebra.
- Exact descriptors: query/demand/proof/result/cache-entry descriptors are a
  certificate layer derived from deps/results/session/source/policy. They remain
  active for exact replay, but the next simplification target is to make this
  layer thinner and more directly derived from canonical deps/results.
- Runtime materialization: `TracedExpr`, `TraceContext`, and derived container
  materialization are the transparent libexpr consumer surface. They remain
  active; command-output JSON shortcuts are rejected.
- SQLite trace storage: active startup bulk-load and shutdown writeback backend.
  Trace capsules are the durable content-addressed objects inside SQLite.
- Removed: file-native generation packs, generation indexes/exact-hit records,
  serving-object refs, `ResultPayloadObject`, persisted exact-generation hits,
  and exact-authority-only capsule elision.

- [~] Treat canonical deps (`CanonicalQueryKind`, `Dep`, trace hashes) and
  `CachedResult`/`EncodedResultPayload` as the semantic source of truth.
- [x] Keep SQLite plus trace capsules as the only active backend for this
  cleanup slice. Immutable generation packs and Git/native trace-store
  snapshots are not active authority.
- [x] Remove generation-format/generation-store source, headers, tests, and
  lifecycle sidecars from the build instead of carrying them as a parallel
  backend algebra.
- [~] Collapse the exact replay code around one certificate concept derived
  from demand, trace, result, session/source/policy, and coverage. Avoid
  separate query/demand/proof/result/cache-entry objects where a canonical dep
  or result payload already carries the fact.
- [x] Keep direct serving only as a local scalar optimization after exact
  descriptor acceptance. Do not persist a separate serving-authority object
  graph for it in this branch.
- [x] Re-run focused compile/tests after the deletion slice, then record the
  resulting `src` diff size and any behavior changes.

### Cleanup slice 2026-05-27

- Removed the active command-output action cache because it was not transparent
  to `libexpr` consumers. This clears the `libcmd`-specific design from the
  product path rather than trying to polish it.
- Removed stale staged index entries for those files after restoring the
  worktree, so `git status` no longer reports deleted staged experiments as
  active branch changes.
- Removed uncalled `TraceSession` JSON projection methods and the authorized
  projection index/API from the active source. This reduced `src` churn from
  roughly `38,144` insertions before cleanup to roughly `24,597` insertions
  after that slice.
- Removed the remaining active `NIX_EVAL_TRACE_JSON_ACTION_*`/by-name package
  diagnostic hooks from `libexpr` and the benchmark harness tests. They were
  research telemetry for the rejected command-output path, not transparent
  replay authority, and they added evaluator hot-path hooks/counters that could
  perturb wall-only runs.
- Kept the generic derivation-output identity observation, but moved it out of
  the by-name package diagnostic machinery. `derivationStrict` now records
  output-name to output-path identity for static derivation outputs directly as
  an evaluator-level dependency fact.
- After removing the stale diagnostics, the active `src` diff is roughly
  `23,990` insertions and `2,011` deletions against `HEAD`.
- Disabled the exact-authority-only capsule elision. Exact descriptors remain
  an accelerator, but canonical trace/result capsules are still written so
  normal verify/query/history paths retain the same authority object.
- Fixed derived attrset result-shape publication: dependency capture now stays
  open while `buildCachedResult()` publishes the result, and traced attrsets
  record the relevant keyset dependency before finalizing deps.
- Made partial derived-container keysets conservative for now. For `//`
  shadowing, `removeAttrs`, and `listToAttrs`, the prototype records broader
  source keysets and may miss on unrelated changes until there is a narrower
  derived-container proof. This is intentional fail-closed behavior, not the
  final precision target.
- Removed or adjusted tests that encoded contradictory/non-current semantics:
  persistent exact descriptor tests now cover the implemented zero-dep
  descriptor path, Git implicit-guard tests no longer contradict same-repo Git
  authority semantics, and precision tests document the current conservative
  misses.
- Verification: full `nix develop .#native-clangStdenv -c meson test -C build
  'nix-expr-tests' --print-errorlogs --timeout-multiplier 10` passed; focused
  `nix develop .#native-clangStdenv -c meson test -C build 'eval-trace-deps'
  --print-errorlogs` passed; benchmark harness check
  `nix build -L .#checks.x86_64-linux.eval-trace-bench` passed; `git diff
  --check` passed.
- Continued consolidation: removed `generation-format.{cc,hh}`,
  `generation-store.{cc,hh}`, and their standalone tests from the active tree
  and Meson manifests. The branch no longer carries file-native generation
  records, serving-object refs, `ResultPayloadObject`, exact-generation hits,
  or exact-authority-only capsule elision as a second backend algebra.
- Removed the remaining direct exact-result APIs from `TraceBackend`,
  `Verifier`, `SqliteTraceStorage`, and the context adapter. Those APIs were
  command/known-leaf entry points, not transparent libexpr replay. Direct
  scalar serving now exists only inside `tryAcceptExactReplayDescriptor()` after
  the same exact descriptor acceptance used by normal verification.
- Removed the unused compact `buildPersistentExactDescriptorsFor(...)` builder
  and trimmed `PersistentExactDescriptorSource` down to the fields that build
  descriptors: session key, recovery key, attr-path material, result payload,
  result hash, and observations. This avoids parallel descriptor-construction
  semantics.
- Removed the active Git/native trace-store manifest loader/writer after the
  later review showed it was a second durable backend rather than a semantic
  proof simplification. SQLite/capsules are again the only active storage
  lifecycle.
- Verification after this deletion slice: `nix develop .#native-clangStdenv -c
  meson compile -C build nixexpr nix-expr-tests` passed; full `nix develop
  .#native-clangStdenv -c meson test -C build 'nix-expr-tests'
  --print-errorlogs --timeout-multiplier 10` passed; focused `nix develop
  .#native-clangStdenv -c meson test -C build 'eval-trace-deps'
  --print-errorlogs --timeout-multiplier 10` passed; `git diff --check`
  passed.
- Current staged `src` diff after the Git/native trace-store removal is roughly
  `15,226` insertions and `3,015` deletions. The previous staged size was
  `16,514` insertions and `2,020` deletions, so this cleanup removed about
  `1.3k` net lines and about `2.3k` absolute lines from active `src`.

### Cleanup slice 2026-05-26

- Removed the exact-head fast memo path from JSON serving and command key
  construction. The remaining route is proof-key based; `GitJsonActionCacheKeys`
  no longer carries `fastKey`, and the old fast-v1 action cache helpers are
  gone.
- Removed child-fragment proof publication/loading from `tryServeJsonOutput`.
  The trace-session JSON projection API still supports fragments internally,
  but the command proof cache no longer publishes separate child certificates.
- Removed the direct-output memo/proof-capture serving branches and their env
  gates. The exact-source proof experiment remains separate for now because it
  writes a proof certificate rather than serving an unproven memo.
- Removed the unsafe by-name semantic recovery oracle and the package-file drop
  oracle from active guard acceptance. Exact path changes now reject normally.
- Removed the disabled child-output witness shadow sidecar path. This also
  removes the extra per-child context split in the outPath JSON renderer.
- Simplified Git action key construction after removing unsafe clean-status
  oracles: scoped-source serving is now the only path that uses `gitHeadRevision`
  without `cleanGitHeadRevision` at key-build time.
- Collapsed the exact-source direct-proof experiment to one binary gate and
  removed its benchmark steering knobs/certificate-count scan. This also
  removed the now-unused proof-load miss summary plumbing.
- Trimmed the corresponding zero-only/rejected-oracle counters from
  `NIX_SHOW_STATS`, including the stale direct-proof-capture group after the
  rejected direct-output proof shortcut was deleted. The surviving exact-source
  proof renderer is now named for exact-source proof publication, not direct
  proof capture.
- Fixed two branch compile drifts in
  `sqlite-trace-storage-lifecycle.cc`: include the local trace-serialize bind
  helpers directly, and treat `readFile()` as the `std::string` it returns when
  loading hot manifest segments.
- Fixed the release/unity build variant of the hot-manifest segment read by
  qualifying it as `nix::readFile(segment)`. In Meson unity builds,
  `generation-store.cc`'s anonymous optional-returning helper named
  `readFile()` can otherwise shadow the intended filesystem helper.
- Removed an abandoned `TraceSession::ExactProjectionRequest` API and its
  unused local helpers. The remaining installable replay path goes through the
  first-class `InstallableDemand` methods instead of two overlapping projection
  abstractions.
- Removed unused capsule and generation-store helper functions exposed by the
  cleanup build warnings. These were remnants of older payload/object write
  paths and did not participate in current replay authority.
- Stopped generating the legacy SQLite schema header after removing the last
  include site. The schema file remains as the normalized SQLite schema source,
  but it is no longer part of this generation-store prototype build path.
- Removed zero-only counters that had no increment sites and only appeared in
  declarations or `NIX_SHOW_STATS` output. A follow-up reference-count scan over
  `nr*` eval-trace counters now finds no counters with only declaration,
  definition, and stats/reporting references.
- Verification: `nix develop .#native-clangStdenv -c meson compile -C build
  nixcmd nixexpr` passes. `git diff --check` passes.

### 2026-05-27 post-cleanup wall-only benchmark

Release benchmark build:

```
nix build -L --builders '' .#nix-cli --out-link result --print-out-paths
```

Candidate binary:
`/nix/store/mchjfw6n6jbs541d7m1kxaalbkggf54b-nix-2.35.0pre20260515_dirty/bin/nix`,
sha256 `22ac907f7a5cbb7ee457bc6298284b94b880a6bf3818fdf457491a8bfb5683cf`.

The release build caught one issue that the local clang devshell compile did
not: in Meson unity builds, `generation-store.cc`'s anonymous helper
`readFile(const std::filesystem::path &) -> std::optional<std::string>` can
shadow the intended filesystem helper in `sqlite-trace-storage-lifecycle.cc`.
The fix is to call `nix::readFile(segment)` when loading hot-manifest segment
bytes.

Run `279` repeated the 100-commit wall-only source-cert/accepted-result flag
bundle with no debug and no `NIX_SHOW_STATS`:

- `cold/279`: mean `2.440401s`, p50 `0.443463s`, p90 `16.300832s`,
  max `18.612880s`.
- `hot/279`: mean `0.243248s`, p50 `0.213422s`, p90 `0.367386s`,
  max `0.567038s`.
- Pairwise cold comparison against regenerated `cold/0`: total `244.04s`
  versus `356.36s`, mean ratio `0.498x`, `86/100` commits faster by more
  than 5%, `14/100` slower by more than 5%.
- Pairwise hot comparison against regenerated `hot/0`: total `24.32s` versus
  `104.72s`, mean ratio `0.233x`, all `100/100` commits faster by more than
  5%.
- Output hashes matched the reference across `cold/0`, `hot/0`, `cold/279`,
  and `hot/279`.
- `cold/279` and `hot/279` stderr logs were empty.
- Artifact shape across `cold/279` and `hot/279`: `24` aggregate proof
  certificates, `200` accepted-result sidecars totaling `431982` bytes,
  `176` alias files, `24` guard files, and `24` store-path sidecars.

Causal read:

- The cleanup did not destroy the performance-relevant path. The current branch
  still beats the regenerated baseline on both cold mean and hot mean, and hot
  is still a full accepted-result path rather than a counter/stat artifact.
- The cleanup is not performance-neutral versus prior run `216`:
  `cold/216 = 2.076039s`, `hot/216 = 0.220917s`; `279` is about `17.6%`
  slower cold and `10.1%` slower hot. Do not attribute this to semantic
  simplification without a targeted rerun or diff read.
- The cold regression is concentrated in the protected-lane anchor commits,
  whose p90 rose from `12.657s` in `216` to `16.301s` in `279`. The next
  performance investigation should compare run `216` and `279` manifests and
  authority-mode sidecars, then inspect whether any cleanup enlarged
  publication/verification work on anchor commits.
- The accepted-result sidecars are smaller per run than `216`
  (`215991` bytes per run versus `233891`), so the slowdown is unlikely to be
  caused by accepted-result payload growth.

### 2026-05-27 env and unsafe publisher cleanup

Changes:

- Removed the unsafe outPath publication skip branches from
  `TraceSession::tryServeOutPathJsonObject()`:
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_OUTPATH_SKIP_TRACE_ACTIVATION` and
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_OUTPATH_SKIP_SHAPE_DEPS` no longer
  exist in active code. The safe publisher now always installs
  `TraceActivationScope` and always records container shape deps before
  considering proof publication.
- Centralized active JSON action-cache feature environment names in one local
  `JsonActionFeature` table in `proof-json-action-cache.cc`. Routing-scope
  ignored-env handling now consults that table instead of duplicating every
  feature env string separately.
- Removed stale ignored env names for deleted experiments from active
  routing-scope code. Historical benchmark notes still mention them as past
  ablations, but setting those names no longer changes cache keying or
  authority.
- Made generic feature-gate helpers file-local and removed the unused
  non-negative size env parser from `proof-json-action-cache.hh/.cc`.
- Updated the eval-trace-bench provenance test fixture to use the active
  accepted-result sidecar env instead of the deleted child-fragment env.

Verification:

- `git diff --check` passed.
- `nix develop .#native-clangStdenv -c meson compile -C build nixcmd nixexpr`
  passed.
- `nix build -L --builders '' .#checks.x86_64-linux.eval-trace-bench` passed:
  `87` pytest tests, ruff, ruff format check, and pyright.
- `nix build -L --builders '' .#nix-cli --out-link result --print-out-paths`
  passed in release/unity mode, producing
  `/nix/store/sfac7kxv36bxigl0bqq609m2mk9si51v-nix-2.35.0pre20260515_dirty`,
  binary sha256
  `cab9e177b6f0e200377fd632437064b405f71a4a084a7b4d973724c260198054`.
- Smoke run `280` repeated the first 10 commits with the source-cert/
  accepted-result flag bundle, no debug, and no `NIX_SHOW_STATS`:
  - `cold/280`: mean `5.253022s`, p50 `0.391202s`, p90 `15.502776s`,
    max `19.580709s`.
  - `hot/280`: mean `0.232893s`, p50 `0.228824s`, p90 `0.261611s`,
    max `0.272862s`.
  - Output hashes matched the reference and stderr logs were empty.

Causal read:

- This slice is cleanup, not a new performance claim. The standard
  source-cert/accepted-result flags never enabled the deleted unsafe publisher
  skips, so the expected behavior is no serving change for normal wall-only
  runs.
- The 10-commit smoke confirms the env-table refactor did not obviously break
  routing, authority-mode selection, or accepted-result serving. It is not a
  substitute for the full 100-commit comparison if future edits touch authority
  semantics.
- The first-10 cold anchors remain expensive (`7d761689...`, `fd3fbe0...`,
  `7fb36e...`), consistent with the `279` read that the remaining cold problem
  is anchor publication/verification cost, not accepted-result payload size.

## 2026-05-24 authorized projection benchmark checkpoint

Superseded note: the run `0` values in this checkpoint were from an earlier
reference population. After benchmark results were deleted, run `0` was
regenerated again on 2026-05-26. The current reference numbers are recorded in
the 2026-05-26 section below: `cold/0` mean `3.563573s`, `hot/0` mean
`1.047246s`.

Baseline was regenerated from the branch baseline commit
`92a3df1ab706cf514357ae57b367050b373ff146` using:

```
nix build -L --builders '' ".?rev=92a3df1ab706cf514357ae57b367050b373ff146"
```

Run `0` is the refreshed no-debug/no-`NIX_SHOW_STATS` reference baseline.
Over 100 commits it measured:

- `cold/0`: mean `3.316277s`, median `0.938353s`, p90 `10.515076s`,
  max `13.428621s`.
- `hot/0`: mean `0.886366s`, median `0.885450s`, p90 `0.905417s`,
  max `0.932551s`.

Parallel review/adversarial findings:

- The current design is not fundamentally incapable of beating the baseline:
  the action-proof route is already far below the baseline hot mean. The open
  question is cold-tail reduction, not hot feasibility.
- The shape/projection abstractions remain useful as routers and lazy
  materialization controls, but they are not the main cold bottleneck for the
  current `outPath` workload. Cached shape authority can avoid navigation and
  materialization, but it cannot by itself avoid first-time derivation output
  forcing when the output changes.
- Aggregate proof-action cache hits, exact-head routing, and child fragment
  reconstruction explain most of the measured hot win. Child certificates are
  not proven to be the cold lever.
- Debug run `cold-debug/5` showed slow cold commits dominated by
  `projection.outPathPublish.childCaptureUs` / forced child `outPath`
  computation, with guard scans rejected before fresh child capture and store.
- The broad accepted-certificate scan was a real hot-path hazard. Exact HEAD
  miss followed by sorting/scanning sibling certificates produced avoidable
  candidate checks.
- Full cert promotion confirmed the scan hypothesis but duplicated large
  `.json`, `.guard`, and `.store-paths` sidecars during cold publication.
  That improved hot time but regressed cold mean/tail on 100 commits.

Implementation and benchmark sequence:

- Run `10` was the prior accepted prototype:
  `cold/10` mean `3.070835s`, median `0.481119s`, p90 `11.399044s`,
  max `14.017373s`; `hot/10` mean `0.477112s`, median `0.431395s`,
  p90 `0.649976s`, max `0.783659s`. Outputs matched reference.
- Run `11` narrowed child JSON fragment deps to direct child deps rather than
  inherited parent deps. This was semantically cleaner, but did not materially
  change performance because child output sidecars are still dominated by
  store-path/source proof material.
- Run `13` tested full accepted-cert promotion on 100 commits:
  `cold/13` mean `3.204087s`, median `0.435321s`, p90 `11.959531s`,
  max `16.462957s`; `hot/13` mean `0.362508s`, median `0.353146s`,
  p90 `0.371601s`, max `0.723712s`. Outputs matched reference, but each run
  had `148` cert JSON files plus matching guard/store-path sidecars.
- Run `15` replaced full promotion with a small exact-head alias file pointing
  at an accepted sibling cert. The alias binds `proofKey`, repo root, current
  head rev, target cert filename, target cert size, and target cert SHA-256.
  It is routing only; the original target certificate and its guard/store-path
  sidecars still provide all semantic authority.
- Run `15` 100-commit result:
  `cold/15` mean `3.056803s`, median `0.481447s`, p90 `11.256954s`,
  max `13.927125s`; `hot/15` mean `0.360528s`, median `0.351627s`,
  p90 `0.370974s`, max `0.690092s`. Outputs matched reference.
- File count comparison confirms the mechanism: `cold/13` and `hot/13` each
  had `148` cert JSONs, `148` guards, and `148` store-path sidecars; `cold/15`
  and `hot/15` each had `72` cert JSONs, `76` aliases, `72` guards, and `72`
  store-path sidecars.

Decision:

- Keep the alias-routing direction. It preserves the broad-scan hot win while
  removing the full sidecar-copy cold penalty.
- Keep child direct-dep fragments only if the code stays simpler and the
  authority story remains clearer; do not count it as a performance win.
- Do not spend more time trying to make positive shape navigation fix the cold
  tail by itself. The next cold lead is an `outPath`-specific path that avoids
  or amortizes first-time child derivation output forcing, or records a smaller
  proof for unchanged child output availability.
- Add a focused ablation later: aggregate-only action proof, alias disabled,
  and child-fragment disabled. This should quantify exactly how much of the
  `hot/15` result comes from each layer before hardening the prototype.

### 2026-05-24 ablation: action proof layers

Added explicit ablation knobs for the JSON action cache. These env vars are
ignored by the proof-key environment scope because they only select local
serving/publication strategy; they must not create distinct semantic cache
entries:

- `NIX_EVAL_TRACE_JSON_ACTION_DISABLE_ALIAS=1`
- `NIX_EVAL_TRACE_JSON_ACTION_DISABLE_AGGREGATE=1`
- `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_CHILD_FRAGMENTS=1`
- `NIX_EVAL_TRACE_JSON_ACTION_DISABLE_CHILD_FRAGMENTS=1`

All runs below are 100 commits, no debug, no `NIX_SHOW_STATS`, and all outputs
matched `reference`.

| run | variant | cold mean | cold median | cold p90 | hot mean | hot median | hot p90 | cache files |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `15` | aggregate + alias + child fragments | `3.056803s` | `0.481447s` | `11.256954s` | `0.360528s` | `0.351627s` | `0.370974s` | `72` certs, `76` aliases |
| `16` | alias disabled | `3.065790s` | `0.479025s` | `11.263326s` | `0.484032s` | `0.460229s` | `0.654259s` | `72` certs, `0` aliases |
| `17` | child fragments disabled | `3.015501s` | `0.481243s` | `11.128435s` | `0.367538s` | `0.357745s` | `0.379424s` | `24` certs, `76` aliases |
| `18` | aggregate disabled | `3.754139s` | `1.268727s` | `11.567447s` | `0.999044s` | `0.982776s` | `1.080710s` | `48` certs, `152` aliases |
| `0` | refreshed baseline | `3.316277s` | `0.938353s` | `10.515076s` | `0.886366s` | `0.885450s` | `0.905417s` | n/a |

Causal read:

- Alias routing is causal for hot performance. Disabling aliases keeps cold
  essentially unchanged but moves hot mean from `0.360528s` to `0.484032s` and
  p90 from `0.370974s` to `0.654259s`. The unchanged cold time rules out alias
  publication as the main cold mechanism; the hot regression matches the
  expected return of broad sibling cert scans.
- Aggregate whole-output action certificates are causal for both hot and cheap
  cold commits. Disabling aggregates moves hot beyond the refreshed baseline
  (`0.999044s` vs `0.886366s`) and lifts cold median from `0.481447s` to
  `1.268727s`. Child fragments alone cannot replace the aggregate layer because
  the run still pays trace-session/navigation/projection overhead.
- Child fragments are not causal for the current win. Disabling them slightly
  improves cold mean and materially reduces proof files (`72` certs to `24`
  certs), with only a small hot change (`0.360528s` to `0.367538s`). The likely
  explanation is that aggregate certs already serve the whole JSON output for
  the repeated cases; child certs add write/read surface without removing the
  dominant slow commits.

Refinement:

- Child fragment certs are now disabled by default. They remain behind
  `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_CHILD_FRAGMENTS=1` for future targeted
  experiments, but the default prototype should be aggregate + alias only.
- A 10-commit no-env smoke run after flipping the default (`cold/19`,
  `hot/19`) matched reference output and produced only aggregate artifacts:
  `5` certs and `5` aliases per run, with no child certs. It measured
  `cold/19` mean `6.046896s` and `hot/19` mean `0.378041s`; the short cold
  mean reflects the first-10 changed-output tail and should be compared to
  100-commit run `17` for the default-shape performance read.
- The next design search should stop investing in child projection certs for
  this workload. The profitable path is a smaller/faster aggregate certificate
  and an exact-head alias/index that avoids directory scans without duplicating
  sidecars.
- Aggregate cert JSON is tiny; the cost is in the proof sidecars. In `cold/17`,
  the `24` aggregate JSON certs total only `43,488` bytes, while their git
  guard sidecars total `9,711,605` bytes and store-path availability sidecars
  total `17,185,570` bytes. A representative aggregate cert has a `208` byte
  JSON output, `4` retained deps, a `404,763` byte git guard, and a `716,149`
  byte store-path sidecar. This makes sidecar encoding/verification and
  store-path proof scope the next concrete optimization surface.
- Cold-tail work still needs a different lever: reduce first-time
  `outPath`/derivation output forcing or make its proof cheaper. The ablation
  shows child fragments are not that lever.

### 2026-05-24 sidecar verification micro-ablation: exact guard skip

Tried a narrow git-guard fast path: after parsing and binding a guard sidecar,
accept it without running `git diff` when the guard `baseRev` equals the current
clean `HEAD`.

Run `20` was the 100-commit no-debug/no-`NIX_SHOW_STATS` benchmark for that
change. Outputs matched reference and artifact counts stayed at the refined
default shape (`24` certs, `76` aliases, `24` guards, `24` store-path sidecars):

- `cold/20`: mean `3.160505s`, median `0.483041s`, p90 `11.392688s`,
  max `15.255260s`.
- `hot/20`: mean `0.390361s`, median `0.361532s`, p90 `0.438872s`,
  max `0.810219s`.

Compared with the refined default run `17`, this did not help:

- `cold/17`: mean `3.015501s`, median `0.481243s`, p90 `11.128435s`.
- `hot/17`: mean `0.367538s`, median `0.357745s`, p90 `0.379424s`.

Causal read:

- The exact-base guard skip is not a useful lever for this workload. It can only
  affect exact current-head certs, while most hot wins now route through aliases
  to older accepted certs. The run does not prove the skip caused the regression
  because benchmark noise and changed-output tail variance are visible, but it
  does prove the change is not worth retaining as a benchmark-first refinement.
- Removed this micro-change after run `20`. The next sidecar lead is
  alias-certified guard acceptance: when a broad cert is accepted for the
  current clean head and an alias is written, bind that alias to the target cert
  and guard ref so the next exact alias hit can skip the guard sidecar/diff while
  still verifying dynamic store-path availability.

### 2026-05-24 sidecar verification micro-ablation: alias-certified guard

Tried the follow-up alias-certified guard prototype on a 10-commit smoke run.
Alias files were upgraded to `nix.eval-trace.json-action-proof.alias.v2`, binding
the current clean `HEAD`, target cert filename/size/hash, and the accepted
`gitGuardRef` size/hash. On alias hit, the loader skipped re-reading/rerunning
the git guard but still required the target cert's `gitGuardRef` to match the
alias-bound ref and still verified store-path availability.

Run `21` was no-debug/no-`NIX_SHOW_STATS`, 10 commits:

- `cold/21`: mean `6.505919s`, median `6.122723s`, p90 `12.057450s`,
  max `15.303074s`.
- `hot/21`: mean `0.389530s`, median `0.373076s`, p90 `0.429610s`,
  max `0.468602s`.

The comparable refined-default 10-commit smoke run was `19`:

- `cold/19`: mean `6.046896s`, median `5.594785s`, p90 `11.225144s`.
- `hot/19`: mean `0.378041s`, median `0.378083s`, p90 `0.386447s`.

Causal read:

- Correctness and artifact shape were fine (`5` certs, `5` aliases, `5` guards,
  `5` store-path sidecars; outputs matched reference), but the smoke was not
  promising. Skipping alias guard revalidation does not move the hot path enough
  to justify retaining the extra alias authority surface.
- This points away from git-guard revalidation as the current bottleneck. The
  remaining proof-hit floor is more likely fixed command setup plus the
  store-path availability sidecar parse/check. The cold tail is still first-time
  derivation/output forcing.
- Removed the alias v2 prototype after run `21`; keep the simpler v1 routing
  alias until a broader profile shows git guard checks are dominant.

Debug/stat diagnostic after reverting the alias v2 code:

- `hot-debug/6` used the current simple-alias code with 10 commits, copied from
  `cold/17` state via `--hot-from cold/17`.
- Mean wall time was `0.378771s`; outputs matched via the normal harness.
- Proof-action load counters across the 10 commits:
  - attempts/hits: `10/10`
  - candidate checks: `10`
  - total proof-action load time: `591080us` (`59108us` average)
  - git guard time: `83754us` (`8375us` average)
  - parse time: `328us`
  - verify time: `506318us` (`50632us` average)

Causal read:

- The loader is already doing one candidate check per commit. Alias routing is
  doing its job.
- Git guard revalidation is a minority of accepted-hit cost, so the failed
  exact-guard and alias-v2 experiments are consistent with the counters.
- Since representative aggregate certs retain only four env deps and the cert
  JSON parse cost is negligible, the `verifyUs` floor is almost certainly
  dominated by decoding/checking the large store-path availability sidecar.
  Next work should either narrow the store-path proof scope for command-level
  JSON output or add finer counters around sidecar read/parse/query before
  changing semantics.

Added the finer sidecar counters and reran the same 10-commit hot debug shape as
`hot-debug/7` after rebuilding the candidate (`df8bf421906ae5cdb3fe6c8e87003b8acd65df9f6d5d1e7003552e02fdb67024`):

- Mean wall time: `0.392626s` (median `0.367655s`, max `0.498946s`).
- Proof-action load attempts/hits: `10/10`; candidate checks: `10`.
- Total proof-action load time: `705595us` (`70559us` average).
- Git guard time: `37275us` (`3728us` average).
- Cert parse time: `322us`.
- Verify time: `667342us` (`66734us` average).
- Store-path sidecar read: `4061us` (`406us` average).
- Store-path sidecar decode: `188301us` (`18830us` average).
- Store-path validity queries: `121730` path checks, `434941us`
  (`43494us` average per command).

Causal read:

- Store-path proof verification, not git guard routing, is now the dominant
  accepted-hit verifier cost. The sidecar does `12173` store-path checks per
  command; query time plus text decode explains almost all of `verifyUs`.
- This makes alias-v2 guard binding the wrong next lead. Even a perfect guard
  cache can only recover a few milliseconds on this benchmark shape.
- Added an explicit experimental ablation knob,
  `NIX_EVAL_TRACE_JSON_ACTION_CONTEXT_STORE_PROOF=1`, to store only the
  availability of store paths carried by the JSON output context. This is not yet
  the hardened design; it is a ceiling measurement for whether narrowing
  command-output store proof scope is worth pursuing. The soundness question is
  whether the command-output cache is allowed to ignore unrelated `.drv`
  publication side effects from derivations that were evaluated but are not
  needed to interpret the returned JSON/context.

Context-store-proof ablation results:

- `cold/22`/`hot/22` were the first 10 commits with
  `NIX_EVAL_TRACE_JSON_ACTION_CONTEXT_STORE_PROOF=1`.
  - `cold/22`: mean `6.428569s`, median `11.685611s`, max `15.320265s`.
  - `hot/22`: mean `0.335411s`, median `0.334657s`, max `0.343359s`.
  - Outputs matched reference.
  - Artifact shape: `5` certs, `5` aliases, `5` guards, `5` store-path
    sidecars; sidecars total `1155` bytes. A representative sidecar contains
    only the two returned context `.drv` paths instead of `12173` paths.
- `hot-debug/8` reused `cold/22` and confirmed the counter-level cause:
  - attempts/hits: `10/10`; candidate checks: `10`.
  - total proof-action load time: `49094us` (`4909us` average), down from
    `705595us` on `hot-debug/7`.
  - store-path checks: `20`, down from `121730`.
  - store-path query time: `611us`, down from `434941us`.
  - verify time: `2640us`, down from `667342us`.
- `cold/23`/`hot/23` were the 100-commit no-debug/no-`NIX_SHOW_STATS` run with
  context-store proof enabled.
  - `cold/23`: mean `3.245756s`, median `0.446768s`, p90 `12.022571s`,
    max `15.597287s`.
  - `hot/23`: mean `0.329875s`, median `0.320645s`, p90 `0.353502s`,
    max `0.681415s`.
  - Outputs matched reference.
  - Artifact shape: `24` certs, `76` aliases, `24` guards, `24` store-path
    sidecars; sidecars total `5544` bytes.

Causal read:

- The hot improvement is causal: the only intended change is store-proof scope,
  and the debug counters show the removed work directly. Full sidecar
  verification was spending tens of milliseconds per accepted command proving
  internal `.drv` publication side effects that the JSON output cache does not
  need to return the JSON string and its context.
- The cold mean regressed versus `cold/17` (`3.245756s` vs `3.015501s`) but still
  beats regenerated baseline `cold/0` (`3.316277s`). The regression is in the
  changed-output tail, not in cheap-cache commits; median improved versus
  `cold/17` (`0.446768s` vs `0.482289s`). This does not look caused by
  sidecar-size reduction; it is the existing materialization tail variance.
- Adopt context-store proof as the prototype default and keep
  `NIX_EVAL_TRACE_JSON_ACTION_FULL_STORE_PROOF=1` as the escape hatch for the
  conservative/full side-effect proof. The hardening task is to document the
  semantic contract explicitly: command-output cache proves the returned
  JSON/context, not every incidental derivation publication side effect observed
  while computing it.

Default-flip verification:

- Rebuilt the candidate with context-store proof default:
  `6de24d2c45ba1150124f59fe4fbeee305e43593bf9cd18f724202cbfeea7872e`.
- `cold/24`/`hot/24` were a 10-commit no-env/no-debug/no-`NIX_SHOW_STATS` smoke:
  - `cold/24`: mean `6.475885s`, median `11.904896s`, max `15.248966s`.
  - `hot/24`: mean `0.338716s`, median `0.340338s`, max `0.343326s`.
  - Outputs matched reference.
  - Default certs now carry `"storePathProofScope": "output-context-v1"`;
    `5` store-path sidecars total `1155` bytes.
- `git diff --check` passed for the touched C++ files and this experiment log.

### 2026-05-24 observed-key proof and by-name directory proof

Implemented a gated prototype behind
`NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1`. This remains a
benchmark-first proof experiment, not yet the hardened default.

Implementation notes:

- Refined directory-listing git guards so a change below an existing direct
  child directory does not invalidate a parent `readDir` observation unless the
  direct child entry type/presence changes. This removed broad root-directory
  false positives and is on the normal guard path.
- Added `nix-observed-keyset-v1` guards for selected non-recursive nix attrset
  files:
  - `pkgs/top-level/all-packages.nix`
  - `pkgs/top-level/aliases.nix`
  - `pkgs/top-level/python-aliases.nix`
  - `pkgs/top-level/python-packages.nix`
- The guard records the observed key universe, a scope hash, and binding hashes
  for observed keys that are present. Verification reparses the changed file,
  requires the same scope hash, rejects if a previously absent observed key
  appears, and rejects if any observed present binding changes or disappears.
- Structured deps for those candidate files are covered by the observed-key
  guard, so they no longer also add a strict exact-file guard.
- Added a v6 git guard sidecar extension for `pkgs/by-name/<prefix>` directory
  listings. Exact file deps are still checked first. If a by-name direct child
  add/delete/type change is for an unobserved package key, the directory-listing
  guard can accept; if the child is observed, it rejects normally.
- Tightened the by-name extension after an adversarial read: observed children
  are now recorded only from actual structured directory `hasKey` observations
  for that exact `pkgs/by-name/<prefix>` directory. They are no longer inferred
  from the global attr path/key universe, and an empty observed-child set no
  longer relaxes the directory guard. This keeps the positive-membership proof
  but prevents a complete keyset observation (`attrNames`, `length`, etc.) from
  being treated as an unobserved-child whitelist.
- Tightened the same by-name extension again after classifying the remaining
  run `56` slow stores: if a directory also has a complete keyset structured
  dep (`#keys`, from `attrNames`/similar), we now suppress observed-child
  relaxation for that directory entirely. The mixed case matters because a
  single `readDir` result can be used both for `hasAttr("hello")` and for
  `attrNames`; positive membership authority must not authorize a stale full
  keyset. This moves the observed-key prototype cache namespace to v7 so older
  guards that recorded the broader relaxation are not reused silently.
- First validation of that hardening caught an overreach: every non-empty
  traced directory attrset also records an `ImplicitStructure #keys` fingerprint
  when it is created, even when user code only asked `hasAttr`. That fingerprint
  is verifier guard material, not semantic complete-keyset authority. The
  suppression now only treats explicit `StructuredProjection #keys` deps as
  complete keyset demands; otherwise the membership-only by-name path would be
  disabled by construction.
- Fixed an adversarial edge case in that by-name prefix mapping: nixpkgs has
  one-character by-name prefix directories (`q`, `h`, `j`, `t`), so package
  names map to `min(2, name.size())` prefix characters, not always two.
- Moved the observed-key prototype into its own v6 proof namespace. The cache
  directory, certificate version, aggregate proof key salt, and child proof key
  salt now all agree on v6/v4 observed-key authority, so stale pre-observed
  proof facts cannot be silently reused by the observed-key experiment.
- Current observed-key artifacts use `eval-trace-json-action-proof-v7` after
  the mixed keyset/positive-membership hardening above.
- Added cheap `NIX_SHOW_STATS` counters for observed-key guard storage,
  observed-key guard verification/skips/rejects, and observed-directory
  by-name decisions. These are measurement-only and remain disabled when stats
  are off.

Benchmark sequence:

- Run `34` is invalid: it generated observed-key certificates with a mismatched
  payload/cache version. Ignore it except as the point where the bug was found.
- Runs `35`/`36` used observed-key proof plus retained structural proof. The
  extra hit was real, but hot path cost was too high:
  - `cold-debug/35`: 6 hits / 4 misses, mean `6.552837s`.
  - `hot-debug/35`: mean `0.480675s`, with `verifyUs=1168424us`.
  - `cold/36`: mean `5.241380s`; `hot/36`: mean `0.472251s`.
- Runs `37`/`38` used lean observed-key proof without retained structural
  payload. Hot recovered, but the cold miss remained because
  `python-packages.nix` was still present as an exact guard through structured
  deps:
  - `cold/37`: mean `6.161148s`; `hot/37`: mean `0.337955s`.
  - `cold-debug/38`: 5 hits / 5 misses; `1575c9f64e00` still missed on an
    exact `pkgs/top-level/python-packages.nix` guard.
- Run `40` covered `python-packages.nix` with the observed-key guard and skipped
  the redundant exact structured guard. This converted `1575c9f64e00` from an
  11s miss to a proof hit:
  - `cold/40`: mean `5.160274s`, median `0.375406s`.
  - `hot/40`: mean `0.357587s`, median `0.355459s`.
  - Outputs matched reference.
- Run `41` confirmed the counter-level cause:
  - `cold-debug/41`: 6 hits / 4 misses, `load.guardAccepted=6`,
    `load.guardRejected=9`.
  - Remaining first-10 misses were: first commit with no cert, by-name
    `anonymous-pro-fonts` deletion, and two commits whose output JSON actually
    changed.
- Runs `43`/`44` added the v6 by-name observed-child guard. This converted the
  `3f7b5d89ca3e` by-name false miss into a proof hit:
  - `cold/43`: mean `4.263628s`, median `0.365571s`.
  - `hot/43`: mean `0.356206s`, median `0.353914s`.
  - `cold-debug/44`: 7 hits / 3 misses, `load.guardAccepted=7`,
    `load.guardRejected=3`.
  - `hot-debug/44`: mean `0.361565s`; guard sidecars grew from about `404 KiB`
    to `468 KiB` each, but no-debug hot stayed near the lean observed-key path.
- Run `45` was a final 10-commit no-debug smoke after optimizing by-name guard
  construction. Outputs matched reference:
  - `cold/45`: mean `4.301240s`; `hot/45`: mean `0.362073s`.
- Run `46` is the 100-commit observed-key/by-name proof result:
  - `cold/46`: mean `1.916949s`, median `0.451587s`, p90 `12.324962s`,
    max `15.339012s`.
  - `hot/46`: mean `0.358412s`, median `0.342008s`, p90 `0.407941s`,
    max `0.691247s`.
  - Outputs matched reference.
  - Artifact shape: `12` certs, `88` aliases, `12` guards, `12` store-path
    sidecars; proof cache size about `9.8M`.
- Run `47` is the fresh current-default comparison without observed-key proof:
  - `cold/47`: mean `2.973340s`, median `0.474036s`, p90 `12.371809s`,
    max `15.495280s`.
  - `hot/47`: mean `0.351630s`, median `0.329637s`, p90 `0.424899s`,
    max `0.662502s`.
  - Outputs matched reference.
  - Artifact shape: `21` certs, `79` aliases, `21` guards, `21` store-path
    sidecars; proof cache size about `12M`.
- Run `48` is a 10-commit smoke with the one-character by-name prefix fix
  included, before the final proof-key salt namespace cleanup:
  - `cold/48`: mean `4.255752s`; `hot/48`: mean `0.353649s`.
  - Outputs matched reference.
- Run `49` is the 10-commit smoke after the final v6 proof-key salt/cache
  namespace cleanup:
  - `cold/49`: mean `4.212740s`, median `0.413874s`, max `15.352804s`.
  - `hot/49`: mean `0.394822s`, median `0.369168s`, max `0.506383s`.
  - Outputs matched reference.
  - The cache contains `eval-trace-json-action-proof-v6`, confirming the final
    namespace is active.
- Run `50` is the 10-commit stats-enabled diagnostic after adding the observed
  proof counters:
  - `cold-debug/50`: mean `5.264016s`, median `0.399562s`, max `18.750928s`.
  - `hot-debug/50`: mean `0.387999s`, median `0.371667s`, max `0.470729s`.
  - Outputs matched reference.
  - Counter evidence: `cold-debug/50` had 7 proof hits / 3 misses, 8
    observed-key reparses accepted, 13 unchanged observed-key skips, and 8
    by-name directory changes accepted because the child key was unobserved.
    `hot-debug/50` had 10 proof hits / 0 misses, 8 observed-key reparses
    accepted, 22 unchanged observed-key skips, and the same 8 unobserved
    by-name accepts. Both runs had zero observed-key rejects.
- Run `51` is the no-stats smoke after counter wiring:
  - `cold/51`: mean `4.278933s`, median `0.392233s`, max `15.705556s`.
  - `hot/51`: mean `0.382477s`, median `0.367593s`, max `0.453223s`.
  - Outputs matched reference. This stayed in the run `49` timing band, which
    is expected because the new counters are disabled when `NIX_SHOW_STATS` is
    unset.
- Run `52` is the fresh 100-commit current-code observed-key result before
  adding `all-packages.nix` to the observed guard set:
  - `cold/52`: mean `1.918084s`, median `0.470405s`, p90 `12.065482s`,
    max `15.703052s`.
  - `hot/52`: mean `0.364078s`, median `0.342462s`, p90 `0.416886s`,
    max `0.689092s`.
  - Outputs matched reference.
  - Artifact shape: `12` certs, `88` aliases, `12` guards, `12` store-path
    sidecars; proof cache size about `9.8M`.
- Runs `53`/`54` added `pkgs/top-level/all-packages.nix` to the gated observed
  key guard candidates:
  - `cold/53`: mean `4.246710s`; `hot/53`: mean `0.365143s` over 10 commits.
    Outputs matched reference.
  - `cold/54`: mean `1.814479s`, median `0.488892s`, p90 `11.917055s`,
    max `15.148102s`.
  - `hot/54`: mean `0.387147s`, median `0.369583s`, p90 `0.448632s`,
    max `0.691525s`.
  - Outputs matched reference.
  - Artifact shape: `11` certs, `89` aliases, `11` guards, `11` store-path
    sidecars; proof cache size about `9.0M`.

Causal read:

- The observed-key/by-name proof is causal for cold improvement. The fresh
  default run still misses on `3f7b5d89ca3e` and `1575c9f64e00`; the observed
  proof turns both into proof hits.
- `cold/46` has only 12 slow stores over 100 commits, versus 21 in
  `cold/47`. Those 12 are first stores for output JSON values that did not yet
  have an accepted certificate in the run. Later reappearances of older output
  hashes are served by the existing alias/certificate machinery.
- This shows the design is not fundamentally incapable of beating the baseline:
  `cold/46` is far below regenerated `cold/0` (`1.916949s` vs `3.316277s`) and
  below the current default `cold/47` (`2.973340s`). Hot remains far below
  regenerated baseline (`0.358412s` vs `0.886366s`) but is slightly slower than
  current default (`0.351630s`), likely from larger git guard sidecars and
  observed-key verification overhead.
- The main abstraction that mattered was not lazy shape projection itself; it
  was proof-authorized negative/positive membership over the observed key
  universe. Shape/projection machinery remains useful as a source of observed
  key facts, but the performance win came from authorizing unchanged outputs
  across unrelated attrset/by-name churn.
- Adding `all-packages.nix` is causal for one remaining repeated-output cold
  miss: commit `e6e9864f360f` moved from a 12.591s store in `cold/52` to a
  0.5s proof hit in `cold/54`, aliased back to the existing
  `7fb36e13811a` certificate. That dropped slow cold stores from 12 to 11 and
  cold mean from `1.918084s` to `1.814479s`. The cost is a hot mean increase
  from `0.364078s` to `0.387147s`, still far below the regenerated
  `hot/0` baseline.

Adversarial semantic read:

- This is a gated prototype because it serves cached results across changes to
  files/directories whose full keysets changed. The soundness argument is only
  for the observed key universe, not for `attrNames`, missing-attr suggestions,
  or complete negative membership.
- The observed-key file guard is intentionally narrow. It relies on parsing
  simple non-recursive attrset files and binding the surrounding scope hash.
  If parsing fails, the file is not covered and the proof falls back.
- `all-packages.nix` is large but still matches the same syntactic guard
  model: function/let/function/with wrapping a non-recursive attrset. The
  extra guard increased a representative cert from about `153 KiB` to `220 KiB`
  but reduced total 100-commit proof-cache size because one full cert/guard
  pair became an alias.
- The by-name directory guard is also intentionally narrow. It only applies to
  `pkgs/by-name/<prefix>` directory-listing guards and exact file deps are
  checked before the relaxed directory rule. If an observed package's
  `package.nix` was read, the exact path guard still rejects a change.
- A positive by-name membership observation is not allowed to override a
  complete keyset observation for the same directory. If `attrNames` or an
  equivalent `#keys` dep was demanded, child add/delete/type changes must fall
  back even when some other code also asked `hasAttr` for a specific child.
- The complete-keyset check must ignore `ImplicitStructure #keys` facts emitted
  as creation-time guard fingerprints. Those are necessary for verifier
  recovery but do not mean the Nix expression observed the full keyset.
- The prototype does not prove that arbitrary attrset keyset changes are safe;
  it proves that, for this workload and these candidate files, unobserved-key
  churn can be ignored while preserving output correctness on the benchmark.
- Added `tests/functional/eval-trace-json-action-observed-key-proof.sh` for
  the first hardened slice. It builds a small nixpkgs-shaped Git repo with
  `pkgs/top-level/all-packages.nix`, seeds a v7 JSON action certificate for a
  derivation attrset, then checks:
  - same-head proof hits skip unchanged observed-key verification,
  - an unobserved binding change is served with `NIX_ALLOW_EVAL=0`,
  - deleting an unobserved binding in the candidate file is also served,
  - the unobserved-binding hit increments `nixObservedKeyAccepted` and not
    `nixObservedKeyRejected`,
  - a previously absent observed key appearing rejects under
    `NIX_ALLOW_EVAL=0`,
  - changing the observed derivation binding rejects under `NIX_ALLOW_EVAL=0`,
  - unsupported candidate-file syntax rejects/falls back instead of being
    treated as covered,
  - changing the surrounding candidate-file scope rejects even when the
    observed binding text is unchanged,
  - changing an imported non-candidate attrset file still rejects through the
    strict file guard,
  - an `aliases.nix`-style `lib: self: { ... }` candidate accepts unobserved
    alias churn and rejects observed alias binding changes,
  - dirty candidate-file changes and untracked dirty files disable proof action
    loading/publication by failing the clean-Git-HEAD key construction,
  - non-Git source trees do not enter the JSON action proof path at all
    (`load.attempts == 0`, `store.attempts == 0`) even when the observed-key
    experiment env var is set.
- The focused test passed when run manually through the dev shell with
  `NIX_CLIENT_PACKAGE=/home/connorbaker/nix/result` so the test used the
  current `result/bin/nix` instead of stale `build/src/nix`. It was rerun after
  adding the unobserved-delete, parse-failure, scope-change, non-candidate
  strictness, dirty-file, and untracked-file cases.
- Added `tests/functional/eval-trace-json-action-observed-directory-proof.sh`
  for the by-name directory half of the optimization. It builds a small
  `pkgs/by-name/he` tree where `all-packages.nix` observes the `he` prefix via
  `builtins.readDir`, then checks:
  - same-head proof hits,
  - adding an unobserved by-name child serves with `NIX_ALLOW_EVAL=0`,
  - that hit increments `observedDirectoryChecks` and
    `observedDirectoryAcceptedUnobserved`,
  - changing the imported observed package file rejects despite the surrounding
    directory relaxation,
  - changing an unobserved by-name child from a directory to a regular file is
    still served,
  - changing or removing an observed by-name child with a matching prefix
    rejects under `NIX_ALLOW_EVAL=0` and increments the observed-directory
    rejection counters,
  - the same accept/reject behavior works for a one-character by-name prefix
    directory (`pkgs/by-name/q`),
  - a by-name directory used as a complete keyset via
    `builtins.attrNames (builtins.readDir ...)` rejects an unobserved child
    addition instead of using the positive-membership relaxation,
  - the complete-keyset fixture also performs `builtins.hasAttr "hello"` on
    the same `readDir` result, pinning the adversarial mixed case where
    positive membership exists but must be suppressed because the keyset is
    also semantically demanded. The cold store stats assert
    `observedDirectorySuppressedKeyset >= 1`; the later `NIX_ALLOW_EVAL=0`
    replay asserts `guardRejected >= 1`.
- The focused observed-directory test also passed through the same dev-shell
  manual harness against the current `result/bin/nix`.
- Test-design note from the observed-directory hardening: a fake observed child
  must both be tracked by Git and map to the same by-name prefix. An empty
  directory is invisible to Git, and a name like `marker` belongs under
  `pkgs/by-name/ma`, not `pkgs/by-name/he`. The fixture now uses `herald` under
  `he` to exercise the intended proof path.
- 2026-05-24 follow-up validation: rebuilt `.#nix-cli` after tightening the
  by-name observed-child source, then reran both focused functional scripts
  through `nix develop` against the current `result/bin/nix`; both passed.
- Run `55` is a 10-commit no-debug/no-`NIX_SHOW_STATS` smoke after that
  tightening, with observed-key proof enabled:
  - `cold/55`: mean `4.435044s`, median `0.396564s`, max `16.008680s`.
  - `hot/55`: mean `0.372680s`, median `0.354371s`, max `0.440665s`.
  - Outputs matched `reference`.
  - Artifact shape stayed lean for the 10-commit run: `3` certs, `7` aliases,
    `3` guards, `3` store-path sidecars.
  - The key proof-hit smoke still holds: the by-name churn commit
    `3f7b5d89ca3e` ran cold in `0.383630s`, and the python top-level churn
    commit `1575c9f64e00` ran cold in `0.366561s`. That means deriving
    observed by-name children only from actual directory `hasKey` deps did not
    break the intended benchmark mechanism.
- Run `56` is the corresponding 100-commit no-debug/no-`NIX_SHOW_STATS`
  observed-key run after the same safety tightening:
  - `cold/56`: mean `2.031514s`, median `0.505508s`, p90 `12.857673s`,
    max `16.373397s`.
  - `hot/56`: mean `0.380999s`, median `0.377781s`, p90 `0.423760s`,
    max `0.664130s`.
  - Outputs matched `reference`.
  - Artifact shape: `12` certs, `88` aliases, `12` guards, `12` store-path
    sidecars.
  - This is intentionally slower than `cold/54` (`1.814479s`) because the old
    by-name mapping had accepted one extra repeated-output commit,
    `169bbf0bfcad`, by treating the global observed key `mi` as authority for
    the `pkgs/by-name/mi` directory and accepting a change to
    `pkgs/by-name/mi/mihomo/package.nix`. The tightened implementation no
    longer accepts that unless there is an actual structured directory
    `hasKey("mihomo")` observation for the prefix directory. That costs one
    cold store (`11` slow stores became `12`) but removes an authority
    overreach.
  - Even with that correction, the prototype remains well past the regenerated
    baseline gate: `cold/56` `2.031514s` vs `cold/0` `3.316277s`, and
    `hot/56` `0.380999s` vs `hot/0` `0.886366s`.
- Post-run `56` classification:
  - The remaining slow commits are exactly the fresh certificate stores. Most
    have different output JSON/context and are not alias opportunities.
  - `34513330fcc1` has only a direct docs diff, but relative to reusable older
    certs the accumulated diff includes observed exact package-file changes
    such as `pkgs/by-name/li/libvpx/package.nix` and
    `pkgs/by-name/ne/newt/package.nix`; the old note was about cert-base to
    current diff, not the commit's parent diff.
  - `169bbf0bfcad` has JSON/context equal to the earlier `a292268556ef` cert,
    but cert-base to current also includes a by-name package rename under
    `pkgs/by-name/ci`. Because that shard had no positive `hasKey` observation,
    the current proof correctly falls back rather than treating an arbitrary
    `readDir` keyset change as irrelevant.
  - This argues against broadening by-name relaxation further without a
    first-class "no complete keyset demanded" proof. The safe immediate action
    is the v7 hardening above: suppress relaxation when a complete keyset dep is
    known, not add more negative-membership behavior.
- Remaining focused tests before defaulting:
  - a benchmark smoke after any future change that makes observed-key proof the
    default rather than an opt-in experiment.

Next design leads:

- Keep the observed-key/by-name proof as the main cold lead. It materially beats
  the refreshed baseline and the current default cold path.
- Do not spend more time on child projection certs for this workload; the 100
  commit result shows the aggregate certificate plus observed membership proof
  is the profitable layer.
- Investigate the remaining slow stores as changed-output amortization or
  derivation/reachability authority, not broader keyset relaxation. After the
  by-name safety tightening this is 12 slow stores. Two instructive
  repeated-output misses are `34513330fcc1`, rejected by exact package-file
  changes (`pkgs/by-name/li/libvpx/package.nix` and
  `pkgs/by-name/ne/newt/package.nix` in the cert-base diff), and
  `169bbf0bfcad`, which has the same final JSON/context as `a292268556ef` but
  cannot reuse it because the cert-base diff contains an unobserved by-name
  child add/delete under `pkgs/by-name/ci`. Neither is safely addressable with
  observed keyset membership alone. The next gains need either an explicit
  proof that no complete keyset was semantically demanded for the affected
  by-name shards, a proof that a changed package file cannot affect the
  demanded top-level output, or a derivation output/reachability proof that is
  narrower than full evaluation.
- The cheap localized counters are now in place. Use them for the next
  hardening pass to make tests/assertions distinguish observed-key accepts,
  observed-key rejects, unchanged skips, and by-name unobserved child accepts.
- Practical validation note: direct `meson test -C build ...` currently trips a
  stale build-directory/toolchain mismatch (`stack protector mode differs in
  precompiled file` plus missing C++ standard headers). Focused functional
  scripts can still be run through `nix develop` with `NIX_CLIENT_PACKAGE`
  pointed at the latest `result` output, and `nix build .#nix-cli` remains the
  reliable compile gate for this branch state.

## Phase 1: semantic descriptors

- [x] Add task list and implementation plan.
- [x] Add `TraceQuery`, `TraceDemand`, `TraceProof`, `TraceResult`, and
  `TraceCacheEntry` descriptor types.
- [x] Add canonical descriptor digest builders.
- [x] Add persistent-exact-hit eligibility predicate.
- [x] Bind hash algorithm and digest size into descriptor digests.
- [x] Add typed object-reference primitives.
- [x] Add explicit little-endian wire helpers for new persistent formats.
- [x] Add tests for descriptor digest determinism.
- [x] Add tests proving volatile proofs are not persistently replayable.
- [x] Add tests proving locators/source hints do not become authority.

## Phase 2: immutable generation format

- [x] Add generation manifest/index/pack-reference value types.
- [x] Add generation manifest digest builder.
- [x] Add fail-closed index-record validation helpers.
- [x] Add RAM-resident sorted-index validation and candidate lookup helpers.
- [x] Add validated `GenerationIndex` wrapper for read-side index opening.
- [x] Add golden encoding tests.
- [x] Add corrupt/overflow index-record tests.
- [x] Add hash-collision/full-key-validation tests.
- [x] Replace external hash-blob constructors that rely on `assert` with
  fail-closed checked constructors.
- [ ] Bind result/probe sidecar decoders to expected capsule identity.

## Phase 3: shadow flat-snapshot backend

- [x] Add transitional `TraceBackendStorage` adapter seam so
  `TraceBackend` no longer publicly owns `SqliteTraceStorage` directly.
- [ ] Add read-only generation opener.
- [ ] Add RAM-resident sorted index lookup.
- [ ] Add pack payload `pread` reader.
- [ ] Add in-memory recorder buffer to generation writer.
- [ ] Add temp-generation writer and atomic publish path.
- [ ] Run shadow path beside current SQLite path with current conservative
  semantics.

## Phase 4: proof precision recovery

- [ ] Replace scalar source identity with source proof components:
  content, namespace, metadata, presentation/path, VCS/source-info, locators.
- [ ] Add clean Git tree proof accelerator using existing libgit2 dependency.
- [ ] Add generic filesystem projection proofs for file content, directory
  listing, path absence, symlink, and metadata.
- [ ] Add attrset proof components: presence, absence, key set, selected value,
  source position.
- [ ] Add per-process validation memoization for repeated source/proof checks.

## Phase 5: production cutover

- [ ] Replace production lookup/recovery with generation backend.
- [ ] Remove `SqliteTraceStorage` production dependency.
- [ ] Delete SQLite schema/lifecycle/control-plane code from production build.
- [ ] Keep SQLite only for diagnostic import/export if still useful.
- [ ] Rebaseline eval-trace-bench against `cold/900` and `hot/900`.

## Benchmark gates

- [ ] Shadow backend with current conservative semantics preserves `551/149`
  cold hit/miss behavior and improves `hot/904`.
- [ ] Proof descriptors recover at least `581` cold hits without output mismatch.
- [ ] Hot total wall is at or below `hot/900` (`95.70s` on the current bench).
- [ ] Cold total wall is at or below `cold/900` (`683.42s` on the current bench).

## Implementation notes

- `2026-05-17`: First slice intentionally avoids changing production
  acceptance behavior. Added proof descriptors, generation descriptors,
  checked digest blob construction, and RAM index primitives.
- `2026-05-17`: Added a transitional `TraceBackendStorage` adapter in
  `context.cc`. This removes direct public ownership of `SqliteTraceStorage`
  from `TraceBackend`, but verifier/recorder acceptance still depend on the
  SQLite concrete type internally. This is a seam, not the replacement.
- `2026-05-17`: Subagent review identified `EvalTraceHash::fromBlob` as a
  release-build safety issue. It now throws on malformed external blobs and
  exposes `tryFromBlob` for fail-closed decoders.
- `2026-05-17`: Generation index records require full-key validation and reject
  volatile-proof flags. Digest equality remains candidate routing only.

## Current limitations

- No on-disk generation opener/writer exists yet.
- No descriptor bytes are persisted yet; current descriptors are value objects
  and digest builders only.
- The verifier still takes `SqliteTraceStorage &`, so the flat backend cannot
  be wired into production until acceptance logic is separated from SQLite.
- Result/probe sidecar decoders still need expected-binding APIs before they
  are safe as reusable projection decoders.
- There is no benchmarkable behavior change yet; all changes are scaffolding
  and safety hardening.

## 2026-05-17 implementation notes

- Hardened descriptor primitives: demand/proof/result/cache-entry digests now bind schema and provider epochs; proof eligibility now calls structural validation before exact-hit acceptance.
- Hardened proof validation: unknown observation domains, polarities, precision kinds, object kinds, duplicate obligations, duplicate required object kinds, coverage-count mismatches, volatile observations, required feature bits, and non-exact obligation precision fail closed for persistent exact hits.
- Hardened generation index primitives: records now require exact `RequiresFullKeyValidation` flags, reject unknown flag bits, bind proof/result object refs to descriptor digests, recompute cache-entry digests, reject duplicate route keys, and support manifest-aware entry-count and pack-bound validation.
- Changed writer-side generation index construction to return `std::optional<GenerationIndex>` instead of relying on debug-only `assert` validation.
- Added explicit little-endian wire helpers for new persistent formats. Persistent cache files must not serialize native C++ structs, local intern IDs, pointers, padding, unordered iteration order, `NodeStamp`, or SQLite surrogate IDs.
- Added a concrete `TraceBackendStorage` adapter seam in `context.cc`: `TraceBackend` now forwards record/load/hash/root/flush operations through the seam and no longer reaches through to `SqliteTraceStorage`. `Verifier` is still SQLite-specific for this slice.
- Added runtime fail-closed behavior for `TraceBackend::verify()` before `bindSession()` rather than relying on debug-only assertions.

## Remaining implementation risks and follow-up tasks

- Persist descriptor bytes, not only descriptor digests. Index digests route candidates; decoded descriptor bytes and payload validation authorize replay.
- Add manifest/index/object binary encoders using `wire-encoding.hh`; validate magic, version, epochs, hash algorithm, digest sizes, file sizes, sortedness, duplicate keys, and object bounds before a generation becomes visible.
- Add fixed-width hot-head projection for exact-session hits so normal hot hits do not scan long historical candidate ranges.
- Add dirty-only append generation writer. Do not rewrite or merge old generations on normal shutdown; compaction is a separate explicit operation.
- Add crash-consistent publication: temp generation path, write packs, write index, write manifest last, fsync files, fsync generation directory, then atomically update a `current` ref.
- Add descriptor/capsule object validation path: object digest, descriptor digest, cache-entry digest, proof validation, result hash, trace hash, dep-key-set hash, full-trace hash.
- Add shadow snapshot mode before production cutover if behavior comparison against SQLite is needed; keep SQLite authoritative until shadow projection is zero-diff.
- Benchmark risks to track: descriptor hashing copies/sorts, duplicate candidate spans, cold full-index validation, pack decompression, and repeated canonicalization passes over deps.
- Added descriptor wire encoders/decoders for query, demand, proof, result, and cache-entry descriptors. Descriptor objects now have explicit magic/version/epoch framing and reject malformed proof bytes before they can authorize replay.
- Added generation manifest and index-record wire encoders/decoders. These are explicit little-endian frames; decoded index records must pass full fail-closed validation before use.
- Descriptor and manifest encoders now canonicalize set-like fields before writing bytes. This prevents caller vector order from leaking into persistent identity while descriptor validation still rejects duplicate proof obligations and duplicate required object kinds.
- Added whole-index encode/decode helpers for contiguous fixed-width `entries.idx` bytes, including manifest-aware validation. This is the first cold-load boundary for an immutable generation.
- Review fixes applied: wire decoders now use overflow-safe bounds checks, proof descriptor validation is no longer `noexcept`, proof/manifest/index decoders catch allocation failures and fail closed, standalone index records reject nonzero pack IDs, and `TraceBackend` construction rejects null storage at runtime.
- Next recommended implementation slice from research agents: a shadow-only generation store v0 independent of SQLite, plus a SQLite exporter/comparator later. It should write/open immutable manifest + entries index + descriptor packs, compare against SQLite-derived snapshots, and never affect replay semantics until zero-diff.
- Added `generation-store.hh/cc`: a standalone immutable generation directory layer with `manifest.etgm`, `entries.idx`, `proof.pack`, and `result.pack`. It validates file digests, manifest metadata, fixed-width index records, pack bounds, and descriptor-object digests on open.
- Added generation-store tests for write/open round trip, descriptor-object validation, and rejection of corrupted pack/index files. This remains shadow-only infrastructure; no replay path or SQLite path uses it yet.
- Generation index validation now has explicit hash-algorithm overloads. Manifest-aware open/write validates cache-entry digests using the manifest algorithm instead of the ambient process-global setting.
- `openGenerationStoreDirectory()` now validates every referenced proof/result descriptor object before returning. This is intentionally eager for shadow v0; later hot paths can move descriptor validation to candidate-finalist reads once the format is proven.

## Adversarial note: descriptor object coverage gap

- Current `generation-store` v0 persists and validates proof/result descriptor objects, but index records still carry only query/demand digests. This is not sufficient for replay authority under the stated full-key-validation model, because digest equality alone cannot validate stored query/demand descriptor bytes under a collision or future descriptor-version extension.
- Treat the current store as shadow-only infrastructure. Before any replay path can use it, the generation format must add stored query and demand descriptor object refs, or an equivalent descriptor-object table, and validate current query/demand descriptor bytes against those objects before accepting a candidate.
- Preferred redesign: replace `proof.pack` with a generic `descriptor.pack` or add query/demand object refs to the existing descriptor pack. Then `GenerationIndexRecord` routes by fixed-width digests but points to all descriptor bytes needed to recompute the cache-entry binding.
- Tightened generation v0 after adversarial review: fixed-width records now carry query, demand, proof, and result descriptor object refs. `proof.pack` was replaced by generic `descriptor.pack`; open validates all query/demand/proof/result descriptor bytes and recomputes their typed digests before returning.
- Added `appendPersistentExactGenerationEntry()` to build one canonical index record plus descriptor/result pack bytes from typed query/demand/proof/result descriptors. It rejects ineligible persistent exact proofs before mutating packs and computes all object refs/digests in one place for future SQLite shadow export.
- Added `resolveGenerationEntry()` so a generation record resolves to query/demand/proof/result descriptors plus a recomputed cache-entry descriptor. This is the exact comparison target the future SQLite shadow bridge should use; route indexes still do not authorize replay.
- Final review fixes: proof validation now recomputes coverage digest from obligations using the active/manifest hash algorithm; decoded cache-entry descriptors reject `requiresFullKeyValidation=false`; generation writer validates descriptor objects before writing; opener checks manifest byte counts against actual files; manifest parent digests are canonicalized at write and rejected when non-canonical on decode; stale `ProofPack` enum was renamed to `DescriptorPack`.
- Fixed flake exact-session fail-closed behavior: `FlakeTraceSessionConfigRequest` now records whether the source identity is exact, and flake trace-cache session config/reuse is disabled when the locked-flake fingerprint is unavailable. This avoids creating reusable exact-session cache keys from non-exact source identity.

## Current blocker for real SQLite shadow export

- The standalone generation format now has the right authority shape, but the current SQLite candidate snapshot does not retain the full new descriptor objects required to produce sound generation entries. `CandidateSnapshotRow` stores legacy proof summary fields (`proofDescriptorHash`, coverage, precision) plus trace/result IDs; it does not store or reconstruct `TraceQueryDescriptor`, `TraceDemandDescriptor`, or the full `TraceProofDescriptor` obligations under the new format.
- A sound bridge must first change the record/publication path to construct and retain query/demand/proof/result descriptors at publication time, or add an exporter that can derive them from the exact canonical dep stream without losing precision. A fake exporter from current summary fields would reintroduce digest-only authority and is unacceptable.

## 2026-05-17 descriptor-retention implementation notes

- Fresh `SqliteTraceStorage::publishRecord` now builds a full persistent-exact descriptor bundle before publishing the candidate row: query descriptor, demand descriptor, proof descriptor, and result descriptor. The bundle is sidecar metadata for immutable-generation/shadow-export work; current verifier and replay paths remain unchanged.
- Descriptor construction deliberately uses canonical semantic inputs rather than local ids: attr paths are fed through the canonical attr-path encoder, dependency keys are fed through canonical key hashing, and result descriptors are derived from the encoded result payload bytes plus aux context rather than materialized `CachedResult` state.
- Summary-only candidate rows are still valid for legacy SQLite/Git replay, but they are not sufficient authority for persistent exact generation export. A row must carry the full descriptor bundle and still pass `appendPersistentExactGenerationEntry`, which preserves the fail-closed checks for volatile inputs, unsupported features, non-exact precision, and `requiresFullKeyValidation`.
- Added focused test coverage for fresh candidate descriptor retention. The current test asserts that a freshly recorded candidate keeps the descriptor bundle, preserves a multi-obligation proof shape, and can derive a cache-entry descriptor with `requiresFullKeyValidation=true`.
- Subagent review reinforced that shadow generation should be an auditor/projection layer: build from an in-memory snapshot, write immutable descriptors/results, reopen only for byte-integrity checks, compare generated facts back to the snapshot, and publish counters/logs only. It must not become a verifier or fast-hit authority until full current-observation validation exists.
- Remaining semantic caveat: the query descriptor currently binds to the exact semantic/recovery session digests and requested canonical attr path. If evaluation settings beyond those session digests need first-class descriptor fields, add them before generation-backed serving is considered.

### Next implementation tasks

- Add the actual shadow-export path from complete `CandidateSnapshotRow` descriptor bundles into generation-store records. Skip and count summary-only rows.
- Add exporter tests: complete row exports to a resolvable generation entry; summary-only rows are skipped; volatile/ineligible proofs do not mutate packs.
- Add parity counters for generated entries, skipped summary-only rows, skipped ineligible proofs, and descriptor mismatches.
- Revisit proof well-formedness cost before enabling full-cache export by default; duplicate detection is currently simple and may become expensive on very large proofs.

## 2026-05-17 shadow-export snapshot bridge

- Added an in-memory persistent-exact generation export builder. It walks candidate rows under storage exclusivity, skips summary-only rows, derives a cache-entry descriptor from the full descriptor bundle, deduplicates by cache-entry digest, and appends only eligible entries through `appendPersistentExactGenerationEntry()`.
- The bridge intentionally returns packs and records as a snapshot rather than installing a serving path. This keeps generation output as audit/export material until a separate verifier can validate current observations and full keys.
- Added focused test coverage for complete candidate export into a resolvable generation entry. The oracle is `resolveGenerationEntry()`, which checks descriptor/result object bindings inside the generated packs.
- Duplicate candidate rows are collapsed by entry digest before pack mutation. This avoids emitting duplicate index records while preserving the important distinction that candidate-link parity must be checked separately by a future shadow auditor.
- Still missing: persistence of this snapshot into a durable generation directory during shutdown, mismatch counters wired to runtime stats, and tests for summary-only/ineligible rows once we expose a clean way to synthesize those rows without mutating production internals.

## 2026-05-17 fail-closed exporter test notes

- Added test-only accessors to drop a fresh row's persistent descriptor bundle and to mark a fresh proof descriptor volatile. These are intentionally narrow mutators used to exercise exporter fail-closed behavior without adding production shims.
- Added exporter regression tests for two invalid publication classes: summary-only candidate rows produce no generation records or pack bytes, and ineligible volatile proofs produce no generation records or pack bytes.
- This keeps the rule explicit: generation export is derived from complete, eligible descriptor bundles only. Legacy proof summary fields (`proofDescriptorHash`, coverage, precision) remain replay metadata, not generation authority.

## 2026-05-17 descriptor hot-path tightening

- After adversarial review, descriptor construction was moved off the fresh record path. Fresh candidate rows now record that persistent-exact descriptor source data is available, but they do not eagerly store the full query/demand/proof/result descriptor bundle.
- Descriptor bundles are built lazily from the candidate row's trace/result handles plus existing trace/result caches. This preserves the soundness boundary while avoiding per-record duplication of proof obligations and result descriptor state.
- Summary-only rows are represented by the absence of both a descriptor bundle and descriptor source availability. The exporter cannot synthesize generation entries from legacy summary hashes.
- Entry-level dedup now uses the typed `TraceCacheEntryDigest` key directly instead of allocating hex strings. Object-level pack interning is still a separate performance task.
- Remaining blocker before shutdown integration: proof-obligation construction still depends on live interning pools, so the current export builder remains a locked snapshot/audit helper. To build packs outside the storage lock, add a canonical dependency descriptor source that copies/serializes pool-independent key/value facts under lock, then performs hashing and pack mutation after releasing the lock.

## 2026-05-17 soundness review fixes

- Removed the stale `TraceRow::resultId == resultId` check from persistent-exact descriptor construction. A content-addressed trace can be paired with multiple result payloads, so descriptor construction must validate the requested trace/result pair through available trace/result material rather than treating the trace row's first result id as authority.
- Loaded candidate rows are now marked as descriptor-source candidates. They still export fail-closed: descriptor construction succeeds only when the full trace deps and result payload are available in memory. This lets verified/decoded historical rows participate without treating legacy summary hashes as authority.
- Persistent-exact generation export now fails closed for result kinds whose whole-value demand semantics are not yet represented precisely in the descriptor model: trivial, full attrset, failed, and list results. Current export is limited to scalar/string/path/float values until demand/projection descriptors are redesigned for containers and errors.
- Duplicate entry tracking now inserts only after `appendPersistentExactGenerationEntry()` accepts the descriptor bundle. Repeated ineligible entries are counted as ineligible rather than hidden by the duplicate filter.
- `PersistentExactGenerationExport` remains shadow/audit-only. It does not validate current observations, does not include trace capsule object authority, and must not be used as a serving cache hit path.

## 2026-05-17 additional regression coverage

- Added a regression test for same-trace/different-result publication: two candidates with identical dependency traces but different scalar result payloads must both export, because `TraceRow::resultId` is not authoritative for all result pairings of a content-addressed trace.
- Added a duplicate accepted-entry regression test: two identical accepted candidates should produce one generation index record and one duplicate counter increment.
- Added an explicit non-serving comment to the export snapshot type. The generation snapshot remains an internal consistency/audit artifact until current-observation validation, full-key validation, and trace/result capsule validation are part of the serving design.

## 2026-05-17 pool-independent source snapshot

- Added a pool-independent descriptor source snapshot for persistent-exact generation export. Under storage exclusivity we copy only framed semantic material: session/recovery digests, canonical attr-path material, encoded result payload, result hash, and per-observation key/value material.
- Generation descriptor construction and pack mutation now operate from the copied source snapshot. The test accessor snapshots under lock and builds packs after releasing the lock, which is the intended shutdown shape.
- The snapshot material intentionally contains framed bytes, not local intern ids. Dep key material resolves string/data-path/typed-key pool ids while the lock is held, then later descriptor hashing uses only the copied bytes.
- The current snapshot format is internal to the exporter/auditor. It is not a persistent wire format and must not be used as replay authority; serving still needs current-observation/full-key/capsule validation.
- Remaining improvement: move production call sites to the two-phase API once shutdown publication is added, then delete or privatize the legacy locked convenience wrapper.

## 2026-05-17 snapshot implementation tightening

- Generalized path-material snapshotting so both attr-path ids and structured data-path ids are resolved through the same framed component encoding.
- Replaced the new snapshot value visitor with direct variant checks to keep this path independent of helper visibility and easier to audit.
- The two-phase test accessor now exercises the intended lock boundary: copy source material while holding storage exclusivity, then build generation records and packs after releasing it.

## 2026-05-17 locked wrapper removal

- Removed the locked export convenience wrapper so callers cannot accidentally build descriptor packs while holding storage exclusivity. Callers must now explicitly take a `PersistentExactGenerationSourceSnapshot` under lock and pass it to `buildPersistentExactGenerationExportFromSnapshot()` after releasing the lock.

## 2026-05-17 focused review result

- Spawned a fresh read-only reviewer after the source-snapshot refactor settled. It reported no blocking compile/type/API or replay-soundness findings in the scoped files.
- Residual risk remains unvalidated because no build or test run was requested for this slice.

## 2026-05-17 generation pack object interning

- Added `GenerationPackBuilder`, which interns descriptor/result pack objects by `(object kind, descriptor digest)` while preserving the existing one-shot append API for tests and simple callers.
- `buildPersistentExactGenerationExportFromSnapshot()` now uses the pack builder. This avoids rewriting identical query/demand/proof/result descriptor objects across distinct generation records.
- Reuse validates that the existing object ref still points at identical pack bytes. A digest collision or corrupted builder state fails closed instead of aliasing different bytes under the same object ref.
- Export snapshots now report `internedObjectHits`, and the same-trace/different-result regression checks that shared query/demand/proof descriptors are actually reused.
- This does not solve full streaming/mmap, but it shrinks pack bytes and object validation work for partial hits/hot hits with shared query or proof structure.

## 2026-05-17 pack builder staging fix

- Tightened `GenerationPackBuilder` after review: object refs are now planned first, the complete `GenerationIndexRecord` is validated, and only then are new pack bytes and intern-map entries committed.
- This preserves the important invariant from the old builder: rejected entries should not leave orphan descriptor/result bytes behind. The builder reserves pack capacity before committing to reduce allocation failure risk during commit.
- Existing object reuse still compares referenced pack bytes before returning a shared ref; corrupted builder state or digest/byte mismatch fails closed.

## 2026-05-17 subagent architecture recommendation

- Architecture review recommends crash-safe shadow publication as the next major implementation slice, not serving and not mmap/streaming yet.
- Proposed publication shape: snapshot dirty sources under storage exclusivity, build generation packs outside the lock, write to an incoming temp directory, fsync files/directories, rename into immutable `generations/gNNN`, then atomically update/fsync a `CURRENT` ref. Reopen/resolve the generation before reporting success.
- Serving remains deferred until current-observation validation, full-key validation, and trace/result capsule validation are specified and tested. Pack consistency alone is not replay authority.

## 2026-05-17 build fix: unity helper names

## 2026-05-21 lazy exact-hit DAG and integration status

- Original target restated: beat the pre-schema SQLite cache baseline while
  preserving exact tracing soundness and precision. Matching the baseline is
  not enough; the serving path must avoid the verifier/replay cost that made
  the old hot path too slow.
- Baseline anchor for no-debug/no-stats short runs: `cold/938` and `hot/938`
  over the first 10 overlapping commits. Mean wall times were `6.05894s`
  cold and `0.880468s` hot.
- Current short run after the metadata-only generation opener, before a real
  fresh-process exact-hit serving path: `cold/1016` mean `6.22673s` and
  `hot/1016` mean `0.930418s` over the same commits. This is slower than the
  baseline, which confirms the new code is still scaffolding rather than a
  completed performance win.

### DAG

- A. Define authority format for exact hits.
  Depends on: none.
  Output: an immutable serving record that includes route digests, manifest
  binding, entry digest, result payload authority, full-key/current-observation
  validation material, version/feature bits, and explicit rejection rules.
- B. Capture pool-independent semantic source material.
  Depends on: A.
  Output: a shutdown snapshot containing canonical attr paths, query/demand
  descriptors, proof obligations with full validation material, result payload
  bytes, result hash, and trace/capsule binding.
- C. Build compact immutable exact-hit sidecar.
  Depends on: A and B.
  Output: sorted fixed-width routing records plus referenced authority/payload
  objects. The sidecar is a finalist router only; it never authorizes replay by
  itself.
- D. Persist and reopen generation metadata lazily.
  Depends on: C.
  Output: startup loads manifest, entries index, and exact-hit index only.
  Descriptor/payload objects are validated only for finalists.
- E. Fresh-process exact-hit acceptance.
  Depends on: A, C, D.
  Output: route by current query/demand, validate manifest/entry/object/payload
  bindings, validate current observations, decode the authoritative result
  payload, and replay deps only if the parent-capture model is represented.
- F. Parent-capture dep replay model.
  Depends on: A.
  Output: either compact replay material for `TraceContext`/parent-slot deps or
  a hard fail-closed eligibility gate that rejects them for exact-hit serving.
- G. Dirty-only publisher.
  Depends on: B and C.
  Output: build packs outside the storage lock, write immutable generation
  files, fsync, atomic publish, and reopen/validate before reporting success.
- H. Benchmark gate.
  Depends on: E, F, G.
  Output: no-debug/no-stats `eval-trace-bench` comparison against the selected
  baseline, first with 10 commits for iteration and then with 100 commits.

### Parallelization plan

- Worker 1 owns generation/export construction: build compact sidecar records
  from eligible rows and keep them sorted/deduplicated.
- Worker 2 owns lifecycle/persistence only after A is tightened. Do not persist
  descriptor-only authority as a serving format.
- Worker 3 owns adversarial review of acceptance invariants and benchmark
  implications.
- Integrator owns builds, tests, benchmark runs, and final architecture
  decisions.

### Adversarial correction from the 2026-05-21 review wave

- Descriptor-only generation entries are not sufficient serving authority.
  `TraceProofObligation` currently stores key/value digests, not the canonical
  validation bytes needed for full-key validation.
- `TraceResultDescriptor` is not result payload authority. A hot serving record
  needs a validated result payload object/ref or it must continue using the
  durable trace/result capsule path.
- `TraceContext` and parent-slot observations are not eligible for the fast
  exact-hit path until dep replay/current-parent validation is represented.
  Until then, reject them rather than silently serving stale child results.
- Generation ids, row ids, session ids, and manifest candidate rows are routing
  metadata only. Acceptance must bind through content digests and validated
  payload/current-observation material.
- The immediate next implementation direction is to redesign the exact-hit
  authority object before wiring lifecycle persistence or fresh-process serving.
  Persisting the current compact exact-hit sidecar is useful for audit/routing
  but must not be treated as replay authorization.

### 2026-05-21 verification and benchmark checkpoint

- `nix build -L . --builders ''` passed after the export-side exact-hit sidecar
  work. Expression tests: `1913` passed, `3` skipped. Functional tests: `218`
  passed, `8` skipped.
- Post-build short benchmark run: `1017`.
- Baseline comparison over the same 10 no-debug/no-stats commits:
  `cold/938` mean `6.05894s`, `hot/938` mean `0.880468s`.
- Current comparison:
  `cold/1017` mean `6.22842s`, `hot/1017` mean `0.918446s`.
- Interpretation: the current patch set is still a correctness/scaffolding
  slice. It has not yet implemented the fresh-process exact-hit serving path
  that the unsafe oracle suggested could recover roughly `0.24s` of hot time.

### Refined next implementation wave

- Add serving-specific authority objects rather than overloading descriptor
  records: `ExactHitAuthority`, `ObservationMaterial`, `ResultPayload`,
  `TraceBinding`, and later `ParentReplay`.
- Keep fixed indexes as routing only. A fixed exact-hit record may contain
  route digests and an authority object digest/ref, but replay authorization
  must come from lazy pack objects whose bytes and typed semantic digests are
  validated for the selected finalist.
- Add a serving-specific eligibility gate. For v1, the only serveable subset is
  no-observation scalar non-error results with exact current-node/session/source
  binding and a validated result payload/capsule binding. Every non-empty
  `TraceProofObligation` remains rejected until full key/value observation
  material and current-observation validators exist.
- Reject `TraceContext` and `TraceParentSlot` fast serving unless a future
  parent-replay object proves the current parent/sibling boundary. Do not use
  descriptor-only proof for these domains.
- Add finalist range reads or mmap for lazy object validation. Reading and
  hashing whole descriptor/result packs for one candidate would erase the
  benefit of metadata-only startup.
- Benchmark gate should wait until serving is actually enabled for the safe
  subset; otherwise 10-commit runs will continue to show scaffolding overhead
  rather than a real win.

- First compile attempt from the clean path reached libexpr compilation and failed in Meson unity mode because multiple `.cc` files used generic anonymous-namespace helper names (`feedU64`, `feedDomain`, `appendHash`, etc.).
- Renamed helpers in the new proof/generation files with file-specific prefixes so unity concatenation cannot collide with existing helpers such as trace-capsule serialization helpers.

## 2026-05-17 build fix: test depHash namespace

- Full package build progressed past libexpr and into libexpr tests. The new proof/generation tests used `test::depHash`, but the helper available through installed headers is `::nix::depHash`. Updated those tests to call the production helper directly.

## Benchmark note: regenerated pre-schema SQLite baseline vs current rewrite (2026-05-18)

Requested baseline commit: 92a3df1ab706cf514357ae57b367050b373ff146.

Chosen run numbers:
- Baseline pre-schema SQLite run: cold/906 and hot/906.
- Current rewrite run: cold/907 and hot/907.

Commands:
- Baseline build: nix build -L ".?rev=92a3df1ab706cf514357ae57b367050b373ff146#nix" --builders '' -o result
- Baseline bench: nix run path:/tmp/nix-eval-trace-build-src#eval-trace-bench -- generate --nix /home/connorbaker/nix --nixpkgs /home/connorbaker/nixpkgs --num-commits 100 --run-number 906
- Current build: nix build -L path:/tmp/nix-eval-trace-build-src#nix --builders '' -o result
- Current bench: nix run path:/tmp/nix-eval-trace-build-src#eval-trace-bench -- generate --nix /home/connorbaker/nix --nixpkgs /home/connorbaker/nixpkgs --num-commits 100 --run-number 907

Comparison result from eval-trace-bench:
- All outputs match reference.
- Baseline cold/906: 599 hits, 101 misses, 85.6% hit rate, 4.04s mean wall, 3.80s mean CPU.
- Current cold/907: 551 hits, 149 misses, 78.7% hit rate, 8.59s mean wall, 8.08s mean CPU.
- Baseline hot/906: 700 hits, 0 misses, 100% hit rate, 1.13s mean wall, 0.47s mean CPU.
- Current hot/907: 700 hits, 0 misses, 100% hit rate, 1.40s mean wall, 0.77s mean CPU.

Interpretation:
- Current rewrite is sound on this benchmark, but it is not meeting the performance goal.
- Cold regressed by about 2.13x wall-clock versus the regenerated pre-schema SQLite baseline.
- Hot regressed by about 1.24x wall-clock even with identical 100% hit rate, so the hot-path regression is fixed overhead rather than cache effectiveness.
- Cold cache effectiveness also regressed: 48 additional misses across 100 commits.
- Next investigation should separate two issues: why cold/907 lost hits versus cold/906, and why hot/907 has higher fixed overhead despite full hits.

## 2026-05-18 re-evaluation against corrected baseline

Corrected baseline: pre-schema SQLite commit `92a3df1ab706cf514357ae57b367050b373ff146`, regenerated as eval-trace-bench run `906`.
Current pre-verifier-reuse run: `907`.

Observed baseline gap:
- `cold/906`: 599 hits, 101 misses, 85.6%, mean wall 4.04s, mean CPU 3.80s.
- `cold/907`: 551 hits, 149 misses, 78.7%, mean wall 8.59s, mean CPU 8.08s.
- `hot/906`: 700 hits, 0 misses, 100%, mean wall 1.13s, mean CPU 0.47s.
- `hot/907`: 700 hits, 0 misses, 100%, mean wall 1.40s, mean CPU 0.77s.

Interpretation:
- Hot is a pure overhead regression because both baseline and current have 100% hits. The current verifier path was loading full trace deps twice for exact-session/async verification; the active patch reuses already-loaded deps and restores the verified-trace memo fast path.
- Cold is both overhead and precision: the current implementation rejects GitRevisionIdentity-governed structural candidates that the old implementation accepted. That is soundness-improving but too coarse; it makes several `closures.gnome.*` commits miss four attrs that baseline hit.
- The correct target is not simply “match `906`”: `906` itself is too slow and had weaker source/proof semantics. To beat it, the hot path must avoid SQLite-shaped row/projection work, and the cold path must recover safe reuse through a refined proof model rather than relaxing Git identity checks.

What has not yet affected production benchmarks:
- The generation-store/provenance work is currently export/audit infrastructure. It does not serve cache hits in the production verifier path yet, so it cannot improve eval-trace-bench until serving is wired in.

Next implementation pressure points:
- Short term: measure whether loaded-deps reuse closes the hot overhead gap; inspect stats if it does not.
- Medium term: redesign source/proof identity so result/value identity is not unnecessarily invalidated by stale source-session identity, while still preventing stale full-hash/proof propagation through TraceContext.
- Long term: move serving off SQLite rows onto immutable generation descriptors and fixed-width indexes: startup validates/open generations, hot exact hits read descriptor/result payloads directly, shutdown appends dirty generations only.

## 2026-05-18 run 908 after loaded-deps verifier reuse

Build status:
- Current build completed successfully.
- `nix-expr-tests-run`: 1904 passed, 3 skipped, no failures.

Benchmark comparison on the corrected 100-commit baseline set:

| run | hits | misses | hit rate | mean wall | mean CPU | mean loadTrace.count |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `cold/906` | 599 | 101 | 85.57% | 4.036s | 3.799s | 7.69 |
| `cold/907` | 551 | 149 | 78.71% | 8.589s | 8.080s | 43.87 |
| `cold/908` | 551 | 149 | 78.71% | 8.043s | 8.051s | 36.23 |
| `hot/906` | 700 | 0 | 100.00% | 1.131s | 0.473s | 7.00 |
| `hot/907` | 700 | 0 | 100.00% | 1.399s | 0.775s | 19.00 |
| `hot/908` | 700 | 0 | 100.00% | 1.181s | 0.762s | 12.00 |

Effect of the patch:
- Hot wall improved from `hot/907` by about 15.6%, and is now about 4.4% slower than `hot/906` by wall time.
- Hot CPU remains about 61% above `hot/906`, so the wall result is hiding remaining verifier/result/projection work.
- Cold wall improved from `cold/907` by about 6.4%, but remains about 99% slower than `cold/906`.
- Cold hit-rate did not improve; the lost cold hits are still semantic/proof-model related, not duplicate-load overhead.

Current conclusion:
- Loaded-deps reuse was a worthwhile local cleanup but cannot be the main performance path.
- The remaining cold gap is dominated by source/proof precision: the new GitRevisionIdentity guard avoids stale reuse but rejects candidates that baseline accepted.
- The remaining hot CPU gap is still overhead in serving exact hits through SQLite-shaped full-trace/result verification. To beat `906`, exact hot hits need a shorter descriptor/result path, not another pass over full deps.

Refined next steps:
1. Add an exact-hit descriptor fast path that validates a compact proof envelope before loading full deps/results. It must not weaken TraceContext/source identity semantics.
2. Redesign Git/source proof semantics so reusable result identity can survive irrelevant source-session changes, while proof identity remains current and cannot be propagated stale.
3. Promote generation-store descriptors from audit/export to serving authority for exact hits, then use SQLite only as fallback or remove it entirely after the replacement path is complete.
4. Keep `906` as the current corrected baseline until a new pre-schema baseline is explicitly regenerated.

## 2026-05-18 run 909 after early verified-session check

Change:
- Moved `VerificationSession::verifiedTraceIds` reuse ahead of `loadFullTrace` in both verifier entry points.
- Intent: repeated exact-session hits should not reload full dependency capsules once the trace has already been verified in the same session.

Build status:
- Current build completed successfully.
- `nix-functional-tests`: 218 passed, 8 skipped, 0 failed.
- `nix-expr-tests-run`: 1904 passed, 3 skipped, 0 failed.

Benchmark comparison:

| run | hits | misses | hit rate | mean wall | mean CPU | mean loadTrace.count | mean resultDecode.calls |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `cold/906` | 599 | 101 | 85.57% | 4.036s | 3.799s | 7.69 | 0.00 |
| `cold/908` | 551 | 149 | 78.71% | 8.043s | 8.051s | 36.23 | 10.44 |
| `cold/909` | 551 | 149 | 78.71% | 8.018s | 8.055s | 31.99 | 10.44 |
| `hot/906` | 700 | 0 | 100.00% | 1.131s | 0.473s | 7.00 | 0.00 |
| `hot/908` | 700 | 0 | 100.00% | 1.181s | 0.762s | 12.00 | 12.00 |
| `hot/909` | 700 | 0 | 100.00% | 1.185s | 0.762s | 7.00 | 12.00 |

Interpretation:
- The patch did exactly what it targeted: hot `loadTrace.count` is now back to `7`.
- It did not materially reduce wall or CPU, so `loadFullTrace` count was not the remaining hot bottleneck after run `908`.
- The remaining hot CPU gap appears to be result decode/materialization and verifier/orchestration overhead; `hot/909` still has `12` result decode calls per commit for `7` served attrs.
- Cold remains unchanged in hit rate and effectively unchanged in wall/CPU. The cold work is still proof-model/source-identity, not this local memo path.

Next implementation pressure point:
- Remove duplicate result decoding on hot hits. For a trace already verified in the current session, serving should decode/materialize exactly one result for the requested attr, not repeat any verification-time result decode. If that still does not move wall/CPU, the remaining overhead is likely coroutine/blocking-pool/store-access scheduling and the SQLite-shaped exact-hit path must be replaced with a direct generation descriptor/result path.

## 2026-05-18 run 910 after proof-only TraceContext verification

Built current changes locally from `/tmp/nix-eval-trace-build-src` and ran:

```bash
nix run path:/tmp/nix-eval-trace-build-src#eval-trace-bench -- generate --nix /home/connorbaker/nix --nixpkgs /home/connorbaker/nixpkgs --num-commits 100 --run-number 910
```

Compared against corrected pre-schema SQLite baseline run `906` from `92a3df1ab706cf514357ae57b367050b373ff146`.

Aggregate results:

```text
run       hits misses hitRate   wallMean   cpuMean   loadTrace verify alreadyVerified resultDecodeCalls resultDecodeUs verifyTraceCallUs pass1Us pass2Us storePathBatchUs
cold/906 599  101    85.57%    4.036135   3.798636  7.69      7      0               0                 0              0                 0       0       0
cold/909 551  149    78.71%    8.018229   8.055462  31.99     7      4.24            10.44             77062.51       236496.51         169238  35644   32822
cold/910 551  149    78.71%    8.011965   8.038363  31.99     7      4.24            5.51              77013.54       237917.50         170450  35827   32841
hot/906  700  0      100.00%   1.130831   0.472763  7.00      7      0               0                 0              0                 0       0       0
hot/909  700  0      100.00%   1.184620   0.761671  7.00      7      5.00            12.00             121053.61      252322.03         166447  52458   32292
hot/910  700  0      100.00%   1.219616   0.767046  7.00      7      5.00            7.00              131954.24      260237.41         173935  52433   32754
```

`eval-trace-bench runs` reported `PASS` for all 100 commits across `reference,cold/906,hot/906,cold/909,hot/909,cold/910,hot/910`.

Interpretation:

- The proof-only TraceContext verifier did what it was intended to do mechanically: hot result-decode calls dropped from `12` to `7`.
- It did not improve performance. Hot wall worsened from `1.184620s` to `1.219616s`; hot CPU stayed effectively flat/slightly worse (`0.761671s` to `0.767046s`).
- Cold was unchanged because hit-rate was unchanged (`551/149`, `78.71%`), and the same long source-identity-transition commits dominate wall time.
- Therefore ignored result materialization is not the decisive bottleneck. Remaining hot overhead is verifier/proof orchestration and dependency checking. Remaining cold overhead is mostly precision/hit-rate loss from source/proof identity semantics plus expensive failed candidate handling.

Adversarial conclusion:

- Do not continue optimizing around resultDecode call count alone.
- Deprioritize proof-only TraceContext verification as a performance optimization; keep only if it becomes necessary for a later semantic redesign.
- The current SQLite/verifier shape is unlikely to beat baseline by micro-optimizing loads/decodes. Run `909` already matched baseline loadTrace count on hot, yet CPU remained ~62% higher than baseline (`0.762s` vs `0.473s`).
- To beat baseline, the next change needs to alter semantics/architecture: separate semantic result identity from proof/source identity, recover stale-source structural hits safely, and eventually serve exact hits through compact descriptors or append-only packed generations rather than full verifier row fanout.

Parallel review findings:

- Both review agents converged on the same distinction: key cache reuse by evaluation semantic identity, not by proof-object identity.
- The likely winning backend is not a general database hot path; it is an immutable, append-only packed-generation cache with fixed-width indexes and compact proof envelopes, with SQLite either removed or kept only for diagnostics/import-export.
- A faster backend alone will not fix the cold gap if source identity remains over-constraining. Cold needs the `GitRevisionIdentity`/source-proof split first.

Refined next implementation direction:

1. Revert or quarantine the run-910 proof-only TraceContext path if it is only a performance change.
2. Add rejection counters/classification for stale source identity cases: stale Git identity only, content mismatch, TraceContext blocker, implicit structural mismatch, and result decode on rejected candidate.
3. Prototype the semantic/proof identity split: allow result/value reuse when value-affecting deps are entailed, but recompute or republish current trace identity before TraceContext consumers observe it.
4. Then prototype compact exact-hit descriptors so hot hits can validate a small proof envelope and decode only the served payload.
5. Treat packed append-only generations as the likely final serving backend once the semantic model is right.

## 2026-05-18 post-910 implementation adjustment

Applied two changes after run `910`:

1. Removed the proof-only `verifyForTraceContext` path.
   - Reason: run `910` reduced hot `resultDecode.calls` from `12` to `7` but worsened hot wall/CPU, so this is a negative performance optimization.
   - Restored TraceContext parent verification to `verify(...).has_value()` while preserving the earlier loaded-deps verifier reuse from runs `908/909`.

2. Added narrow implicit-guard recovery telemetry.
   - New counters split recovery implicit-guard checks/failures by `gitRevisionIdentity` and `implicitStructure`.
   - Purpose: distinguish source-identity blockers from structural-slice blockers in the expensive cold structural-variant path.
   - This is instrumentation only; it does not change acceptance semantics.

Expected benchmark shape for next run:

- Hot should return near `909` behavior: `resultDecode.calls ~= 12`, wall near `1.18s` if no unrelated noise.
- Cold hit rate should remain `551/149` until the source/proof identity model changes.
- The new telemetry should classify the expensive `recovery.acceptance.implicitGuardFailures` counts, especially for long commits like `acdd01...`, `66262d...`, `2b9b15...`, and the later source-transition block.

## 2026-05-18 run 911 corrected-baseline comparison and Git reidentity direction

Corrected baseline is run `906`, built from `92a3df1ab706cf514357ae57b367050b373ff146`:

- `cold/906`: 599 hits, 101 misses, 85.57% hit rate, wallMean 4.036135s, cpuMean 3.798636s.
- `hot/906`: 700 hits, 0 misses, 100% hit rate, wallMean 1.130831s, cpuMean 0.472763s.

Current post-schema run `911`:

- `cold/911`: 551 hits, 149 misses, 78.71% hit rate, wallMean 9.018615s, cpuMean 8.073477s.
- `hot/911`: 700 hits, 0 misses, 100% hit rate, wallMean 1.188227s, cpuMean 0.761904s.
- Recovery telemetry showed 128 implicit-guard failures, all `GitRevisionIdentity`; `ImplicitStructure` failures were zero.

Adversarial interpretation:

- The biggest cold regression is not generic SQLite writeback. It is a semantic recovery failure: structurally matching historical candidates are rejected because the source/proof identity guard differs.
- Returning the historical trace id would be unsound because it would erase current source identity. The acceptable form is reidentity: reuse the recovered result only after rebuilding a current dependency vector, confirming the current trace hash equals the selected candidate trace hash, and publishing a new trace with the current Git identity in the full proof vector.
- This is intended to improve cold hit rate back toward or beyond run 906 without weakening proof precision. The risk is that `GitRevisionIdentity` may be semantic in some path. The implementation must therefore fail closed unless all non-Git implicit guards match and all hash-contributing deps recompute to the selected trace hash.

Implementation note:

- Added a Git reidentity recovery path in verifier acceptance. On Git-only implicit guard mismatch, it resolves the candidate dependency keys against current state, rejects volatile/unresolvable deps, rejects non-Git implicit mismatches, recomputes the trace hash, decodes the historical result once, and records a new current trace with current deps.
- Added `gitReidentity` counters under `evalTrace.recovery.acceptance`.
- Removed a redundant recovery acceptance full-trace reload by passing the already loaded candidate deps into acceptance.

## 2026-05-18 run 912 after Git reidentity recovery

Built the current tree with `nix build -L .#nix-cli --builders ''` and generated benchmark run `912` with:

```bash
nix run .#eval-trace-bench -- generate --num-commits 100 --run-number 912
```

`eval-trace-bench runs` reported that all outputs match the reference.

Corrected baseline remains run `906`, built from `92a3df1ab706cf514357ae57b367050b373ff146`.

Aggregate comparison:

```text
run       hits misses hitRate wallMean  cpuMean  loadTraceMean verifyMean recoveryTimeTotal recordTimeTotal implicitGuardFailures
cold/906 599  101    85.57%  4.036135  3.798636 7.69          7.00       15.766s           4.757s          0
cold/911 551  149    78.71%  9.018615  8.073477 31.99         7.00       190.338s          16.378s         128
cold/912 602  98     86.00%  5.708138  5.614941 19.90         7.00       95.887s           15.388s         51
hot/906  700  0      100.00% 1.130831  0.472763 7.00          7.00       0.000s            0.000s          0
hot/911  700  0      100.00% 1.188227  0.761904 7.00          7.00       0.000s            0.000s          0
hot/912  700  0      100.00% 1.185637  0.768503 7.00          7.00       0.000s            0.000s          0
```

Interpretation:

- Git reidentity did the semantic job it was intended to do for this benchmark: cold hit count improved from `551/700` to `602/700`, slightly above the corrected baseline `599/700`.
- Performance is still not good enough. Cold wall is still `+1.672s` mean versus baseline, and hot CPU remains about `+0.296s` mean versus baseline.
- The cold bottleneck is no longer primarily hit rate. `cold/912` has better hit rate than `cold/906`, but it still does too much recovery/full-trace work: `loadTraceMean=19.90` versus `7.69`, and total recovery time is `95.887s` versus `15.766s`.
- Hot remains structurally inefficient even with perfect hits and `loadTraceMean=7`: the current verifier/proof/result path costs substantially more CPU than the pre-schema SQLite baseline.
- The `gitReidentity` counters did not appear in the exported dataframe under the expected name. The hit-rate/wall-time result proves the path is active, but the telemetry wiring needs to be fixed before relying on those counters.

Adversarial review update:

- Reidentity is sound only if it republishes a new current trace/proof envelope and never exposes the historical trace id after source identity changes.
- Reidentity must remain Git-only for now. Non-Git implicit mismatches, volatile deps, unresolved deps, and payloads whose value semantics depend on observable Git metadata must fail closed unless separately proven safe.
- Beating baseline requires reducing the cost of candidate serving, not broadening recovery acceptance.
- The next implementation target should be compact descriptor/proof prefiltering before full-trace load and an exact hot-hit fast path that validates small current proof metadata and decodes only the served result.

Refined task list:

1. Fix `gitReidentity` telemetry export so accepted/rejected/time counters are visible in `eval-trace-bench export`.
2. Classify remaining `cold/912` misses and long commits by candidate path: direct-hash miss, scan-history miss, implicit-guard mismatch, value-affecting mismatch, or first-time miss.
3. Add compact recovery descriptors that can reject or accept candidate classes before loading full traces. Target metric: bring cold `loadTraceMean` from `19.90` toward `7.69` without reducing hit rate.
4. Add an exact hot-hit fast path over compact proof descriptors. Target metric: keep hot `loadTraceMean=7` while reducing CPU from `0.768s` toward or below `0.473s`.
5. Re-run `eval-trace-bench` after each semantic/performance change and compare against run `906`, not just against recent runs.
6. Keep notes updated with attempts that fail; do not preserve compatibility shims or legacy bridge paths if a cleaner proof/cache model replaces them.

Parallel review finding:

- Independent review agrees that Git reidentity fixes a precision/hit-rate blocker but cannot beat the baseline by itself. The measured regression is now verifier/load/result-path cost: cold loads too many candidate traces, and hot spends too much CPU despite perfect hits.

## 2026-05-18 complete-probe structural-variant optimization

Adversarial review of the first implementation found an important soundness boundary:

- A trace capsule probe is a covering-index projection. It is useful for rejecting candidates, but it is not proof authority.
- The probe frame is stored inside the content-addressed capsule envelope, but prefix reads of external payload objects do not hash the full envelope before decoding the probe. Therefore, using a probe hash hit to accept or route a successful recovery would let a corrupted probe influence acceptance before the authoritative full capsule is validated.
- Tightened the optimization to reject-only: when a complete probe computes a current trace hash with no matching history bucket, skip the full capsule load. If the probe hash has any matching history bucket, fall through to the existing full-capsule structural-variant path.

This preserves the Git/CAS pack-index analogy: cheap index rows can reject, but base objects validate acceptance.

Benchmark plan:

- Run `914` is the first clean benchmark for the reject-only complete-probe optimization.
- Ignore partial run `913`; it was killed because it used the over-trusting probe-hit implementation.
- Compare `914` against `906` and `912` on: cold wall/CPU, hit count, `loadTrace.count/time/payloadBytes`, `structVariantCandidateLoadUs`, `structVariantProbeRejects`, `structVariantProbeMatches`, `recovery.timeUs`, and output equality.

## 2026-05-18 run 914 complete-probe result

Run `914` tested the safe reject-only complete-probe structural-variant optimization. It did not help.

Aggregate comparison:

```text
run       hits misses hitRate wallMean  cpuMean  loadTraceMean recoveryTimeTotal svCandidateLoads svProbeRejects svProbeMatches svCandidateLoadTotal
cold/906 599  101    85.57%  4.036135  3.798636 7.69          15.766s           0                0              0              0.000s
cold/912 602  98     86.00%  5.708138  5.614941 19.90         95.887s           1055             0              1053           53.103s
cold/914 602  98     86.00%  6.071956  5.630795 19.90         106.809s          1055             0              1053           59.231s
hot/906  700  0      100.00% 1.130831  0.472763 7.00          0.000s            0                0              0              0.000s
hot/912  700  0      100.00% 1.185637  0.768503 7.00          0.000s            0                0              0              0.000s
hot/914  700  0      100.00% 1.775273  1.075905 11.82         41.357s           450              0              450            26.571s
```

Interpretation:

- Probe rejects were zero. Every complete probe hash matched a history bucket, so the safe reject-only path always fell through to the full structural-variant path.
- The optimization added overhead without reducing full candidate loads. It has been removed.
- Partial over-trusting run `913` remains invalid and should be ignored.

Next hot-path implementation:

- Added a narrower exact-session Git proof check: if exact-session lookup selected the row and every recorded `GitRevisionIdentity` dep equals the current `SessionConfig.sourceIdentity`, accept Git/source coverage without recomputing the Git identity through libgit2.
- This is intentionally narrower than “trust all Git deps”: it fails closed without a session config, without a recorded Git identity dep, or if any recorded Git identity digest differs from the current exact session source digest.
- The next clean benchmark after rebuilding this change should be run `915`.

### 2026-05-18 run 915 exact-session source-identity result

Compared current exact-session/source-identity coverage against corrected baseline `906` and Git-reidentity run `912`.

- `cold/906`: 599/700 hits, wallMean 4.036135s, cpuMean 3.798636s, loadTraceMean 7.69, recovery 15.766s.
- `cold/912`: 602/700 hits, wallMean 5.708138s, cpuMean 5.614941s, loadTraceMean 19.9, recovery 95.887s, SV candidate-load total 53.103s.
- `cold/915`: 602/700 hits, wallMean 6.080384s, cpuMean 5.632087s, loadTraceMean 19.9, recovery 106.185s, SV candidate-load total 59.016s.
- `hot/906`: 700/700 hits, wallMean 1.130831s, cpuMean 0.472763s, loadTraceMean 7.
- `hot/912`: 700/700 hits, wallMean 1.185637s, cpuMean 0.768503s, loadTraceMean 7, exact Git-not-recoverable 100.
- `hot/915`: 700/700 hits, wallMean 1.392135s, cpuMean 0.780380s, loadTraceMean 7, exact proof hits 1, bypasses 1, current Git hits 1, current Git misses 99.

Conclusion: the narrow source-identity exact proof check is not enough for this workload. It mostly fails closed because the stored GitRevisionIdentity dep hash does not match the current session source identity in 99/100 hot cases, or because exact proof acceptance needs richer resolver material than the current full-trace proof path exposes. Cold remains dominated by structural-variant candidate loading, so the next implementation is a conservative SV representative pruning pass using existing `DepKeySetHash` metadata. This is a first-stage covering-index style optimization: same full dependency key set implies same trace-contributing key sequence, while final `TraceHashLookup` plus `acceptRecoveredTrace` remains authoritative.

### 2026-05-18 run 916 conservative SV key-set grouping

Implemented a conservative structural-variant representative pruning pass using existing `DepKeySetHash` metadata: for each attr, SV now loads at most one representative per full dependency key set, then still uses the normal recomputed `TraceHashLookup` plus `acceptRecoveredTrace` for authority.

Benchmark comparison:

- `cold/906`: 599/700 hits, wallMean 4.036135s, cpuMean 3.798636s, loadTraceMean 7.69, recovery 15.766s, SV candidates 463.
- `cold/915`: 602/700 hits, wallMean 6.080384s, cpuMean 5.632087s, loadTraceMean 19.9, recovery 106.185s, SV loads 1055, SV candidate-load total 59.016s, SV candidates 1400.
- `cold/916`: 602/700 hits, wallMean 5.571599s, cpuMean 5.331087s, loadTraceMean 14.2, recovery 59.196s, SV loads 485, SV candidate-load total 22.832s, SV candidates 719.
- `hot/916`: 700/700 hits, wallMean 1.253435s, cpuMean 0.765809s, loadTraceMean 7, exact proof hits 1, current Git hits 1, current Git misses 99.

Conclusion: the key-set grouping is sound under normal valid-store/collision-resistance assumptions and preserves hit count in this run. It materially reduces cold candidate loading, but not enough to beat `906`: full `DepKeySetHash` is too specific, and remaining cold time still comes from structural-variant recovery. The next cold step should persist/load a trace-contributing-only `StructHash` or a key-only projection so candidates differing only in non-trace-hash guards group together before payload inflation.

Adversarial review: the current first-representative-per-key-set implementation can lose recovery in degraded/corrupt-sidecar cases if the chosen representative cannot be loaded/resolved but a later same-key-set representative could. This is precision-only because final acceptance remains authoritative. If we keep this optimization, tighten by keeping a small vector per key-set group and trying the next representative only on missing/corrupt/uncomputable representative failure.

The exact-session source-identity check is not earning its cost: it misses 99/100 hot cases and did not move cold. It should be removed or gated behind richer descriptor/resolver material; descriptor bytes alone are insufficient because the fast path needs bound resolver material for env/store/trace-context/source checks.

### 2026-05-18 run 917 remove failed exact-source shortcut

Removed the exact-session Git/source-identity shortcut after run 915/916 showed it missed 99/100 hot cases and did not improve cold recovery. The path now remains fail-closed for Git/source coverage until descriptor-bound resolver material exists.

Final comparison for this series:

- `cold/906` baseline: 599/700 hits, wallMean 4.036135s, cpuMean 3.798636s, loadTraceMean 7.69, recovery 15.766s, SV candidates 463.
- `cold/912` Git reidentity: 602/700 hits, wallMean 5.708138s, cpuMean 5.614941s, loadTraceMean 19.9, recovery 95.887s, SV loads 1055, SV load total 53.103s.
- `cold/916` key-set grouping + failed exact shortcut: 602/700 hits, wallMean 5.571599s, cpuMean 5.331087s, loadTraceMean 14.2, recovery 59.196s, SV loads 485, SV load total 22.832s.
- `cold/917` key-set grouping + exact-source fail-closed: 602/700 hits, wallMean 5.303736s, cpuMean 5.293699s, loadTraceMean 14.2, recovery 56.617s, SV loads 485, SV load total 22.200s.
- `hot/906` baseline: 700/700 hits, wallMean 1.130831s, cpuMean 0.472763s.
- `hot/912`: 700/700 hits, wallMean 1.185637s, cpuMean 0.768503s.
- `hot/917`: 700/700 hits, wallMean 1.207743s, cpuMean 0.762108s.

Status: key-set grouping is worth keeping as a conservative first step because it preserves hit count and cuts SV candidate loads by ~54% (1055 -> 485) and recovery time by ~41% versus run 912 (95.887s -> 56.617s). It still does not meet the goal of beating pre-schema baseline `906`. The remaining cold gap is still SV/recovery work; the remaining hot gap is exact-hit verification/materialization CPU.

Refined next steps:

1. Cold: persist/load trace-contributing-only `StructHash` or a key-only projection so SV groups by the exact trace-hash key sequence, not full dep key set. This should group candidates that differ only by non-contributing guards and may close more of the 485 remaining candidate loads. Keep final `TraceHashLookup` + `acceptRecoveredTrace` as authority.
2. Cold precision hardening: if retaining key-set grouping, store a short representative list per group and fall back to the next representative only on missing/corrupt/uncomputable representative failure.
3. Hot: build descriptor-backed exact-hit preflight with bound resolver material. The existing descriptor hashes are not enough: source/env/store/trace-context rechecks need concrete resolver material, and result replay needs result-sidecar descriptor validation without full trace load.
4. Remove or bypass result decode/full-trace fanout after exact descriptor acceptance. Hot `loadTraceMean` is already 7, but CPU remains ~61% above baseline; the fast path has to avoid full proof decode/verification, not just improve hit routing.

## Benchmark note: run 918 probe structural-key shortcut

Built current tree after adding an explicit `kTraceCapsuleProbeMaxDeps` wire-contract constant and using complete trace probes as a structural-hash prefilter during structural-variant recovery.

Comparison against corrected baseline `906` and prior current run `917`:

| run | hits | misses | hit % | wall mean | cpu mean | loadTrace mean | recovery total | SV loads | SV load total | SV candidates | probe matches | probe load total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| cold/906 | 599 | 101 | 85.57 | 4.036135 | 3.798636 | 7.69 | 15.766s | 0 | 0s | 463 | 0 | 0s |
| cold/917 | 602 | 98 | 86.00 | 5.303736 | 5.293699 | 14.20 | 56.617s | 485 | 22.200s | 719 | 483 | 0.590ms |
| cold/918 | 602 | 98 | 86.00 | 5.342512 | 5.292128 | 14.20 | 60.115s | 485 | 22.988s | 719 | 483 | 2.767ms |
| hot/906 | 700 | 0 | 100.00 | 1.130831 | 0.472763 | 7.00 | 0s | 0 | 0s | 0 | 0 | 0s |
| hot/917 | 700 | 0 | 100.00 | 1.207743 | 0.762108 | 7.00 | 0s | 0 | 0s | 0 | 0 | 0s |
| hot/918 | 700 | 0 | 100.00 | 1.208682 | 0.763657 | 7.00 | 0s | 0 | 0s | 0 | 0 | 0s |

Conclusion: the per-recovery probe structural-key shortcut is not sufficient. It preserves soundness and hit rate but only shifts work into probe loading/hashing; it does not reduce `structVariantCandidateLoads`, `loadTrace.count`, or recovery search cost. The next implementation target should be an in-memory serving/index redesign: build complete indexes once at startup/load time and make exact/hot and structural-variant lookups avoid repeated trace/probe materialization.

## 2026-05-18 run 919: probe fast path plus fail-closed source identity

Build status: `nix build -L . --builders ''` passed before benchmarking. The eval-trace tests enforced that stale Git/source identity must miss rather than be reidentified through structural-variant recovery.

Benchmark command: `nix run .#eval-trace-bench -- generate --num-commits 100 --run-number 919`.

Comparable runs:

| run | hits | misses | hit % | wall mean | cpu mean | loadTrace mean | recovery total | SV loads | SV load total | SV candidates | probe matches | probe load total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| cold/906 | 599 | 101 | 85.57 | 4.036135s | 3.798636s | 7.69 | 15.766s | 0 | 0s | 463 | 0 | 0s |
| cold/917 | 602 | 98 | 86.00 | 5.303736s | 5.293699s | 14.20 | 56.617s | 485 | 22.200s | 719 | 483 | 0.590ms |
| cold/918 | 602 | 98 | 86.00 | 5.342512s | 5.292128s | 14.20 | 60.115s | 485 | 22.988s | 719 | 483 | 2.767ms |
| cold/919 | 551 | 149 | 78.71 | 7.536290s | 7.427135s | 18.53 | 93.711s | 837 | 47.505s | 1180 | 835 | 5.122ms |
| hot/906 | 700 | 0 | 100.00 | 1.130831s | 0.472763s | 7.00 | 0s | 0 | 0s | 0 | 0 | 0s |
| hot/917 | 700 | 0 | 100.00 | 1.207743s | 0.762108s | 7.00 | 0s | 0 | 0s | 0 | 0 | 0s |
| hot/918 | 700 | 0 | 100.00 | 1.208682s | 0.763657s | 7.00 | 0s | 0 | 0s | 0 | 0 | 0s |
| hot/919 | 700 | 0 | 100.00 | 1.209488s | 0.763568s | 7.00 | 0s | 0 | 0s | 0 | 0 | 0s |

Finding: the probe fast path did not shrink the bottleneck. After fail-closed source identity semantics, many previous structural-variant recoveries correctly become misses, dropping cold hit rate and increasing total recovery time. This means the previous path was faster partly because it accepted stale source identities. That is not an acceptable optimization.

Adversarial review note from the parallel reviewer: grouping by one representative per dep-key-set hash is sound under valid capsules and collision-resistant hashes, because acceptance still goes through trace-hash lookup and implicit guard validation. It can lose precision in degraded/corrupt-sidecar cases if the chosen representative is unloadable while another equivalent candidate is usable. A robust implementation should either retain a small fallback list per key-set group or make the sidecar complete enough that the representative contains all data needed for authoritative matching.

Decision: this path cannot beat the baseline while preserving soundness. The next viable direction is to stop treating source identity as something recoverable by structural variants. Source identity needs to be represented as a direct, cheap, canonical key in the cache index. Structural-variant recovery should only handle dependency-shape changes for observations that are not invalidated by changed source/proof identity.

## 2026-05-18 run 920: restore content-proof Git reidentity

Build status: `nix build -L . --builders ''` passed. Eval-trace unit suite: 1907 tests run, 1904 passed, 3 skipped. Functional suite: 218 passed, 8 skipped.

Change: restored the documented semantics that a stale `GitRevisionIdentity` rejects the GitIdentity accelerator, but does not poison DirectHash/SV recovery when trace-contributing deps recompute to the same `TraceHash`. In that case recovery re-records a new current capsule instead of promoting the stale capsule. GitIdentity-only stale rows still miss because there is no content proof.

Benchmark command: `nix run .#eval-trace-bench -- generate --num-commits 100 --run-number 920`.

Comparable runs:

| run | hits | misses | hit % | wall mean | cpu mean | loadTrace mean | recovery total | direct hits | direct time | SV hits | SV time | SV loads | SV load total | SV candidates | probe matches | probe load total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| cold/906 | 599 | 101 | 85.57 | 4.036135s | 3.798636s | 7.69 | 15.766s | 56 | 2.328s | 20 | 9.819s | 0 | 0s | 463 | 0 | 0s |
| cold/917 | 602 | 98 | 86.00 | 5.303736s | 5.293699s | 14.20 | 56.617s | 16 | 7.059s | 35 | 47.028s | 485 | 22.200s | 719 | 483 | 0.590ms |
| cold/918 | 602 | 98 | 86.00 | 5.342512s | 5.292128s | 14.20 | 60.115s | 16 | 7.013s | 35 | 50.619s | 485 | 22.988s | 719 | 483 | 2.767ms |
| cold/919 | 551 | 149 | 78.71 | 7.536290s | 7.427135s | 18.53 | 93.711s | 0 | 5.328s | 0 | 85.594s | 837 | 47.505s | 1180 | 835 | 5.122ms |
| cold/920 | 602 | 98 | 86.00 | 5.485207s | 5.319444s | 14.20 | 65.281s | 16 | 6.934s | 35 | 55.780s | 485 | 25.601s | 719 | 483 | 3.784ms |
| hot/906 | 700 | 0 | 100.00 | 1.130831s | 0.472763s | 7.00 | 0s | 0 | 0s | 0 | 0s | 0 | 0s | 0 | 0 | 0s |
| hot/920 | 700 | 0 | 100.00 | 1.206316s | 0.765192s | 7.00 | 0s | 0 | 0s | 0 | 0s | 0 | 0s | 0 | 0 | 0s |

Finding: run 920 fixes the 919 semantic regression and restores the previous cold hit rate, but it is still slower than both the corrected pre-schema baseline and run 917. The dominant remaining cost is structural-variant recovery: 35 SV hits cost 55.780s total and still perform 485 full candidate loads. The probe optimization added so far does not remove those loads and therefore does not address the bottleneck.

Next direction: make the SV candidate projection truly covering for the acceptance path. If a probe contains every dep needed to recompute trace_hash plus the implicit guards needed for reidentity/guard validation, SV can avoid inflating full trace capsules for most candidates. The probe must remain tied to immutable capsule bytes/proof material; mutable candidate metadata cannot become authority.

## 2026-05-18 run 921: full dependency probe experiment

Build status: `nix build -L . --builders ''` passed. Eval-trace unit suite remained green: 1907 tests run, 1904 passed, 3 skipped.

Change tested: expanded trace probe sidecars from trace-hash-contributing deps to a bounded full dependency projection, and used complete probes during structural-variant acceptance to try to avoid full capsule loads.

Benchmark command: `nix run .#eval-trace-bench -- generate --num-commits 100 --run-number 921`.

Result vs 920:

| run | hits | misses | hit % | wall mean | cpu mean | recovery total | direct hits | direct time | SV hits | SV time | SV loads | SV load total | SV candidates | probe matches | probe load total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| cold/920 | 602 | 98 | 86.00 | 5.485207s | 5.319444s | 65.281s | 16 | 6.934s | 35 | 55.780s | 485 | 25.601s | 719 | 483 | 3.784ms |
| cold/921 | 602 | 98 | 86.00 | 5.508829s | 5.303903s | 66.627s | 16 | 6.937s | 35 | 57.211s | 485 | 26.592s | 719 | 483 | 3.708ms |
| hot/920 | 700 | 0 | 100.00 | 1.206316s | 0.765192s | 0s | 0 | 0s | 0 | 0s | 0 | 0s | 0 | 0 | 0s |
| hot/921 | 700 | 0 | 100.00 | 1.204665s | 0.764480s | 0s | 0 | 0s | 0 | 0s | 0 | 0s | 0 | 0 | 0s |

Finding: no benefit. The full-probe expansion did not reduce `structVariantCandidateLoads` at all and increased cold wall/SV time slightly. This implies the matched acceptance path still falls back to full trace loads in the cases that matter, or the expensive work is elsewhere in SV resolution. Keeping this would add format churn and sidecar size without measurable payoff, so it should be removed.

## 2026-05-18 run 923: reidentity result-id reuse experiment

Change tested:
- Added `SqliteTraceStorage::recordCurrentTraceWithExistingResult` and changed GitRevisionIdentity reidentity promotion to publish a current proof capsule using the already-authorized `ResultId` instead of going through the normal fresh `record` path.
- Semantics preserved: stale GitRevisionIdentity capsules are still not promoted as current exact-session proof; a new current-deps proof object is published. The result object remains immutable content addressed data, matching the action-cache/CAS split.

Benchmark:
- Baseline remains `cold/906` and `hot/906`, built at `92a3df1ab706cf514357ae57b367050b373ff146`.
- Previous current build: `cold/922`, `hot/922`.
- This experiment: `cold/923`, `hot/923`.

Results:
- `cold/906`: 599 hits / 101 misses, wallMean 4.036135s, cpuMean 3.798636s, recovery 15.766s, SV 20 hits / 9.819s, direct 56 hits / 2.328s, loadTraceTotal 769.
- `cold/922`: 602 hits / 98 misses, wallMean 5.495138s, cpuMean 5.304489s, recovery 67.194s, SV 35 hits / 57.587s, direct 16 hits / 6.939s, loadTraceTotal 1420.
- `cold/923`: 602 hits / 98 misses, wallMean 5.514564s, cpuMean 5.298504s, recovery 66.818s, SV 35 hits / 57.296s, direct 16 hits / 6.941s, loadTraceTotal 1420.
- `hot/906`: 700 hits / 0 misses, wallMean 1.130831s, cpuMean 0.472763s, verify 32.985s.
- `hot/922`: 700 hits / 0 misses, wallMean 1.205860s, cpuMean 0.765260s, verify 55.205s.
- `hot/923`: 700 hits / 0 misses, wallMean 1.204134s, cpuMean 0.763197s, verify 55.317s.

Conclusion:
- Reusing the existing result id during Git reidentity is semantically clean but not a performance win in this benchmark.
- The dominant cold regression remains candidate verification/recovery shape, especially SV recovery: current runs do 35 SV hits and 719 SV candidates versus baseline 20 SV hits and 463 candidates, with loadTraceTotal 1420 versus 769.
- The dominant hot regression is exact verification/proof work: current hot verifies 1200 passed traces versus baseline 700 and spends ~55s aggregate verify time versus ~33s.
- Next useful target is not result serialization. It is reducing duplicate/full verification work and preventing DirectHash-capable rows from falling into the expensive SV path.

## 2026-05-18 benchmark note: probe sidecar cap 65536 (run 925)

Run 925 tested raising `kTraceCapsuleProbeMaxDeps` from 4096 to 65536. It preserved cold hit rate (602/700, 86%) but regressed performance: cold wall mean 5.769s vs 5.515s in run 923 and 4.036s in baseline run 906; hot wall mean stayed roughly unchanged at 1.204s vs baseline 1.131s. The cap did reduce `evalTrace.loadTrace.count` from 1420 to 937, but SV recovery time increased from 57.296s to 84.557s and verify time increased from 125.777s to 154.010s. Conclusion: larger complete probes are not sufficient in this form; the extra sidecar work costs more than the avoided full trace materialization. Reverted the cap to 4096.

## 2026-05-18 no-debug eval-trace-bench baseline

- Harness mode: default/plain benchmark mode, with no `--debug`, no `NIX_SHOW_STATS`, and process wall time recorded in `timing.json`.
- Baseline build: `nix build -L ".?rev=92a3df1ab706cf514357ae57b367050b373ff146"`.
- Baseline run number to use going forward: `906` (`reference`, `cold/906`, `hot/906`).
- Current build/run for comparison: `nix build -L . --builders ''`, then `cold/927` and `hot/927`.
- Soundness result: all outputs matched `reference`.
- Wall mean across 100 commits: `reference` 6.07s, baseline `cold/906` 3.41s, baseline `hot/906` 0.90s, current `cold/927` 4.50s, current `hot/927` 1.37s.
- Interpretation: the no-debug/no-stats harness confirms stats/debug were distorting wall time, but current changes still regress the pre-schema SQLite baseline: cold is about 32% slower than `cold/906`, hot is about 52% slower than `hot/906`.

## 2026-05-18 no-debug benchmark: exact-session complete-probe preflight slice

Built current tree locally with `nix build -L . --builders ''` after converting complete non-zero dependency probes into exact-session proof projections. Expr test result: 1904 passed, 3 skipped, 2 disabled.

Benchmark run: `928` generated with `nix run .#eval-trace-bench -- generate --num-commits 100 --run-number 928 --runs cold,hot` using default no-debug/no-stats mode.

Comparison against no-debug baseline `906` and previous current run `927`:

- `reference`: mean 6.0685s, median 6.0145s.
- `cold/906`: mean 3.4074s, median 0.9561s, max 13.6314s.
- `hot/906`: mean 0.9027s, median 0.9046s, max 1.0002s.
- `cold/927`: mean 4.5008s, median 1.3742s, max 15.6896s.
- `hot/927`: mean 1.3693s, median 1.3655s, max 1.5257s.
- `cold/928`: mean 4.7432s, median 1.4759s, max 16.0654s.
- `hot/928`: mean 1.2684s, median 1.2726s, max 1.3209s.

Interpretation:

- Exact-session complete-probe preflight improved hot wall time versus the immediately previous current run by about 7.4% (`1.3693s -> 1.2684s`).
- It still misses the pre-schema baseline by about 40.5% on hot (`1.2684s` vs `0.9027s`).
- Cold got worse by about 5.4% versus `927` and remains about 39.2% slower than `906`.
- The cold regression is consistent with larger capsule/probe payloads and writeback/export pressure; this slice should not be treated as sufficient for the overall goal.

Subagent/review notes folded into next-step design:

- Generalize Git-specific recovery metadata into a source-identity/source-guard digest selection index. Use it as a candidate filter, not authority; immutable capsules/proofs still authorize replay.
- Add tests for stale source identity yielding direct source-index miss with zero SV candidate loads, source key persistence across SQLite/generation reload, multi-repo canonicalization, and metadata-corruption non-authority.
- Watch the dep-key-set representative optimization: selecting only the first representative per `DepKeySetHash` is sound under valid sidecars and collision resistance, but can lose precision in degraded/corrupt-sidecar cases if a later representative would have loaded. Either retain fallback representatives or document/guard the degraded-store precision tradeoff.
- Carry `keySetHash` in history entries to avoid an extra header lookup/allocation pass during structural-variant grouping.

Next performance implication:

- To beat baseline, the next change should reduce cold writeback/export and startup selection cost, not just make hot proof projection faster. The strongest candidate is a compact direct source-identity index plus append-only/delta writeback avoidance for rows already loaded from the database.

## 2026-05-18: no-debug/no-stats benchmark run 930

Baseline remains pre-schema SQLite run `906` from rev `92a3df1ab706cf514357ae57b367050b373ff146`.

Run `930` was generated after carrying `keySetHash` in recovery history and using complete exact probes as authoritative dep vectors only after recomputing and matching the stored full trace tuple.

Results from `eval-trace-bench runs --runs cold/906,hot/906,cold/930,hot/930`:

- Soundness: all outputs match reference.
- `cold/906`: mean wall `3.41s`.
- `cold/930`: mean wall `4.69s`, `1.38x` slower than baseline.
- `hot/906`: mean wall `0.90s`.
- `hot/930`: mean wall `1.22s`, `1.36x` slower than baseline.

Conclusion: the history metadata/probe optimization is correct but materially insufficient. Cold still has large outliers in the same commit bands as the baseline, but each outlier is slower; hot remains a broad fixed overhead regression. The next direction should not be incremental load/header shaving. We need to reduce candidate discovery and recovery validation work by making source identity a cheap direct selection key, and we should keep the source-key metadata non-authoritative: immutable capsule/proof data still authorizes replay.

Subagent adversarial review notes:

- Add a generalized `SourceIdentityDigest` / source-guard digest, not a Git-only metadata path.
- Index recovery candidates by `(attrPathId, sourceIdentityDigest)` so stale-source candidates miss cheaply instead of entering DirectHash/SV scans.
- Persist/load that digest through SQLite and generation storage, but treat it as an index only; corrupted metadata must fail closed under immutable dep/capsule validation.
- Current SV grouping by `DepKeySetHash` is sound under valid headers and collision resistance, but a degraded/corrupt first representative can cause a precision false negative because later same-keyset representatives are discarded. If we keep grouping, fallback should try another representative when the chosen one cannot load or fails non-authoritative probe validation.

## 2026-05-18 source-identity adversarial fixes

- Source-identity routing digests now bind stable governing repo-root bytes rather than local `RepoRootId` integers. This closes a false-miss class where startup load order or concurrent/imported string interning could allocate different local ids for the same repo root.
- `extractSourceIdentityDigest` is now deliberately weaker than Git recovery acceptance: it records an internally consistent source observation for candidate routing, while `allDepsGitRecoverable()` remains the acceptance precondition. Candidate rows remain indexes/hints; immutable trace capsules remain authority.
- GitIdentity recovery now times source-index lookup separately and avoids a second full-trace load by accepting from the already loaded candidate deps.
- Added regression coverage for local-intern-id-independent source digests and for source-identity extraction not implying recovery coverage.

## 2026-05-18 immutable fullHash capsule fix

- Adversarial review found that `governingRepoId` is excluded from trace identity but serialized in trace capsules. Re-publishing an existing `fullHash` with different governing-repo hints could overwrite the in-memory capsule stream and later conflict with the DB/object-store `(full_hash,result_hash)` payload guard.
- Fixed publication so content-addressed trace objects are immutable after first publication. Existing `fullHash` records keep their original dep stream; later candidate observations derive their source-identity routing hint from that immutable object when available, or fail closed to no source routing hint.
- Added a regression where two publications have the same logical full hash but different file-dep governing-repo hints. The second candidate must not rewrite the first capsule annotation.

## 2026-05-18 benchmark iteration workflow

- Updated `eval-trace-bench generate` so `--run-number` is optional. When omitted, it scans the planned non-reference run directories and chooses the next unused numeric run id across all of them.
- Kept the default benchmark size at 10 commits. This is now the intended fast iteration path; use `--num-commits 100` only after a 10-commit run looks promising.
- Did not add early termination. Truncated benchmarks should be a human decision so partial datasets do not masquerade as comparable full runs.

## 2026-05-18 fast benchmark loop results

- `eval-trace-bench generate` now auto-selected `932` and ran the default 10-commit workflow successfully.
- `eval-trace-bench runs --reference cold/932 --runs cold/906,hot/906,cold/932,hot/932` now compares the short run against the 100-commit baseline using the short run's manifest window.
- 10-commit result: `cold/932` mean 7.10s vs baseline `cold/906` mean 6.01s; `hot/932` mean 1.32s vs baseline `hot/906` mean 0.91s. Outputs matched reference.
- Interpretation: the extra cost is visible even on fast cold cases and hot cases, so the next target should be fixed startup/shutdown/store overhead, especially unnecessary load/writeback work on hot hits, not only cold miss recovery strategy.

### 2026-05-18 eager payload usability check removal

Build: `nix build -L . --builders ''` passed after removing the startup-time `gitGenerationPayloadObjectUsable()` probe from generation manifest replay. Functional tests: 218 passed, 8 skipped. Expr tests: 1907 passed, 3 skipped, 2 disabled.

Benchmark: `nix run .#eval-trace-bench -- generate` auto-selected run `933` with the default 10-commit no-debug/no-stats workflow.

Comparison window: same 10 commits as `cold/933`, compared against baseline run `906` and prior current run `932`.

Finding: no material improvement. `cold/933` is approximately 7.09s mean, matching `cold/932` (~7.10s) and still behind `cold/906` (~6.01s on this window). `hot/933` is approximately 1.21s mean, matching `hot/932` and still behind `hot/906` (~0.91s). This falsifies the hypothesis that eager payload-object existence/stat checks are a dominant fixed overhead in the benchmark window.

Next target: fixed startup overhead in generation manifest loading/replay, especially libgit commit/tree/blob walk, copying suffix blobs into intermediate strings, and rebuilding manifest-log records before in-memory index construction. Hot runs are the clearest signal because shutdown has no dirty rows and should return early.

### 2026-05-18 hot exact-hit bottleneck after run 934

Baseline remains run 906 at the pre-schema SQLite implementation (`92a3df1ab706cf514357ae57b367050b373ff146`): cold mean 6.01s, hot mean 0.91s on the selected 10-commit no-debug/no-stats window.

Current run 934 after manifest/load micro-optimizations: cold mean 7.04s, hot mean 1.20s. Soundness checks pass, but we are still behind baseline.

Targeted hot stats for commit `53443ee9cdffcc1fab3f4eafb0daac6a40c09fcd` show exact-session acceptance never fires: hits=7, misses=0, `verify.exactSessionProof.preflight.hits=0`, `verify.exactSessionProof.hits=0`, `verify.depsChecked=128145`, `verify.timeUs=557306`, `loadTrace.fullTimeUs=167775`, `resultDecode.timeUs=121063`. The hot path is therefore dominated by full trace load/decode and dependency verification, not database publication or manifest suffix replay.

Root-cause hypothesis: current-node lookup erases durable candidate metadata. `CandidateSnapshotRow` stores source identity, proof descriptor hash, coverage, precision, and descriptor availability, but `CandidateSnapshotRow::currentRef()` returns only `{traceId, resultId, nodeStamp}`. The serving fast path then attempts to re-prove exact-session eligibility from dependencies alone and rejects trace context, structured projections, and git/source coverage. The next implementation step is to carry exact-session proof descriptor metadata into the current-node fast path and use descriptor-level acceptance when it is sound, instead of falling back to full dep-domain verification.

### 2026-05-18 failed same-session Git shortcut

Tried replacing the unconditional Git/source exact-session miss with a narrow comparison between stored `GitRevisionIdentity` and `SessionConfig.sourceIdentity`, plus existing env/store rechecks. `nix build -L . --builders ''` compiled through `libexpr`, but the `binary-cache` functional test segfaulted in `nix-env -qas` (`217` passed, `1` failed, `8` skipped in functional tests before build failure).

Adversarial conclusion: session source identity is not replay authority by itself. It is a routing/root identity, not a proof-domain summary. Exact-session replay must persist and check enough proof metadata to show which observation domains were present and which obligations are covered. The shortcut was backed out; the path/trace/result candidate binding index remains because it is an authorization lookup replacement, not a semantic acceptance shortcut.

## 2026-05-18 candidate binding benchmark

Built current tree with `nix build -L . --builders ''`. Functional tests passed (`218 OK`, `0 Fail`, `8 skipped`) and expression tests passed (`1907 passed`, `3 skipped`). The unsafe same-session Git shortcut was backed out; the previous `binary-cache` segfault did not recur.

Generated default no-debug/no-stats 10-commit benchmark run `935` and compared against baseline run `906` from `92a3df1ab706cf514357ae57b367050b373ff146`.

Results:
- `cold/906` mean wall time: `6.01s`
- `hot/906` mean wall time: `0.91s`
- `cold/935` mean wall time: `7.13s`
- `hot/935` mean wall time: `1.20s`
- Soundness: all outputs matched reference.

Interpretation: the path/trace/result candidate binding index is correct enough to pass the full build/test suite, but does not materially close the benchmark gap. The remaining high-value target is authoritative compact exact-session replay: hot hits are still verifying/loading too much because the stored proof is not sufficient to prove replayability without full trace/proof work.

## Benchmark note: run 937 after reverting Git exact-proof check

- Build: `nix build -L . --builders ''` succeeded.
- Functional tests: 218 OK, 0 failed, 8 skipped.
- Expr tests: 1907 passed, 3 skipped.
- Benchmark command: `nix run .#eval-trace-bench -- generate`.
- Run number: `937`, default no-debug/no-stats, 10 commits.
- Comparison command: `nix run .#eval-trace-bench -- runs --reference cold/937 --runs cold/906,hot/906,cold/937,hot/937`.
- Baseline remains pre-schema SQLite run `906`: cold mean 6.01s, hot mean 0.91s.
- Current run `937`: cold mean 7.13s, hot mean 1.20s, all outputs match reference.
- Interpretation: reverting the Git exact-session proof experiment removes the run-936 hot regression (1.81s -> 1.20s), but we are still behind baseline on both cold and hot.
- Adversarial conclusion: current exact-proof logic still performs too much work before proving a hit. Git/source observations fail closed, and trace-context/structured projections still force full verification paths. The next optimization must make acceptance decidable from compact candidate metadata or descriptor-level summaries, not from loading full dependency vectors.

## 2026-05-21 regenerated baseline and fast-iteration notes

- Baseline commit: `92a3df1ab706cf514357ae57b367050b373ff146`.
- Regenerated no-debug/no-stats 100-commit baseline: `cold/938` and `hot/938`.
- Fast iteration uses the first 10 baseline commits unless a full confirmation run is needed.
- First 10 baseline means: `cold/938 = 6.06s`, `hot/938 = 0.88s`.
- Current post-restart reference before new experiments: `cold/941 = 6.56s`, `hot/941 = 1.18s`.
- Failed exact-probe/current-dep experiment: `cold/940 = 6.54s`, `hot/940 = 1.74s`; reverted because it moved work from full verification into slower per-current-dep reconstruction.
- Result sidecar gate improvement: `cold/942 = 6.52s`, `hot/942 = 1.05s`; kept. Debug run `hot-debug/943` shows result decode is no longer the hot path (`resultDecodeUs ~= 1ms`).
- Remaining hot cost is trace load plus full verification. One debug sample: `loadFullTraceUs ~= 173ms`, `verifyTraceCallUs ~= 249ms`, `loadTrace.fullDecodeUs ~= 29ms`, `loadTrace.fullTimeUs ~= 172ms`, `fullPayloadBytes ~= 9.7MB`.
- Next safe target is avoiding redundant semantic-hash passes while preserving trace/full/dep-set hash validation. Blindly trusting stored capsule headers would improve speed but is not acceptable unless the storage model explicitly treats cache objects as trusted writer output and documents the corruption semantics.

## 2026-05-21 combined trace-hash validation experiment

- Added `computeSortedTraceHashTupleFromSorted()` so trace-hash, dep-key-set-hash, and full-trace-hash validation is expressed as one invariant rather than three unrelated call sites.
- This is intentionally a safe experiment: it preserves recomputation of all semantic commitments and does not trust stored capsule headers as authority.
- Expected upside is limited because dep-key material is still fed into three domain-separated hash builders, but this gives a clean benchmark point before considering more aggressive descriptor/trust-boundary work.

## 2026-05-21 Git exact-session proof experiment rejected

- Tried allowing Git-governed exact-session hits by comparing the trace capsule's stored `GitRevisionIdentity` with the current repo identity.
- First version recomputed `computeGitIdentityHash()` at every exact-session acceptance boundary. It built and produced sound benchmark output, but run `945` regressed badly: `cold/945 = 6.51s`, `hot/945 = 2.13s` versus first-10 baseline `cold/938 = 6.06s`, `hot/938 = 0.88s`.
- Adversarial review found the expected cost shape: whole-repo Git status/content hashing per trace hit is `O(trace hits * repo scan)`. Reusing the session Git-identity cache avoids repeated scans but depends on a current-state snapshot/epoch contract; without an explicit immutable source boundary it is too easy to smuggle stale worktree state into exact-session replay.
- Existing tests also encode this semantic boundary: Git-governed exact-session proof must fail closed until there is a cheap, explicit proof summary. The cached variant failed those tests, so it was removed.
- Current build after removal: `nix build --builders '' -L .` passed.
- Current benchmark run: `946`, default no-debug/no-stats 10-commit workflow. All outputs match reference. `cold/946 = 6.49s`, `hot/946 = 1.05s`.
- Decision: do not continue Git exact-session proof without a redesigned source snapshot/epoch model. The next useful optimization must reduce full trace load/verification overhead or introduce compact proof descriptors with explicit soundness semantics.

## 2026-05-21 10-commit iteration notes after power-loss continuation

- Baseline for fast iteration remains the first 10 commits of no-debug/no-stats run `938`: `cold/938 ~= 6.06s`, `hot/938 ~= 0.88s` mean wall time.
- `947`: a source-identity negative preflight gate preserved output correctness but did not materially improve hot or cold timing. It was removed as dead-weight.
- Fused verifier attempt: replacing the existing verifier path with a single blocking `store_.verify(...)` scope failed `eval-trace-flake-inputs` with SIGSEGV during the Nix build test suite. It was reverted; the naive fusion is not safe without deeper lifetime/coroutine analysis.
- `948`: removing the no-op attrset materialization prefetch hint path preserved output correctness but did not materially improve performance (`cold` remained about `6.5s`, `hot` about `1.06s`). Keep only if it stays obviously semantic-neutral; it is not a baseline-beating lever.

## 2026-05-21 iteration notes

- Run 949 retained the earlier removal of eager, unused candidate session-proof descriptor summaries from the publish path. First-10 no-debug/no-stats means: cold/949 = 6.361330s, hot/949 = 1.051893s. This improved cold relative to 946/948 but still missed baseline cold/938 = 6.058941s and hot/938 = 0.880468s.
- Run 950 tested a persisted exact-session preflight-eligibility bit in the Git manifest to skip known-ineligible preflight probes. First-10 no-debug/no-stats means: cold/950 = 6.368122s, hot/950 = 1.050777s. The hot difference was noise-level and cold regressed slightly versus 949, so the eligibility-bit manifest change was removed rather than carried forward.
- Current near-term direction: avoid carrying small heuristic gates. The next useful lever must remove an entire hot-path phase, most likely by using already-persisted exact descriptors to accept safe exact-session hits before loading/verifying full traces, or by fusing trace decode/hash/shape work without changing proof authority.
- Run 951 rebuilt after removing the persisted preflight-eligibility experiment. First-10 no-debug/no-stats means: cold/951 = 6.371963s, hot/951 = 1.048531s. This confirms the experiment was not useful; current retained changes are effectively the eager-proof-summary removal and dead prefetch-hint cleanup.
- Review-agent finding: do not use `PersistentExactDescriptors` as proof authority yet. The current persisted descriptor material is summary/export-oriented and not sufficient to re-observe typed current-world facts. Safer next candidate is complete-probe-backed recursive `TraceContext` exact-session preflight.

## 2026-05-21 quick gate: recursive TraceContext exact-session preflight

- Run `952` tested recursive complete-probe-backed TraceContext exact-session preflight after a successful local `nix build --builders '' -L .`.
- First-10 no-debug/no-stats means: baseline `938` cold `6.058941s`, hot `0.880468s`; retained state `951` cold `6.371963s`, hot `1.048531s`; experiment `952` cold `6.355293s`, hot `1.054839s`.
- Decision: removed the recursive preflight experiment. It adds nontrivial graph/session semantics risks and does not materially improve the benchmark; hot regresses.
- Adversarial review concerns to preserve for future designs: validation-only recursion is required if this is revisited, verification state must be graph/epoch aware rather than trace-id-only, cycle/depth guards need node identity, and TraceContext memo invalidation must account for trace-hash patching.

## 2026-05-21 quick gate: async verifier batching

- Run `953` tested batching current-node lookup with exact-session preflight plus batching already-verified/full-verified result decode.
- First-10 no-debug/no-stats means: baseline `938` cold `6.058941s`, hot `0.880468s`; retained state `951` cold `6.371963s`, hot `1.048531s`; batching `953` cold `6.369268s`, hot `1.048724s`.
- Decision: non-impactful under the 10-commit gate. Do not count this as progress toward beating baseline; prefer lower-level cold/writeback changes next.
- Subagent agreement: lookup+preflight batching is semantically low risk and mirrors sync path shape, but any future version should avoid misleading telemetry by not charging preflight work to `nrVerifyCurrentLookupUs`.

## 2026-05-21 cold-path candidate list from writeback review

- Current cold persistence path is `writeBackGitLocked()`, not the old SQLite writeback path.
- Strong low-risk candidates: skip capped partial probe generation for large traces; avoid full existing-object reads during payload publish behind non-paranoid mode; make snapshot interval adaptive/larger; replace capsule-pair string keys with fixed typed keys.
- Medium-risk candidate: fuse full capsule and complete-probe encoding; requires roundtrip tests because probe canonical bytes must stay exact.

## 2026-05-21 quick gate: suppress capped partial probes for large traces

- Run `954` tested complete-only probe emission: traces with `deps.size() <= kTraceCapsuleProbeMaxDeps` still get complete probes; larger traces emit an empty partial probe frame with the real `totalDeps`.
- First-10 no-debug/no-stats means: baseline `938` cold `6.058941s`, hot `0.880468s`; retained state `951` cold `6.371963s`, hot `1.048531s`; experiment `954` cold `6.352491s`, hot `1.045374s`.
- Decision: provisionally keep. It is sound/fail-closed and gives a small positive wall-time move, but it is not enough to matter alone.
- Adversarial note: this removes partial-probe fast filtering for large traces and makes large-trace exact preflight impossible from probes. That is a performance tradeoff, not a soundness loss; full verification/recovery remains authoritative.
- Follow-up hardening candidate: cap decoded `totalDeps` in probe frames so malformed empty probes cannot advertise unbounded counts to future telemetry/logic.

### 2026-05-21: existing payload object size-skip rejected

- Experiment run: `955` after adding a writeback fast path that skipped re-reading/re-hashing existing same-size payload objects.
- Fixed 10-commit means: `cold/955 = 6.421746s`, `hot/955 = 1.048302s`.
- Comparator: baseline `cold/938 = 6.058941s`, `hot/938 = 0.880468s`; retained current `cold/954 = 6.352491s`, `hot/954 = 1.045374s`.
- Decision: reverted. It added same-size-corruption self-healing caveats and did not improve the measured path.

### 2026-05-21: Git manifest snapshot interval 32 rejected

- Experiment run: `956` with `kGitSnapshotInterval = 32`.
- Fixed 10-commit means: `cold/956 = 6.373969s`, `hot/956 = 1.045281s`.
- Comparator: baseline `cold/938 = 6.058941s`, `hot/938 = 0.880468s`; retained current `cold/954 = 6.352491s`, `hot/954 = 1.045374s`.
- Decision: rejected. Reducing full-snapshot frequency did not improve hot and made cold slightly worse on the 10-commit gate.

### 2026-05-21: Git manifest snapshot interval 4 rejected

- Experiment run: `957` with `kGitSnapshotInterval = 4`.
- Fixed 10-commit means: `cold/957 = 6.354270s`, `hot/957 = 1.048245s`.
- Comparator: baseline `cold/938 = 6.058941s`, `hot/938 = 0.880468s`; retained current `cold/954 = 6.352491s`, `hot/954 = 1.045374s`.
- Decision: rejected. More frequent snapshots did not improve hot and did not materially change cold. The fixed snapshot interval is not the current dominant regression.

### 2026-05-21: fused probe encoding rejected on benchmark

- Experiment run: `958` reused already encoded full dependency rows to build complete probe sidecars, avoiding a second dep-key interning/encoding pass.
- Fixed 10-commit means: `cold/958 = 6.359005s`, `hot/958 = 1.048555s`.
- Comparator: baseline `cold/938 = 6.058941s`, `hot/938 = 0.880468s`; retained current `cold/954 = 6.352491s`, `hot/954 = 1.045374s`.
- Adversarial review found no blocking semantic issue, but the benchmark did not move in the right direction. This is not the dominant cold regression.

### 2026-05-21: manifest-inlined probe/result sidecars rejected on benchmark

- Experiment run: `959` with Git manifest version `4`, adding framed probe/result sidecars to each trace row and seeding `RawTraceCapsuleSnapshot` from startup-loaded manifest bytes.
- Fixed 10-commit means: `cold/959 = 6.368537s`, `hot/959 = 1.044238s`.
- Comparator: baseline `cold/938 = 6.058941s`, `hot/938 = 0.880468s`; retained current `cold/954 = 6.352491s`, `hot/954 = 1.045374s`.
- Finding: sidecars are tiny (`~3.5 KiB` probe bytes and `~8.3 KiB` result bytes across 23 capsules), but avoiding capsule-prefix reads moved hot by only ~1 ms and worsened cold slightly. The hot gap is not dominated by external capsule prefix I/O.

### 2026-05-21 run 961: exact-session GitIdentity coverage experiment rejected

Change tried: allow same-session exact proof to cover repo-governed `FileBytes` / `StructuredProjection` when the stored `GitRevisionIdentity` dep equals the current repo Git identity. This was semantically sound only because `computeGitIdentityHash` covers HEAD plus dirty/deleted/untracked/ignored entries, and the path failed closed on resolution errors.

10-commit no-debug benchmark:
- baseline `cold/938`: 6.058941s; `hot/938`: 0.880468s
- prior current `cold/960`: 6.361825s; `hot/960`: 1.039587s
- experiment `cold/961`: 6.353234s; `hot/961`: 1.613548s

Finding: whole-repo Git identity recomputation is too expensive for hot hits. It avoids full trace decode/replay in principle, but its current implementation scans/hashes enough repo state that the hot path regresses by ~574ms versus current and ~733ms versus baseline. Reverted. Future Git-based exact proof needs a cheap already-known current identity, an incremental repo snapshot, or a narrower per-observation proof; recomputing the whole repo identity inside hot verification is not viable.

### 2026-05-21 run 962: session-source GitIdentity proof rejected

Experiment: accept repo-governed file/projection deps when the stored GitIdentity dep matched the already-computed file-eval `SessionConfig.sourceIdentity` and the session had exactly one matching external root.

10-commit no-debug benchmark:
- baseline `cold/938`: 6.058941s; `hot/938`: 0.880468s
- retained current `cold/960`: 6.361825s; `hot/960`: 1.039587s
- experiment `cold/962`: 6.445723s; `hot/962`: 1.050191s

Finding: this did not unlock the hot preflight bypass in the benchmark path and worsened cold. Reverted. The next candidate should be selected from measured hot/cold phase costs rather than expanding exact-proof authority speculatively.

### 2026-05-21 run 963: single-block verifier path rejected

Experiment: route `Verifier::verifyAttr` through the existing synchronous `SqliteTraceStorage::verify` path inside one blocking critical section, aiming to reduce scheduler/coroutine fragmentation on hot hits while preserving the same proof rules.

Result on the fixed first 10 no-debug commits:

- `cold/938 = 6.058941s`, `hot/938 = 0.880468s` baseline.
- `cold/960 = 6.361825s`, `hot/960 = 1.039587s` retained current comparator.
- `cold/963 = 11.915328s`, `hot/963 = 1.046114s` experiment.

Finding: the single-block path materially worsened cold and did not improve hot. Reverted. The likely failure mode is that the synchronous storage verifier bypasses verifier-local async structure/prefetch behavior or serializes work that the current verifier overlaps. Future hot-path work should target the measured full-trace load/hash/verification cost without collapsing the verifier orchestration into the old synchronous path.

### 2026-05-21 run 964: path-backed exact-session proof rejected

Experiment: allow exact-session proof to accept GitRevisionIdentity when it matches the session source identity, while directly rechecking path-backed observations and using StructuredProjection source-content hashes as a deterministic projection proof.

10-commit no-debug/no-stats gate:

- baseline `cold/938`: 6.058941s
- baseline `hot/938`: 0.880468s
- retained comparator `cold/960`: 6.361825s
- retained comparator `hot/960`: 1.039587s
- experiment `cold/964`: 6.365064s
- experiment `hot/964`: 1.049557s

Result: rejected and reverted. It preserved the intended current-world proof shape for path-backed deps, but did not reduce wall time; the added proof checks were pure overhead for this workload.

### 2026-05-21 run 965: complete-probe verified dependency projection rejected

Experiment: use complete trace probes as dependency projections on hot verification when the probe contained every dependency in canonical order and re-hashed to the stored trace tuple. Result sidecars remained independently checked. A DirSet guard was added after tests exposed that full capsule decode also reconstructs `pools.dirSets`, which the probe projection did not carry.

10-commit no-debug/no-stats gate:

- baseline `cold/938`: 6.058941s
- baseline `hot/938`: 0.880468s
- retained comparator `cold/960`: 6.361825s
- retained comparator `hot/960`: 1.039587s
- experiment `cold/965`: 6.357350s
- experiment `hot/965`: 1.047805s

Result: rejected and reverted. The projection proof shape was defensible after the DirSet fail-closed guard, but it did not improve the measured path. Cold was neutral and hot regressed slightly versus the retained comparator, so full-capsule dependency inflation is not removable this way without also solving the missing auxiliary decoded state and verifier replay costs.

### 2026-05-21 run 966: source-identity exact-session proof rejected

Compared the exact-session proof experiment against the fixed first-10 no-debug/no-stats baseline and retained-current comparator:

- `cold/938`: 6.058941s
- `hot/938`: 0.880468s
- `cold/960`: 6.361825s
- `hot/960`: 1.039587s
- `cold/966`: 6.352391s
- `hot/966`: 1.047345s

Finding: the source-identity proof slightly improved cold versus retained-current but remained behind the pre-schema baseline and regressed hot. Reverted it. The file-eval hot path is still dominated by full verification/load behavior rather than exact-session proof eligibility alone.

### 2026-05-21 run 967: fused verifier orchestrator rejected

Experiment: route `Verifier::verifyAttrImpl` through `SqliteTraceStorage::verify` in one `coroBlock` / exclusive storage section, preserving the same storage-layer verifier semantics while reducing coroutine and lock-boundary crossings.

Compared against fixed first-10 no-debug/no-stats baseline and retained-current comparator:

- `cold/938`: 6.058941s
- `hot/938`: 0.880468s
- `cold/960`: 6.361825s
- `hot/960`: 1.039587s
- `cold/967`: 11.935938s
- `hot/967`: 1.046294s

Finding: rejected and reverted. The fused path likely serialized work that the existing coroutine split was allowing to overlap; cold regressed severely and hot did not improve. Keep the existing async-stage split unless a later design provides finer-grained storage access without broad exclusive serialization.

Follow-up from review agent: the more promising targets are reducing full capsule semantic hash validation cost during `loadFullTrace` and avoiding eager structural-index materialization in the verifier pass logic.

### Rejected experiment: structural-pass lazy materialization / compact file coverage sets

- Run: 968 (10-commit fast gate, no debug/no stats).
- Result: cold mean 6.364024s, hot mean 1.050508s.
- Comparators: baseline run 938 first-10 cold 6.058941s, hot 0.880468s; retained-current run 960 first-10 cold 6.361825s, hot 1.039587s.
- Verdict: rejected and reverted. The change reduced retained structural-index storage but did not improve wall time; hot regressed, and cold was essentially unchanged/worse. This supports the earlier profiling conclusion that the larger hot cost is full trace semantic validation/decode, not the structural index vectors.

### 2026-05-21 run 969: capsule semantic-hash fast path rejected

Experiment: decode a capsule-local canonical hash tuple from encoded dependency rows and skip the legacy post-decode semantic hash recomputation when the tuple matched the capsule header. Tightened during review by staging DirSet definitions until capsule acceptance, validating DirSet digests, rejecting trace-context array paths, avoiding result-only guard-set allocation, and fixing the stored-probe frame size cap.

10-commit no-debug/no-stats gate:

- baseline `cold/938`: 6.058941s
- baseline `hot/938`: 0.880468s
- retained comparator `cold/960`: 6.361825s
- retained comparator `hot/960`: 1.039587s
- experiment `cold/969`: 6.351870s
- experiment `hot/969`: 1.058726s

Verdict: rejected. The fast hash path did not beat the baseline and regressed hot versus retained-current. Removed the fast-path duplicate encoded-dependency accumulation and skip condition. Kept the correctness hardening as a separate candidate to measure next, because the review identified real DirSet poisoning/conflict and trace-context array-path risks independent of the performance experiment.

### 2026-05-21 run 970: capsule correctness-only hardening measured

After removing the rejected semantic-hash skip from run 969, I retained only the capsule correctness hardening: staged DirSet definitions until semantic acceptance, DirSet digest validation, trace-context path rejection for array-index components, source-content guard decode simplification, and the stored-probe frame cap fix.

10-commit no-debug/no-stats gate:
- baseline `cold/938`: 6.058941s
- baseline `hot/938`: 0.880468s
- retained-current comparator `cold/960`: 6.361825s
- retained-current comparator `hot/960`: 1.039587s
- candidate `cold/970`: 6.371104s
- candidate `hot/970`: 1.054011s

Result: rejected as a performance improvement. This is correctness hardening only; it increases hot cost and should not be presented as progress toward beating the baseline unless we intentionally accept the soundness fix and find compensating wins elsewhere.

### 2026-05-21 run 971: typed `(full_hash,result_hash)` capsule keys

Experiment: replace heap-allocated 64-byte string keys for raw capsule pair bookkeeping with a fixed `TraceCapsulePairKey { FullTraceHash, ResultHash }` plus a custom hasher. Semantics are unchanged: the durable capsule identity remains the exact `(full_hash, result_hash)` pair, preserving same-trace/different-result observations. This follows the same content-addressed object identity model documented for the prior string key, but avoids allocation/copying in manifest replay, result decode lookup, and writeback staging.

10-commit no-debug/no-stats gate:
- baseline `cold/938`: 6.058941s
- baseline `hot/938`: 0.880468s
- retained-current comparator `cold/960`: 6.361825s
- retained-current comparator `hot/960`: 1.039587s
- correctness-only candidate `cold/970`: 6.371104s
- correctness-only candidate `hot/970`: 1.054011s
- typed-key candidate `cold/971`: 6.355682s
- typed-key candidate `hot/971`: 1.046411s

Result: small positive movement versus run 970, especially cold, but still nowhere near baseline and hot remains worse than retained-current run 960. This can be kept only as a low-risk cleanup/offset, not as a material performance win.

- [x] Run 972: dirty row counters for no-dirty shutdown fast path. Built successfully; fixed-first-10 means were cold 6.374611s, hot 1.052369s. This regressed versus typed capsule pair keys in run 971 (cold 6.355682s, hot 1.046411s) and remains far behind baseline run 938 (cold 6.058941s, hot 0.880468s). Rejected and removed; the added mutable counters are not justified by measured performance.

## 2026-05-21 exact-session Git coverage experiment rejected

- Run: `973` (10 commits, default no debug/stats) after allowing exact-session proof to cover GitRevisionIdentity plus Git-governed file/structured deps by recomputing current Git identity.
- Result: `cold/973 = 6.377162s`, `hot/973 = 1.612921s` against fixed first-10 baseline `cold/938 = 6.058941s`, `hot/938 = 0.880468s` and current comparator `hot/971 = 1.046411s`.
- Debug run: `hot-debug/946` from `cold/973` showed `exactSessionProof.bypasses = 1`, `exactSessionPreflightCheckUs ~= 571ms`, `loadFullTraceUs ~= 160ms`, and `verifyTraceCallUs ~= 246ms` on the representative first commit. The current Git identity scan cost dominated and only avoided one full verification.
- Decision: reject and remove this approach. Exact-session proof now fails closed for Git/source observations again. Any future Git-backed hot path needs a different design that avoids a large current-repo scan on every hot process, likely by carrying a cheap, trustworthy current source identity from evaluation/source resolution rather than recomputing it inside candidate verification.

### Run 975: capsule validation fusion / encoded-row semantic hash

Built successfully with `nix build --builders '' -L .` and ran a 10-commit no-debug/no-stats benchmark.

First-10 means:

- `cold/938`: 6.058941s
- `hot/938`: 0.880468s
- `cold/974`: 6.349766s
- `hot/974`: 1.046962s
- `cold/975`: 6.324401s
- `hot/975`: 1.036808s

Finding: preserving semantic verification while computing the semantic commitments directly from encoded capsule rows is a small improvement over 974, but not remotely enough to beat baseline. Keep only if the complexity is justified by correctness/format clarity; it is not the main path to the needed performance win.

### Run 976: monolithic verifier wrapper experiment

Patch: routed `Verifier::verifyAttrImpl` through the storage-owned synchronous `SqliteTraceStorage::verify(...)` under one `coroBlock` / one exclusive access.

First-10 means:

- `cold/975`: 6.324401s
- `hot/975`: 1.036808s
- `cold/976`: 11.893816s
- `hot/976`: 1.023361s

Finding: rejected. Hot improved only marginally, while cold regressed catastrophically. The current split verifier path is doing something materially beneficial for cold/recovery scheduling or preserving the benchmark's partial-hit shape. Removed the early-return experiment.

## 2026-05-21 run 977: capsule v6 trusted header commitments

- Change under test: capsule format v6 trusts writer-derived semantic hash commitments after verifying the raw capsule payload hash and manifest/header identity, instead of replaying semantic hash builders over every decoded dependency on hot read.
- 10-commit no-debug/no-stats benchmark means:
  - baseline 92a3df1 run 938: cold 6.058941s, hot 0.880468s
  - previous retained v5 run 975: cold 6.324401s, hot 1.036808s
  - v6 run 977: cold 6.177095s, hot 0.924797s
- Finding: material improvement over v5, especially hot hits, but still behind the pre-schema SQLite baseline. Keep as the current candidate, then continue reducing read/write overhead.
- Adversarial note: v6 makes writer/canonicalization part of the TCB for semantic commitments. Reader still verifies raw payload hash and manifest/header binding before trusting those commitments; stale or mismatched manifest/header data must fail closed.

## 2026-05-21 run 978: permit result sidecar after deps-payload verification

- Change under test: allow result sidecar replay when either full payload or dependency payload verification has already checked the stored capsule object hash.
- 10-commit no-debug/no-stats benchmark means:
  - baseline run 938: cold 6.058941s, hot 0.880468s
  - v6 run 977: cold 6.177095s, hot 0.924797s
  - sidecar eligibility run 978: cold 6.186104s, hot 0.918348s
- Finding: tiny hot improvement, cold noise/regression. Keep temporarily because it is a low-complexity semantics cleanup, but it is not enough to change the direction.

## 2026-05-21 run 979: exact-session preflight fast-reject (reverted)
- Change under test: rejected exact-session preflight probe rows before tuple hashing when their dependency key shape could not be covered by exact-session proof rules.
- 10-commit means: `cold/979 = 6.206784s`, `hot/979 = 0.922165s`.
- Baseline first-10 means remain `cold/938 = 6.058941s`, `hot/938 = 0.880468s`; previous retained candidate was `cold/978 = 6.186104s`, `hot/978 = 0.918348s`.
- Finding: this local fast-reject did not help; it slightly regressed both cold and hot versus run 978, likely because the rejected shapes are not frequent enough in the benchmark hot path to pay for another branch/helper boundary.
- Decision: reverted the experiment. A metadata-level skip before probe decode may still be worth evaluating, but this row-local check is not.

## 2026-05-21 run 980: session memo before exact-session preflight (reverted)
- Change under test: checked `session.verifiedTraceIds` before exact-session preflight for current-node hits, to skip complete-probe decode/re-hash after a TraceId was already proven in the same verifier session.
- 10-commit means: `cold/980 = 6.193877s`, `hot/980 = 0.926231s`.
- Baseline first-10 means remain `cold/938 = 6.058941s`, `hot/938 = 0.880468s`; retained candidate run 978 was `cold/978 = 6.186104s`, `hot/978 = 0.918348s`.
- Finding: no evidence that repeated TraceId validation reuse is hot enough for this workload; the branch likely adds cost on the dominant path.
- Decision: reverted. If this idea returns, it needs instrumentation showing high repeated TraceId hits before changing the main path.

## 2026-05-21 current-node dense side index (reverted before benchmark)
- Change under test: vector side index for `currentNodeIndex` keyed by local `AttrPathId`.
- Build result: failed `TraceStoreTest.Verify_TraceResultMismatch_FailsClosed`.
- Root cause: the accelerator could become stale when current-node bindings were mutated outside the publish/session-bulk-load path; lookup trusted the vector before the map and bypassed an intentionally corrupted trace/result binding.
- Decision: reverted. If revisited, the side index must be the only mutable representation or must be rebuilt after every mutation/corruption hook; otherwise it can defeat fail-closed mismatch detection.

## 2026-05-21 no-op prefetch token path experiment
- Change under test: removed verifier hot-path lookup/removal of speculative prefetch tokens and made `Verifier::submitPrefetchHints` an intentional no-op.
- Rationale: the pool stored tokens but had no producer that performed or completed speculative verification, so each verification paid bookkeeping that could not serve a result.
- Correctness boundary: no cache acceptance logic changed; unresolved hints previously fell through to normal verification after token removal, and now fall through directly.
- Benchmark: run a local build and 10-commit no-debug `eval-trace-bench` after this change.
- Result: kept. Run 982 fixed-first-10 no-debug means: cold `6.194818s`, hot `0.924604s`.
- Comparison: slight improvement over post-revert run 981 (`6.197935s` cold, `0.927485s` hot), still behind baseline run 938 (`6.058941s` cold, `0.880468s` hot).
- Follow-up: this was not material enough by itself; continue with larger cold/hot-path reductions.

## 2026-05-21 fused verifier blocking-scope experiment
- Change under test: `Verifier::verifyAttrImpl` now calls `SqliteTraceStorage::verify()` inside one `coroBlock`/exclusive-access scope instead of splitting current lookup, preflight, full load, verification, decode, and recovery across separate coroutine hops.
- Rationale: the store-level verifier already encodes the same proof sequence; repeated blocking-pool and exclusive-access handoffs add overhead but do not add soundness or precision.
- Correctness boundary: cache acceptance still flows through the existing store-level exact-session, full verification, and recovery functions.
- Benchmark: build locally, then run a 10-commit no-debug `eval-trace-bench` and compare against run 982 and baseline 938.
- Result: rejected and reverted. Run 983 fixed-first-10 no-debug means: cold `11.688164s`, hot `0.917769s`.
- Interpretation: the fused path slightly improved hot but destroyed cold by changing the history/current-node cache behavior profile; this is not an acceptable tradeoff.
- Decision: revert. A future version would need to preserve the async path's exact cold bootstrapping semantics before reattempting handoff fusion.

## 2026-05-21 source-content guard side-table emission experiment
- Change under test: new capsule writes no source-content guard side-table rows and no longer builds/sorts the guard map during `encodeTraceCapsule`.
- Rationale: new dep rows already carry `sourceContentHash` directly; the side table duplicates row-local authority and adds writeback work and bytes.
- Correctness boundary: newly written capsules preserve the row-local source-content hash. The existing decoder path can still read older capsules that used guard rows as backfill.
- Benchmark: build locally, then run a 10-commit no-debug `eval-trace-bench`.
- Result: kept. Run 985 fixed-first-10 no-debug means: cold `6.170340s`, hot `0.927947s`.
- Comparison: improvement over run 984 (`6.191204s` cold, `0.929512s` hot), still behind baseline run 938 (`6.058941s` cold, `0.880468s` hot).
- Review: subagent confirmed the side table is redundant for newly written capsules because row-local `sourceContentHash` remains authoritative; row-local hashes must not be removed.

## 2026-05-21 reverse single-pass session bulk-load experiment
- Change under test: rebuild exact-session current heads and recovery buckets in one reverse pass over append-ordered `allCandidateRows`.
- Rationale: `durableOrder` follows append order, so reverse replay yields newest-first materialized projections without a current-node durable-order map or per-bucket stable sorts.
- Correctness boundary: the durable candidate log remains authority; `currentNodeIndex` and recovery buckets are derived indexes. First matching exact row per path wins in reverse durable order; recovery buckets are pushed newest-first.
- Benchmark: build locally, then run a 10-commit no-debug `eval-trace-bench`.

2026-05-21 - Reverse session replay adversarial fix
- The attempted reverse-vector replay failed `TraceStoreTest.Session_CurrentNodeIndex_RebuildUsesMaxDurableOrder`: physical candidate row order is not semantic authority.
- Reworked `bulkLoadSessionRowsLocked()` to do one scan keyed by explicit `durableOrder`, choosing max durable order for exact-session heads and sorting recovery buckets newest-first after population.
- This preserves the append-only log/materialized-view model while remaining robust to conflict reloads or test fixtures that reorder the vector.

2026-05-21 - Rejected durable-order rebuild optimization
- Build/test passed after correcting semantics, but 10-commit run `986` regressed versus retained run `985` (`cold 6.199217s` vs `6.170340s`, `hot 0.931447s` vs `0.927947s`).
- Reverted the attempted reverse/single-scan session rebuild optimization. It did not move the benchmark in the right direction and is below the complexity bar.

2026-05-21 - Verifier scheduling experiment: fuse verified-trace memo with full trace load
- Removed one blocking-pool/exclusive-store hop in `Verifier::verifyAttrImpl()` by checking `session_.verifiedTraceIds` inside the same exclusive block that would otherwise call `loadFullTrace()`.
- This deliberately avoids an unsafe lock-free session read. The session memo remains accessed under the same synchronization convention as the surrounding verifier/store paths.
- Next benchmark target: this should help hot exact hits if the extra hop was measurable; reject if the first-10 benchmark does not improve against run `985`.

2026-05-21 - Rejected verifier memo/load fusion
- 10-commit run `987` after fusing the verified-trace memo check with `loadFullTrace()` was not material: cold regressed (`6.190573s` vs run `985` `6.170340s`), hot improved only marginally (`0.925407s` vs `0.927947s`).
- Reverted the experiment. A ~2.5ms hot-only change with cold regression does not help beat baseline run `938`.

2026-05-21 - Volatile dep metadata experiment
- Added `LoadedFullTrace` metadata derived from decoded deps and cached on `TraceCacheEntry`.
- `Verifier::verifyAttrImpl()` now gets `hasVolatileDep` from the same load path instead of rescanning the immutable dep vector on that path.
- Soundness boundary: metadata is computed from decoded deps already authenticated by capsule hashes; it is not serialized authority.

2026-05-21 - Rejected volatile dep metadata experiment
- 10-commit run `988` regressed cold (`6.227534s` vs run `985` `6.170340s`) and only marginally improved hot (`0.924220s` vs `0.927947s`).
- Reverted it. The extra API/metadata field is not justified by the measured result and still leaves us far behind baseline hot (`0.880468s`).

2026-05-21 - Complete-probe coverage experiment
- Raised `kTraceCapsuleProbeMaxDeps` from 4096 to 16384 to test whether exact-session hot hits are falling back to full capsule load because probes are incomplete.
- Expected tradeoff: larger uncompressed probe sidecars may cost cold/writeback and cache size, but can let more hot hits bypass full trace decode/verification.
- Decision rule: keep only if first-10 hot improvement is material without unacceptable cold regression versus retained run `985` and baseline run `938`.

2026-05-21 - Rejected complete-probe cap increase
- 10-commit run `989` with probe cap 16384 improved hot only modestly (`0.922828s` vs run `985` `0.927947s`) but regressed cold (`6.212324s` vs `6.170340s`).
- Reverted the cap to 4096. Probe incompleteness contributes some hot cost, but increasing the sidecar size does not beat baseline and hurts cold.

2026-05-21 - Probe-row exact preflight experiment
- Reworked exact-session preflight to validate/hash/prove directly over decoded `TraceCapsuleProbeDep` rows instead of copying them into a temporary `std::vector<Dep>`.
- Soundness boundary is unchanged: probe rows still have to be complete, ordered, re-hash to the stored trace tuple, and pass the exact-session proof classifier before result replay.
- Expected impact is limited but directionally useful: remove allocation/copy from exact-hit preflight without changing accepted observations.

2026-05-21 - Rejected probe-row preflight
- 10-commit run `990` was not material: hot improved only to `0.925845s` from run `985` `0.927947s`, while cold regressed to `6.179136s` from `6.170340s`.
- Reverted it. This confirms temporary preflight vector allocation is not the dominant baseline gap.

### Hot-manifest sidecar experiment (run 991)
- Implemented an advisory sidecar keyed by current Git ref OID to skip the libgit2 manifest walk on hot startup.
- 10-commit no-debug run 991: cold mean 6.830344s, hot mean 0.922200s.
- Rejected and reverted: cold regressed badly vs baseline 938 (6.058941s) and recent run 985 (6.170340s); hot remained well behind baseline 938 (0.880468s) and only slightly better than 985 (0.927947s). Startup manifest lookup is not the dominant hot gap, or the sidecar overhead is in the wrong place.

### Zero-dep-only exact-session preflight experiment (run 992)
- Restricted exact-session preflight to zero-dependency traces. Non-empty traces now fall back to full verification instead of using the non-authoritative complete-probe fast path.
- 10-commit no-debug run 992: cold mean 6.172909s, hot mean 0.925441s.
- Compared with recent run 985: cold unchanged within noise (6.170340s -> 6.172909s), hot improved only about 2.5ms mean (0.927947s -> 0.925441s).
- Compared with fixed pre-schema baseline 938: still behind on cold (6.058941s baseline) and hot (0.880468s baseline).
- Performance conclusion: not a material optimization. Semantic conclusion: this is a safer narrowing because non-empty exact replay should be authorized by an explicit persisted proof summary, not by rebuilding partial probe state during startup/eval.

### Trace-level exact replay class experiment (run 993, authority rejected)
- Added a trace-row exact replay class and used it as a preflight authority for non-empty session-fingerprint-only traces.
- 10-commit no-debug run 993: cold mean 6.180425s, hot mean 0.920445s.
- Compared with run 992: hot improved about 5.0ms mean, cold regressed about 7.5ms mean. Compared with baseline 938: still behind on cold (6.058941s) and hot (0.880468s).
- Adversarial finding: the class was stored in manifest trace-row metadata, which is an index/projection, not hash-bound proof material. Using it to bypass full verification would make mutable index bytes into replay authority. Rejected that authority boundary and removed the fast path/manifest byte.

### Current-source exact proof experiment
- New direction: keep exact proof decisions after `loadFullTrace`, where dependency bytes are capsule-authenticated, but let file/Git traces bypass full verification when the recorded GitRevisionIdentity dep matches the current exact `SessionConfig::sourceIdentity` and every dep is covered by that repo/source proof.
- This reuses the session opener's current Git identity instead of rescanning libgit2 during verification. It should preserve soundness because candidate rows remain routing only; the proof is the verified dependency object plus exact session key/source identity.
- Decision rule: benchmark with 10 commits after build. Keep only if it materially improves hot/cold without weakening rejection for env/store/trace-context/volatile/implicit/copied-source deps.

### Current-source exact proof benchmark (run 994, rejected)

After fixing the implicit-guard bypass bug in `allKeysGitRecoverableImpl`, the current-source exact proof built cleanly and passed `nix build -L . --builders ''` (`nix-expr-tests`: 1907 passed, 3 skipped).

10-commit no-debug benchmark:

- `cold/994`: mean 6.203263s, median 5.796812s, min 0.917347s, max 14.041663s.
- `hot/994`: mean 0.931811s, median 0.934404s, min 0.913732s, max 0.951271s.

Comparison:

- Baseline `938`: cold mean 6.058941s, hot mean 0.880468s.
- Previous accepted/recent `992`: cold mean 6.172909s, hot mean 0.925441s.

Conclusion: reject and revert the current-source exact proof. The soundness boundary can be made correct, but the proof still requires full trace load and adds work on this workload. It does not move us toward beating the pre-schema baseline.

### Current-node fast-path bundling benchmark (run 995)

Change: bundled current-node lookup, exact zero-dep preflight, complete-probe shadow, and already-verified replay into one blocking/mutex section in `Verifier::verifyAttrImpl`. This does not change authority: it only moves existing exact current-node acceptance/replay checks together.

Build: `nix build -L . --builders ''` passed (`nix-expr-tests`: 1907 passed, 3 skipped; functional tests: 218 passed, 8 skipped).

10-commit no-debug benchmark:

- `cold/995`: mean 6.166997s, median 5.794910s, min 0.907399s, max 13.991700s.
- `hot/995`: mean 0.927958s, median 0.926321s, min 0.914994s, max 0.941316s.

Comparison:

- Baseline `938`: cold mean 6.058941s, hot mean 0.880468s.
- Accepted/recent `992`: cold mean 6.172909s, hot mean 0.925441s.
- Rejected source-proof `994`: cold mean 6.203263s, hot mean 0.931811s.

Conclusion: not materially helpful. Scheduler/lock hop removal is not the dominant hot gap on this workload. The remaining gap likely comes from startup/object index load and result replay/materialization rather than per-attr `coroBlock` overhead alone.

## 2026-05-21 fast iteration: complete-probe exact preflight rejected

- Tested non-zero complete-probe exact-session preflight by raising `kTraceCapsuleProbeMaxDeps` to `32768` and allowing complete probes to rehash to the stored trace tuple before exact-session proof acceptance.
- Build passed after updating the policy tests, but default no-debug/no-stats 10-commit benchmark run `1002` regressed: `cold/1002 = 6.273212s`, `hot/1002 = 1.080547s` versus first-10 baseline `cold/938 = 6.058941s`, `hot/938 = 0.880468s`.
- Reverted the experiment and restored the conservative zero-dependency-only exact-session preflight. Rebuild passed on retry; the first build hit a non-reproducible `fetchTree-file` SIGSEGV that passed immediately on rerun.
- Post-revert benchmark run `1003`: `cold/1003 = 6.203508s`, `hot/1003 = 0.925307s`. This recovers the hot regression but remains behind baseline.
- Conclusion: complete probes are still not the right hot-path authority shape. Even when rehashed to the stored trace tuple, the probe path adds enough copy/hash/proof work to lose. The next candidate should reduce full verifier work or recovery fanout, not make probe projection more elaborate.

## 2026-05-21 fast iteration: verifier batching and probe-budget experiments rejected

- Run `1004` tested collapsing async verifier work into one synchronous `SqliteTraceStorage::verify()` call under a single blocking-store access. Build and tests passed, but the benchmark regressed cold badly: `cold/1004 = 11.788981s`, `hot/1004 = 0.922960s` versus post-revert `cold/1003 = 6.203508s`, `hot/1003 = 0.925307s` and baseline `cold/938 = 6.058941s`, `hot/938 = 0.880468s`.
- Interpretation: whole-verification batching serializes cold per-attr work under one storage access and destroys useful parallelism. It does not improve hot. Reverted.
- Run `1005` tested budgeted probe emission (`128 KiB` max encoded probe, over-budget traces emit incomplete probes). Build/tests passed, including an over-budget serialization regression, but benchmark did not improve: `cold/1005 = 6.252231s`, `hot/1005 = 0.927951s`.
- Interpretation: full/probe payload bytes are not the dominant gap on the first-10 benchmark window, or the remaining probe encode/read savings are below noise. Reverted rather than keeping a new format policy with no measured benefit.
- Current best local point remains run `1003`: `cold/1003 = 6.203508s`, `hot/1003 = 0.925307s`; baseline remains `cold/938 = 6.058941s`, `hot/938 = 0.880468s`.

## 2026-05-21 fast iteration: lazy passed-content set materialization kept for now

- Change under test: `VerificationState` now records passed content-file identities in a vector during pass 1 and materializes a `FileIdentitySet` only if pass 2 actually needs membership tests for structural/implicit coverage or invalid-path recovery bookkeeping.
- Rationale: hot valid verification previously paid a hash-set insertion for every content dep even when no later pass queried that set. This keeps the authority and verification decisions identical while delaying index construction until it has a consumer.
- Build: `nix build -L . --builders ''` passed; expression tests reported 1907 passed / 3 skipped, functional tests passed/skipped as expected.
- 10-commit no-debug/no-stats benchmark run `1007`:
  - `cold/1007`: mean `6.162885s`, median `5.775434s`, min `0.901838s`, max `13.998431s`.
  - `hot/1007`: mean `0.922111s`, median `0.922098s`, min `0.908485s`, max `0.940179s`.
- Comparison:
  - Post-revert control `1006`: cold `6.180594s`, hot `0.926097s`.
  - Baseline `938`: cold `6.058941s`, hot `0.880468s`.
- Decision: keep for now. It is not sufficient, but it is directionally positive and low-complexity. The remaining gap is still dominated by verification/replay work, not this hash-set insertion alone.

## 2026-05-21 quick-run findings after power-loss recovery

Baseline for 10-commit no-debug/no-stats iteration remains run 938 at commit 92a3df1ab706cf514357ae57b367050b373ff146:
- cold/938 mean 6.058941s, median 5.452368s
- hot/938 mean 0.880468s, median 0.877134s

Current-tree experiments:
- run 1009: lazy/current Git identity recomputation inside exact proof was a hot-path regression; rejected.
- run 1010: removing inline Git recomputation restored hot time to roughly previous current-tree behavior.
- run 1011: direct verifier-session membership check avoided a store hop and improved hot mean to 0.919138s; kept for now.
- reverse-replay session row rebuild was incorrect: durableOrder, not vector order, is the authority for newest session rows. Existing test TraceStoreTest.Session_CurrentNodeIndex_RebuildUsesMaxDurableOrder caught this.
- run 1012: safe one-pass session-row rebuild preserved correctness but was not material: cold mean 6.185662s and hot mean 0.922115s. This does not close the baseline gap.

Conclusion: micro-optimizing startup scans is not enough. The next candidate must remove whole hot-path phases, especially exact-hit verification/dependency loading work, without weakening soundness or precision.

- run 1013: complete-probe exact-session preflight was implemented and tests updated to allow non-zero complete probes as covering exact-session input, but the 10-commit benchmark regressed (cold mean 6.190712s, hot mean 0.923284s). Reverted. The likely reason is that this benchmark does not have enough probe-covered exact hits to pay for probe decode/hash/proof work; keeping the old full-verification rule avoids extra complexity until we have a narrower serving index.

- run 1014: fused current-node lookup + exact preflight into one store-lock/blocking-pool hop. Build/tests passed, but 10-commit benchmark regressed (cold mean 6.177118s, hot mean 0.927458s). Reverted the fusion. This suggests the extra object/result movement or altered scheduling outweighed the removed coroutine hop for this workload.

## 2026-05-21: unsafe exact-hit oracle measurement

A diagnostic-only hot-hit oracle was added behind `NIX_EVAL_TRACE_TRUST_CURRENT_NODE_FOR_BENCHMARK=1` to answer one question: if current-node exact hits could skip full `loadFullTrace` / dep replay / verifier pass machinery and decode only the cached result, would we beat the pre-schema SQLite baseline?

10-commit no-debug/no-stats benchmark result:

- baseline `cold/938`: mean 6.058941s, median 5.452368s
- baseline `hot/938`: mean 0.880468s, median 0.877134s
- recent safe retained `cold/1012`: mean 6.185662s, median 5.794811s
- recent safe retained `hot/1012`: mean 0.922115s, median 0.923769s
- unsafe oracle `cold/1015`: mean 6.205837s, median 5.826041s
- unsafe oracle `hot/1015`: mean 0.678324s, median 0.680830s

Conclusion: exact hot-hit performance can beat the baseline if the serving path is reduced to lookup plus hash-bound result decode. The current slowdown is mainly verifier/replay machinery, not SQLite I/O. The oracle is unsound by construction because it trusts current-node metadata and does not prove dependency coverage; it must not become production behavior. It should guide a real exact-replay descriptor design.

Implications for the next architecture:

- Keep the disconnected runtime model: no SQLite connection during evaluation, and no DB lock except startup/shutdown fallback paths.
- Keep the soundness/precision tests, especially the dep taxonomy, shape/value distinction, structural override tests, and builtin analogue coverage.
- Keep lazy materialization work that aligns with serving only demanded observations.
- Keep direct verified-session memo access where it avoids scheduler/store-lock hops without weakening proof semantics.
- Keep content-addressed capsules, result sidecars, stable textual float encoding, and wire-format discipline as building blocks for hash-bound descriptors.
- Do not rely on mutable session rows as authority. Persistent session/current indexes should route only; hash-bound immutable descriptor bytes must authorize replay.
- Do not revive probe-only preflight, MRU, or SQLite row-shape micro-optimizations as primary work. Those do not remove the dominant phases.
- Treat SQLite writeback improvements as secondary unless they feed the immutable generation/published-pack model.

Next target: replace the diagnostic oracle with a sound `ExactReplayDescriptor` / immutable generation snapshot path. A descriptor should bind at least session/source identity, attr path identity, trace/result identities, no-volatile status, dependency coverage facts, result hash, and any resolver material needed for trace-context/store/environment deps. If absent or not fully covering, fall back to the current verifier.

## 2026-05-21: ExactReplayDescriptor implementation DAG

Goal: replace the diagnostic `NIX_EVAL_TRACE_TRUST_CURRENT_NODE_FOR_BENCHMARK=1` oracle with a sound exact-hit serving path that preserves eval-trace soundness/precision while skipping verifier replay for eligible exact hot hits.

DAG:

1. `A: descriptor semantics and wire API`
   - Defines `ExactReplayDescriptor` as an authoritative, hash-bound replay proof object.
   - Must fail closed for unknown version/flags/coverage.
   - Must distinguish routing hints from authority.
   - Owner: Wave 1 worker Avicenna.

2. `B: storage/load/publication mapping`
   - Identifies where descriptors are built during shutdown/writeback or generation publication.
   - Identifies startup snapshot structures and exact lookup APIs.
   - Depends on A for final type names, but can be mapped in parallel.
   - Owner: Wave 1 explorer Hooke.

3. `C: verifier exact-hit integration mapping`
   - Replaces unsafe oracle with descriptor validation plus result decode.
   - Defines fallback and telemetry.
   - Depends on A for final validation API and on B for lookup API.
   - Owner: Wave 1 explorer Herschel.

4. `E: adversarial soundness review`
   - Runs in parallel with A/B/C.
   - Defines mandatory fields, rejection cases, and tests.
   - Owner: Wave 1 explorer Godel.

5. `D: integrator implementation wave`
   - Integrates A, then implements B/C in disjoint slices based on explorer findings.
   - Removes or quarantines the unsafe oracle after the sound path exists.
   - Owner: main integrator.

6. `F: verification and benchmark wave`
   - Only the main integrator builds/tests/benchmarks.
   - Required checks: targeted tests for descriptor rejection cases, full local build, 10-commit no-debug/no-stats benchmark against run 938 and recent safe run 1012.

Parallelism rule:

- Subagents must not build, test, benchmark, or run git.
- Workers must use disjoint write scopes.
- Explorers should return file/function anchors and patch plans, not broad rewrites.
- Integrator owns final conflict resolution, verification, and benchmark interpretation.

## 2026-05-21: Wave 1 subagent findings

Wave 1 results:

- `Avicenna`: implemented initial `ExactReplayDescriptor` API in `proof-descriptor.hh` only. It models descriptor identity, candidate validation, fail-closed feature bits, coverage classes, and descriptor digest construction. No verifier/storage wiring.
- `Hooke`: mapped storage/publication/load. Existing shadow descriptor generation already exists around `buildPersistentExactDescriptorsFor`, `snapshotPersistentExactGenerationSourcesLocked`, `buildPersistentExactGenerationExportFromSnapshot`, and `appendPersistentExactGenerationEntry`. The missing part is serving: loaded candidate rows only retain summary descriptor facts, not descriptor object refs/material for authoritative replay.
- `Herschel`: mapped verifier insertion points. The unsafe oracle lives in `Verifier::verifyAttrImpl` current-node branch and should be replaced with descriptor lookup before exact-session preflight. Sync verifier should get equivalent semantics. Current-node and candidate indexes are routing only.
- `Godel`: adversarial acceptance criteria. First implementation should be narrow, fail-closed, and same-session/same-generation if necessary. False negatives are acceptable; false positives are correctness bugs.

Integrator constraints for next wave:

- Do not accept digest-only proof obligations as sufficient for broad exact replay.
- Do not use `currentNodeIndex`, `ResultId`, `TraceId`, candidate row order, or session rows as proof authority.
- Do not cross sessions/local-id namespaces until remapping is explicitly specified and tested.
- Keep descriptor acceptance side-effect-light: no `VerifiedHash` or trace-scoped subsumption writes; only mark the trace verified after descriptor validation plus result payload hash binding succeeds.
- The first sound path may miss often. It should only serve when it can prove exact replay cheaply.

## Lazy exact hot-hit implementation DAG - integrator wave

Goal: beat the pre-schema SQLite hot-hit baseline without relaxing soundness. The diagnostic current-node oracle proved the serving cost can be lower than baseline, but it was unsound because it trusted mutable routing state. The production path must make mutable rows/indexes routing-only and accept a hit only when immutable, hash-bound descriptor evidence matches the current request and result payload.

DAG:

1. `A2: Descriptor contract hardening`
   Owner: descriptor worker.
   Files: `proof-descriptor.hh`, optionally `proof-descriptor.cc`.
   Output: fail-closed exact replay descriptor API with coherent version/feature/result/trace binding.

2. `B2: Storage exact replay helper`
   Owner: storage worker.
   Files: `sqlite-trace-storage.hh`, `sqlite-trace-storage.cc`.
   Depends on: existing descriptor helpers, existing candidate/current/result caches.
   Output: `tryAcceptExactReplayDescriptor(...)` that accepts only narrowly eligible rows and otherwise returns `nullopt`.

3. `C2: Descriptor contract tests`
   Owner: test worker.
   Files: tests only.
   Depends on: descriptor API shape.
   Output: fail-closed tests for required bits, unknown bits, binding mismatch, and coherent acceptance.

4. `D2: Adversarial architecture review`
   Owner: explorer.
   Files: none.
   Output: integration checklist and performance pitfalls.

5. `E2: Integrator verifier wiring`
   Owner: main integrator.
   Depends on: `B2` API.
   Files: `verifier.cc` plus any declaration fixes.
   Output: remove diagnostic env oracle, call storage helper before full trace load in async and sync paths, fallback on miss.

6. `F2: Persistence/generation expansion`
   Owner: main integrator after E2.
   Depends on: correctness of narrow path.
   Output: persist/load descriptor packs or equivalent immutable refs so fresh hot runs can use descriptor acceptance.

7. `G2: Verification and benchmark`
   Owner: main integrator.
   Depends on: integrated implementation.
   Output: build, targeted tests, then eval-trace-bench 10-commit iteration and 100-commit comparison against baseline when promising.

Hard acceptance constraints:

- False positives are correctness bugs; false negatives are acceptable.
- `currentNodeIndex`, session heads, candidate IDs, node stamps, and row order are routing hints only.
- Exact replay must not populate broad verified-hash or trace-scoped subsumption state.
- Result decode must remain centralized through existing cached-result decode paths.
- Any missing descriptor bytes, unknown feature, volatile/uncovered dependency, hash mismatch, or decode failure falls back to verifier replay.

## Lazy exact hot-hit wave status

Implemented so far:

- Removed the diagnostic `NIX_EVAL_TRACE_TRUST_CURRENT_NODE_FOR_BENCHMARK` oracle from verifier code.
- Added descriptor-bound exact replay validation types and focused descriptor tests.
- Added a narrow `SqliteTraceStorage::tryAcceptExactReplayDescriptor(...)` same-process helper.
- Wired verifier sync and async paths to attempt exact replay before exact-session preflight/full-trace load, with fallback on miss.

First verification build result:

- Production libraries compiled.
- Functional tests passed: 218 passed, 8 skipped, 0 failed.
- Expression tests: 1912 passed, 3 skipped, 1 failed.
- Failure was in the new descriptor fixture: `ProofDescriptorTest.ExactReplayDescriptorAcceptsCoherentCandidatePair` constructed a candidate without the required non-zero `CurrentNodeRef`. This was a test/fixture mismatch after tightening validation, not a production compile failure.
- Fixed by adding an `exactReplayCandidate(...)` helper that binds trace id, result id, and node stamp coherently to the descriptor.

Subagent findings incorporated:

- Fresh-process hot-hit improvement requires persisted immutable descriptor evidence. Same-process descriptors validate the semantics but cannot improve eval-trace-bench hot runs after restart.
- The minimal fast path should become a fixed-size precomputed validation-header comparison plus centralized result decode. Dynamic descriptor reconstruction, string comparison, DB lookups, stats/logging, and cache writes on the hot path are likely enough to lose the benchmark win.
- Additional adversarial tests needed beyond descriptor unit tests: same-process accept, stale current-node rejection, candidate binding mismatch fallback, missing persisted descriptor fallback, no broad cache side effects, and result decode corruption fallback.

Next implementation edge:

- Add compact immutable persisted-generation exact replay refs/headers loaded at startup.
- Treat manifest/current/session rows as routing only; exact replay after process restart must be authorized by validated generation descriptor/result evidence.
- Keep first persisted implementation narrow: positive exact-result hits only, complete descriptors only, no volatile inputs, no structural override ambiguity, fallback on any missing or mismatched evidence.

## Lazy exact-hit redesign task DAG

Legend: `CP` = critical path; `P` = parallelizable once dependencies are met; `S` = mostly serial.

- [ ] `N1 semantic-key` (`CP`, `S`): define the canonical semantic exact-hit key from query, demand, proof obligations, result identity, epochs, and hash algorithm. This is the authority key all later nodes bind to.
- [ ] `N2 immutable-exact-hit-record-format` (`CP`, `S`, depends on `N1`): define the append-only record/capsule format for an exact hit, including descriptor bytes, result payload binding, full-key validation material, and explicit fail-closed versioning.
- [ ] `N3 startup-lazy-index` (`CP`, `P`, depends on `N2`): load only generation manifests and compact routing metadata at startup; defer descriptor/result/object validation until a candidate survives semantic-key routing.
- [ ] `N4 hot-hit-fast-path` (`CP`, `P`, depends on `N3`): implement the common exact-hit path as semantic-key lookup plus minimal finalist validation, avoiding historical span scans and avoiding full generation validation on hot startup.
- [ ] `N5 writeback-publisher` (`CP`, `P`, depends on `N2`): publish newly accepted exact hits as immutable records through temp files, fsync, manifest-last publication, and atomic current-ref update; never mutate existing generations.
- [ ] `N6 tests` (`CP`, `P`, depends on `N1`, `N2`, `N3`, `N4`, `N5` as applicable): add targeted unit/regression tests for key determinism, record decode rejection, lazy index routing, hot-hit validation, writeback crash boundaries, and volatile/ineligible proof rejection.
- [ ] `N7 centralized-build-benchmark` (`CP`, `S`, depends on `N4`, `N5`, `N6`): run build/test/bench only from the centralized checkpoint owner after implementation slices land; individual implementation nodes should not run ad hoc benchmark variants.
- [ ] `N8 adversarial-review` (`CP`, `P`, depends on `N1` through `N7` design artifacts; repeats before serving cutover): review collision handling, descriptor coverage, current-observation validation, publication atomicity, stale-generation behavior, downgrade/version handling, and benchmark interpretation before enabling any serving authority.

Critical path order: `N1 -> N2 -> N3 -> N4 -> N6 -> N7 -> N8`, with `N5` required before production writeback cutover and allowed to proceed in parallel with `N3/N4` after `N2`.

Parallel work split:

- `N3 startup-lazy-index` and `N5 writeback-publisher` can proceed in parallel after `N2` because read-side routing and write-side publication share the record contract but not implementation state.
- `N6 tests` should be developed alongside each node, but final acceptance belongs to the centralized checkpoint.
- `N8 adversarial-review` can start on design notes early, then must repeat after `N7` produces a single benchmark/build result set.

## Lazy exact-hit integration and benchmark checkpoints

- Integration checkpoint 1: semantic-key and immutable record format are documented, encoded with explicit endian/version/hash bindings, and rejected fail-closed when any authority field is missing or mismatched.
- Integration checkpoint 2: startup opens generations without eagerly validating every descriptor/result object, while exact-hit candidates still require full semantic-key and capsule validation before replay.
- Integration checkpoint 3: hot-hit path proves it does not scan unbounded candidate history and does not depend on SQLite rows, path hints, native structs, local intern ids, or volatile observations.
- Integration checkpoint 4: writeback publisher uses temp-generation directories, writes payloads before indexes, writes manifest last, fsyncs files/directories, and atomically advances the visible generation ref.
- Integration checkpoint 5: adversarial review signs off that lazy validation has not converted routing indexes, manifests, or digest prefixes into authority.
- Benchmark checkpoint 1: only the centralized build/benchmark owner runs the comparison suite; implementation slices record expected impact but do not produce competing numbers.
- Benchmark checkpoint 2: report cold startup/open cost, first exact-hit latency, steady hot-hit latency, writeback shutdown cost, generation count sensitivity, and candidate-span worst cases.
- Benchmark checkpoint 3: compare against existing gates for `cold/900` and `hot/900`, plus a lazy-index-specific adversarial case with many generations and colliding route prefixes.
- Benchmark checkpoint 4: no serving cutover unless benchmark artifacts, exact-hit acceptance counters, skipped/ineligible counters, and mismatch counters are captured from the same build.

## 2026-05-21 checkpoint: authority/range-read scaffold build + 10-commit benchmark

Build checkpoint after integrating the first lazy exact-hit worker wave:
- `nix build -L . --builders ''` completed successfully.
- Expression tests: 1916 passed, 3 skipped.
- Functional tests: 218 passed, 8 skipped.

Benchmark checkpoint:
- Generated no-debug/no-stats 10-commit run `1018`.
- Compared against baseline run `938` over the 10 overlapping commits.
- Cold: baseline mean 6.058941s, target mean 6.181418s, delta +0.122478s, ratio 1.039868.
- Hot: baseline mean 0.880468s, target mean 0.922943s, delta +0.042475s, ratio 1.048395.

Interpretation:
- This wave did not improve performance, as expected: it only added serving-authority format, serving-v1 eligibility gates, and selected-object range reads.
- The still-missing performance win is fresh-process exact-hit serving that avoids full trace/capsule inflation and verifier passes for eligible hits.
- The next implementation wave must persist authority objects and wire a narrow serving path; otherwise the architecture remains a safer but slower version of the current verifier path.

Next DAG slice:
1. Persist exact-hit authority refs in generation rows, but only for `isServingExactHitV1Eligible` rows.
2. Write result payload authority objects independently of descriptors/capsules.
3. Write observation-material authority objects only when the validator can reconstruct full canonical material; reject all other observations.
4. Implement the fresh-process serving finalist path: metadata index routes, selected authority objects are range-read, digests/feature bits are validated, then result payload is decoded.
5. Keep TraceContext/parent-slot dependent hits rejected until parent replay or current-parent binding authority is implemented.
6. Benchmark each serving increment against run `938` with 10-commit no-debug/no-stats runs.

## 2026-05-21 lazy serving DAG details

DAG for the next architecture step:

1. Stable payload bytes
   - Need a durable byte range for the encoded `EncodedResultPayload` bytes that can be read without inflating `TraceCapsule`.
   - A `TraceResultDescriptor` is not enough: it authenticates result metadata and digest, but does not contain the payload bytes needed to materialize a `CachedResult`.
   - Current authority scaffold has `ResultPayloadObject` metadata with `payloadBytes`/`payloadByteDigest`; serving also needs a locator/ref for the actual payload bytes or an object encoding that includes those bytes.

2. Authority refs in row metadata
   - `GenerationExactHitRecord` stays a compact routing index.
   - `CandidateSnapshotRow::PersistentExactGenerationHit` needs refs to serving authority objects only for rows accepted by `isServingExactHitV1Eligible`.
   - Refs are routing metadata, not authority; serving must validate object bytes and semantic digests before decoding.

3. Fresh-process finalist serving
   - Candidate selection remains current-node/session-bound.
   - Serving reads only selected authority/result-payload ranges.
   - Serving validates: current node equals row binding, session/recovery keys match, source/policy/attr-path identity match, entry/result digests match, result hash recomputes from payload bytes, no volatile inputs, complete dependency coverage, and v1 eligibility.

4. Rejected until later
   - Non-empty proof obligations without full observation material/current validators.
   - TraceContext and parent-slot dependencies without parent replay/current-parent binding authority.
   - Containers if they require expensive local interning or process-local identity assumptions.
   - Any stale descriptor-only exact-hit path that still decodes from full capsules.

Concurrency plan:
- Explorer: map exact startup/load/lookup serving call graph and file hooks.
- Worker: persist row-level authority refs for eligible rows without enabling serving.
- Worker: produce result-payload authority bytes/objects in the generation pack.
- Explorer: adversarially review whether v1 is useful enough and what semantic checks are mandatory.

Integrator ownership:
- Merge non-conflicting patches.
- Add or adjust serving path only after payload bytes and row refs are both present.
- Build/test/benchmark locally after each integrated wave.

## 2026-05-21 subagent review update

Read-only call-graph scout confirmed the current fast-path hook:
- `Verifier::verifyAttrImpl` already calls `tryAcceptExactReplayDescriptor` before exact-session preflight and before `loadFullTrace`.
- `tryAcceptExactReplayDescriptor` already checks current node/session binding and calls `resolvePersistentExactAuthorityForRow`.
- `buildPersistentExactAuthorityFromGenerationHit` currently fails closed because `resolvePersistentExactGenerationHeader` has no generation resolver.

Adversarial review tightened v1:
- Empty-proof scalar-only v1 is sound as a canary but probably too narrow to beat baseline by itself.
- `Trivial` must be rejected for serving v1 because placeholder/missing/misc/null are not a clean scalar authority class.
- Non-empty proofs should be added by whitelisting domains with canonical observation material and current validators, not by weakening descriptor checks.

Implementation implications:
- Do not modify `decodeCachedResult` to accept descriptor-only authority; add a dedicated serving-result decode path once payload authority bytes exist.
- Keep `GenerationExactHitRecord` as routing metadata; put object refs either in a manifest exact-hit authority section or generation store authority pack metadata.
- Exact-hit publication must be atomic with authority/payload object publication.

## Lazy exact-hit serving DAG checkpoint (2026-05-21)

Goal restated: beat the pre-schema SQLite eval-cache baseline by avoiding fresh-process full trace/capsule inflation on hot exact hits, without losing soundness or precision. The immediate win target is a narrow v1 direct-serving subset: exact session/source/policy/attr-path match, exact-value demand, empty proof/zero observation coverage, scalar non-error non-trivial result, immutable payload authority bytes validated by byte and semantic digest.

DAG:

1. D0 buildable checkpoint: producer-side result-payload authority scaffold is buildable. `nix build -L . --builders ''` passed. Relevant results: `nix-expr-tests-run` 1916 passed / 3 skipped; `nix-functional-tests` 218 passed / 8 skipped.
2. D1 production metadata root: replace the reserved zero-only exact-hit authority manifest section with compact, fail-closed exact-hit authority routing refs. Subagent Worker A owns lifecycle manifest plumbing and must not build/test.
3. D2 payload materialization root: add a separate direct result payload decode API that does not weaken capsule-backed `decodeCachedResult`. Subagent Worker B owns codec/materialization helpers and must not build/test.
4. D3 integration join: verifier/storage code may use D1+D2 only after descriptor/session/policy/current-node validation and authority object range-read + digest validation. This join is integrator-owned.
5. D4 tests: focused tests for eligibility, corrupted refs fail-closed, scalar direct payload materialization, and no-serving for Trivial/container/error/non-empty proof.
6. D5 integrator verification: local build, then 10-commit no-debug/no-stats `eval-trace-bench` against baseline run 938; widen only if the result moves toward beating the baseline.

Open design constraint: manifest/index rows remain routing metadata. Authority comes only from immutable bytes plus recomputed byte/semantic digests and current-session/source/policy/query/demand validation. This is the same action-cache/CAS split pattern: metadata selects a candidate, content-addressed authority bytes authorize serving.

## Lazy exact-hit / cold-path DAG checkpoint (2026-05-21)

Goal: beat baseline run 938 from commit 92a3df1ab706cf514357ae57b367050b373ff146, not merely match it.

Current no-debug/no-stats 10-commit signal:
- Baseline 938, same 10 commits: cold mean 6.058941s, hot mean 0.880468s.
- Current run 1020: cold mean 21.457091s, hot mean 0.934020s.
- Interpretation: direct scalar exact-hit serving is not addressing the dominant cold bottleneck. Cold overhead is clustered on heavier commits, so startup/writeback/snapshot or broad scan work is still too expensive.

DAG:
- D0: Maintain sound direct scalar replay. Completed enough to build: payload/result hash validation, observation material validation, per-row authority pack ownership, and scalar codec tests.
- D1: Preserve lazy exact-hit sidecars across full snapshots. Blocking for any multi-commit cold benchmark because snapshots must not erase lazily loaded serving metadata.
- D2: Populate directServingPayload for header-backed exact hits. Independent of D1; needed for same-process/header-backed exact-hit fast path.
- D3: Reduce authority-pack startup cost. Depends on D0; can be designed in parallel. The manifest should not eagerly load large serving authority bytes if exact hits are lazy.
- D4: Make writeback dirty-only. Independent of D2/D3; may depend on D1 semantics for preserving existing rows.
- D5: Integrate first wave, build locally, then run 10-commit eval-trace-bench. Only after a positive signal should we run a 100-commit comparison.

Active subagent wave:
- Russell: D1 snapshot sidecar preservation.
- Bohr: D2 header-backed direct payload population.
- Poincare: D3 authority-pack architecture.
- Noether: D4 dirty-only writeback.

Open risks:
- Full snapshots may still drop exact-hit sidecars for lazy rows.
- Header-backed exact hits may still fall back to capsule decode.
- Authority-pack bytes are likely too eager/heavy for cold startup.
- Dirty writeback may still scan total rows, causing cumulative cold slowdown.
- Broader capsule semantic-hash recomputation is still separate soundness work.

## Lazy/interleaved verification DAG and parallel wave (2026-05-21)

Goal: beat baseline run 938, not merely recover pre-schema performance. Baseline run 938 for the current 10-commit comparison set is cold mean 6.058941s and hot mean 0.880468s. Current best run 1021 is cold mean 15.873457s and hot mean 0.924451s. A debug cold miss isolated the dominant cold cost to Git manifest / trace-capsule encoding: about 19.3s spent encoding 7 rows / 6.36 MiB.

Core architecture decision: split routing, serving authority, and full provenance. Routing rows select candidates only; immutable authority bytes authorize hits; full provenance remains exact and durable but must not be decoded or re-encoded on the hot path. This matches the object/index split used by Git/CAS systems: indexes route, content-addressed objects authorize.

DAG nodes:
- N0 SessionAuthority: schema/provider epoch, semantic session key, recovery key, hash algorithm, runtime roots, policy, eval settings.
- N1 AttrLookup: attr path to current node binding.
- N2 CandidateBinding: candidate row for current node/session/recovery/source identity.
- N3 TraceHeader: trace id, result id, full trace hash, trace hash, dep-set hash, result hash.
- N4 ResultPayloadAuthority: result bytes plus byte/semantic/result-hash validation.
- N5 ExactReplayAuthority: descriptor/generation authority validated against N0-N4.
- N6 CompleteProbeAuthority: complete probe that recomputes the trace hash tuple and can run existing dep semantics without full capsule decode.
- N7 LazyDepProjection: deps-only view over a complete probe or full capsule, preserving canonical dep order.
- N8 FullProvenanceObject: exact full deps/result provenance persisted durably.
- N9 MaterializedShape: attr/list shape and child thunks; children remain lazy.
- N10 ReplayToken: lazy dep replay into an enclosing active recording context.

DAG edges:
- N0 -> N1: no lookup before session/runtime-root authority.
- N1 -> N2 and N1 -> N3: current node selects candidate and trace/result identity.
- N2 + N3 + N4 + N5 -> ServeExactHit: serve without full deps, but do not treat route rows as proof authority.
- N3 + N6 -> VerifyCoveredTrace -> N4 -> ServeVerifiedHit: complete probes may authorize covered traces.
- N3 -> N7 -> FullVerify -> N4 -> ServeVerifiedHit: fallback path for non-covered/non-exact traces.
- ServeHit -> N9: materialize only visible shape; children verify lazily.
- ServeHit + active recording -> N10: replay can be delayed if the enclosing trace is not committed.
- FreshEval -> canonical deps/result -> N8 -> durable generation ref: flush cannot report success until required provenance objects are durable.

Hard soundness constraints:
- `currentNodeIndex`, candidate rows, session rows, local ids, row ids, trace ids, result ids, node stamps, and manifest row positions are routing state only.
- Authority comes from immutable bytes plus recomputed byte digests, semantic digests, trace/result hashes, and current session/source/policy/query/demand validation.
- Same full trace with different result remains distinct: `(full_hash, result_hash)` is the durable capsule key.
- Direct v1 serving remains scalar, exact-value, empty-proof, zero-coverage, non-volatile, non-error, and non-container until attr/list observation validators exist.
- Attribute presence, missing attrs, attr selection, and key-set enumeration are distinct properties. `#has:false` must not be treated as generic positive attr selection if future entailment reasons from domain/polarity.
- Non-empty proof obligations need current observation validators before serving. Digest-only obligations are not enough.
- Full provenance persistence is not optional. We can change the format, split it, or make it append-only, but we cannot drop deps or precision.

Parallel wave assignments:
- Hypatia: map current lazy hot-hit path and AttrCursor analogue; completed with N0-N10 DAG and risks.
- Bernoulli: design routing / serving sidecar / full provenance split; completed with concrete format planes and publish order.
- Ptolemy: adversarial observation and attrset semantics; completed with invariant/test list and descriptor-domain gaps.
- Raman/Erdos: trace-capsule/writeback bottleneck analysis; completed with copy/encoding/probe/compression hot spots.
- Lagrange: narrow pending encoded-payload cache; landed locally, but it mostly moves capsule encoding earlier unless paired with encoder/format changes.
- Locke: backend/format architecture comparison; active, no build/test/edit.
- James: narrow no-wire-format encoder micro-optimizations; active, no build/test.

Immediate implementation order:
1. Keep direct scalar serving fail-closed; do not widen to attrs/lists yet.
2. Make the existing encoded-payload cache safe and useful for both Git and SQLite writeback, but do not treat pending bytes as durable proof authority.
3. Reduce no-format-change encoder copying and duplicate work where low risk.
4. Add descriptor-domain/attr observation tests before any non-empty proof or attr/list serving expansion.
5. Use 10-commit no-debug/no-stats benchmark after each integrated source wave; only then decide whether to invest in the larger routing/sidecar/provenance format split.


## Backend architecture review result (2026-05-21)

Locke's architecture pass ranked the likely path to beat baseline as a file-native immutable generation store rather than continued Git/SQLite manifest work.

Recommended target architecture:
- `objects.pack`: append-only CAS frames for trace capsules, result payload authority, exact-hit authority, observation material, and trace binding. Each object carries kind, version, size, byte digest, and semantic digest.
- `segments/*.elog`: append-only dirty event records for trace object refs, capsule pair refs, candidate rows, runtime roots, and exact-hit refs. Normal shutdown appends dirty facts only; it does not rewrite old rows.
- `*.idx`: fixed-width sorted mmap indexes for current-head lookup, recovery candidates, trace-by-full-hash, capsule-pair lookup, and exact-hit lookup. Index rows route only.
- `CURRENT`: atomic generation head. Publication is object/segment/index write followed by atomic current-ref update.
- Compaction: threshold-driven merge into compacted generations; never paid on the normal cold eval shutdown path.

Why this should beat baseline:
- Cold miss current bottleneck is generation/capsule encode and manifest publication, not evaluation itself. A dirty-only append path makes shutdown proportional to newly produced object/ref bytes and avoids full snapshot/reprojection work.
- Hot hit current gap is small but real. A mmap routing index plus exact authority object can keep the verifier hook before `loadFullTrace()` and avoid full capsule inflation for covered exact hits.

What to stop investing in as primary architecture:
- Git/libgit2 as production backing store. It provides content addressing, but current commit/tree/ref/manifest publication overhead is directly in the measured cold regression.
- Probe-only/lazy-verification-only improvements. They help some fallback paths but do not remove the cold publication bottleneck.
- SQLite-only tuning. A SQLite hybrid can be a fallback, but it is unlikely to beat a file-native dirty append plus mmap index design.

Implementation DAG for backend replacement:
1. Storage substrate: write dirty-only binary segments and CAS objects from the existing in-memory snapshot.
2. Mmap routing indexes: build and validate fixed-width indexes at startup without decoding authority packs or trace capsules.
3. Exact authority object relocation: move serving authority/result bytes out of monolithic manifests into CAS pack refs.
4. Verifier fast path: keep exact replay before full trace load; validate current session/source/policy/query/demand plus authority bytes/digests.
5. Conflict/order model: replace durable vector order with `(generation, segment, recordOffset)` or equivalent monotonic order.
6. Compaction: merge segments off the normal critical path.
7. Retire production Git/SQLite path once equivalence and benchmark gates pass.


## No-wire-format encoder micro-optimization wave (2026-05-21)

James landed a narrow encoder patch in `trace-capsule.cc` with no schema, compression, framing, or public API changes.

Reported changes:
- `encodeTraceCapsule` reads `record.result` by reference instead of copying `EncodedResultPayload`.
- Complete probe encoding reads `record.deps` directly rather than copying every dep into `TraceCapsuleProbeDep` rows.
- Encoded dependency/probe rows hold pointers to `DepHashValue` instead of copying hash/value variants.
- Capsule string interning uses transparent `std::string_view` lookup to avoid temporary string allocation on lookup hits.
- Dirset encoding sorts pointers to existing members instead of copying dirset definitions before sorting.
- Reserve estimates include interned string bytes and result payload/probe sizing.

Integrator risk notes:
- Heterogeneous lookup depends on Boost.Unordered support for transparent hash/equality; build will validate this.
- Pointer-backed dep hashes must not outlive the input `record.deps`; the encoder remains synchronous, so this is acceptable if no rows escape the function.
- This should reduce encode allocation/copy overhead, but it does not eliminate zstd compression or the fundamental cold-path provenance publication cost.

Additional integrator patch:
- SQLite shutdown staging now reuses durable raw capsule bytes or fresh `encodedTraceCapsulePayloadsByPair` bytes before falling back to `encodeTraceCapsule`, matching the Git writeback cache behavior.
- Pending encoded payloads remain writeback-only; they are not inserted into durable raw capsule maps and cannot authorize replay.


## Benchmark after first DAG integration wave (run 1022)

Build result after integration:
- `nix build -L . --builders ''` passed.
- Functional tests: 218 OK / 8 skipped.
- Expr tests: 1919 passed / 3 skipped.

No-debug/no-stats 10-commit benchmark:
- Baseline run 938, same commits: cold mean 6.058941s, hot mean 0.880468s.
- Prior current run 1021: cold mean 15.873457s, hot mean 0.924451s.
- New run 1022: cold mean 15.860358s, hot mean 0.920705s.

Interpretation:
- The no-wire-format encoder copy/allocation reductions are buildable and slightly helped hot wall time, but they did not materially improve cold wall time.
- Heavy cold commits remain about 30s, matching the previous shape. The dominant cost is still full provenance/capsule publication work, not the small copies removed in this wave.
- The SQLite staging cache reuse is still correct integration hygiene, but current benchmark path is Git generation writeback, so it does not move run 1022.

Refined direction:
- Continue with high-leverage publication changes: compression cost isolation, payload-object append/reference instead of manifest payload copying, and ultimately file-native dirty segment + mmap routing indexes.
- Do not invest further in micro-optimizing `encodeTraceCapsule` unless perf/debug counters show it beats compression/publication cost.


## Second parallel wave result (2026-05-21)

Compression check:
- Trace capsule storage compression is already zstd level 1, so changing compression level is not available as a no-format-change win.
- Any further compression win requires a storage-envelope/format change: uncompressed full provenance, split blocks, or a different object publication model.

Git staging copy reduction:
- `EncodedGitTraceRow` now stages payloads by `std::string_view` plus optional shared owner instead of always copying raw/fresh payload bytes into an owning `std::string`.
- Raw in-memory capsules and freshly cached encoded payloads reuse their existing shared ownership; fallback re-encoded payloads are moved into shared ownership.
- Path-backed raw capsules remain path-backed and are not loaded just for staging.
- Wire format and authority semantics are unchanged.

Hot-path review:
- The remaining hot gap is about 40 ms: run 1022 hot mean 0.920705s vs baseline 0.880468s.
- Likely eager work: `replayTrace`/`loadFullTrace` under active recording, materialized result shape creation, complete probe preflight for scalar hits, authority pack loading, and runtime/source identity checks.
- Safe lazy rule: lazy replay may produce verified positive scalar observations; absence/key-set/list-length/container shape need a verified completeness boundary.

File-native MVP:
- Build a file-native backend beside the current Git/SQLite path first.
- Reuse existing generation primitives: `GenerationIndexRecord`, `GenerationExactHitRecord`, `ServingObjectRef`, `GenerationPackBuilder`, `appendPersistentExactGenerationEntry`, `CandidateSnapshotRow`, `PersistentExactGenerationExport`, and existing raw capsule CAS helpers.
- Add `generation-segment-log.hh/cc` and `generation-mmap-index.hh/cc`.
- Segment layout stores dirty append-only segment directories under `eval-trace-v<schema>-<hash>.segments/`, with `HEAD`, `segments/<seq>-<digest>/`, fixed indexes, packs, metadata, and `objects/capsules/` CAS payloads.
- Atomic publish: write CAS objects and temp segment, fsync, take publish lock, re-read HEAD, finalize metadata, rename segment, atomically update HEAD, fsync dirs, then mark dirty rows clean.
- Startup: read HEAD, validate reachable segments, mmap fixed indexes, rebuild minimal routing/raw-capsule projections, but do not eagerly read authority/result/descriptor packs.
- Bring-up policy: shadow writer, shadow loader, primary loader with fallback, primary writer with Git fallback, then retire Git only after equivalence and benchmark gates.

Critical implementation DAG:
1. Format/mmap indexes.
2. Segment writer.
3. Lifecycle shadow write integration.
4. Startup loader/rebuild projections.
5. Exact-hit resolver over segment handles.
6. Mode/fallback controls.


## Benchmark after Git staging-view patch (run 1023)

Build result:
- `nix build -L . --builders ''` passed.
- Expr tests remained 1919 passed / 3 skipped.

Run 1023 benchmark:
- Generated with `nix run .#eval-trace-bench -- generate --num-commits 10`.
- Cold shape was unchanged during the run: heavy commits stayed around 30s.
- This confirms that avoiding staging payload copies is not the dominant cold issue. Full capsule/provenance publication still dominates.

Next step:
- Stop spending time on no-format-change copy reductions as the primary strategy.
- Start the file-native segment-log MVP implementation in slices, with the initial goal of shadow writing dirty-only segments while keeping Git writeback as fallback/authority until loader equivalence is established.


## Lazy verified cache DAG, integration plan, and delegation notes

Goal: beat the pre-schema SQLite-backed baseline across cold and hot wall time, not merely recover parity. The current eager writeback/design keeps preserving exactness, but the heavy cold path is dominated by full manifest/capsule construction and the hot path still pays too much for reconstruction/verification work. The next direction is a lazy, exact, verification-first cache path: make the fast path answer only when a compact authority record proves the requested value under the current identity, and defer or avoid full provenance materialization unless the evaluator actually needs it.

Non-negotiable semantics:
- The database/cache is authoritative only for values proven by complete observations under the relevant evaluation identity/fingerprint.
- Session identity must not be erased into mutable rows. Session-like data is an immutable run/build/evaluation fingerprint or generation label, not a mutable source of truth.
- Fast hits may be lazy only for observations whose preservation properties are exact: direct scalar values, known attribute presence, known absence, known keyset completeness, known list length, and equivalent analogues must each have explicit authority bits rather than relying on incidental trace shape.
- Anything not proven falls back to normal evaluation or the existing complete trace path; no approximate hit is acceptable.
- New persistence must be append-only or copy-on-publish with crash-safe HEAD/generation publication. Only HEAD-reachable generations count.

DAG:
- D0: Restore buildable base after interrupted work. Remove untracked helper-file Meson references or explicitly track/commit those files before using them.
- D1: Preserve useful existing optimizations. Keep no-wire-format capsule encoder improvements, durable/fresh payload reuse, and benchmark workflow improvements. Do not keep changes that merely add extra shadow write cost without serving-path benefit.
- D2: Define authority semantics. Enumerate observation kinds and exact preservation requirements for scalars, attrsets, lists, paths, strings, derivations, and builtins/analogues discovered during prior instability work.
- D3: Refactor session/generation identity. Replace mutable session-row thinking with immutable cache generation descriptors: evaluator build identity, source/flakeref fingerprint, schema/provider epoch, hash algorithm, and complete observation authority.
- D4: Implement lazy verified lookup. Add a compact exact-hit/authority route that can prove direct requested results without loading full trace/provenance. Fallback remains exact existing evaluation.
- D5: Implement append-only file-native publication path. Publish compact generation segments atomically with CAS/object refs and an immutable HEAD/generation chain. Keep SQLite only if it proves faster or as an optional debug/indexing tool, not as the assumed production backend.
- D6: Integrate load path. At startup, load only routing/authority indexes needed for fast hits. Avoid materializing full traces/capsules until a fallback/diagnostic path requires them.
- D7: Adversarial soundness review. Attack stale identity, partial observations, attrset absence/keyset completeness, source fingerprint drift, crash/restart, concurrent writers, corrupted files, and old-generation interaction.
- D8: Central verification. Only the integrator builds, runs tests, and benchmarks. Subagents must not run nix, tests, perf, benchmarks, or git.

Wave 1 assignments:
- Semantics reviewer: D2 and D3. Produce an exact observation/authority taxonomy and identify which hits can be served lazily without precision loss.
- Lazy lookup implementer: D4. Prototype in existing tracked storage files only, prioritizing a narrow scalar/direct-result fast path with conservative fallback.
- File-native implementer: D5 and D6. Fold minimal segment append/load support into existing tracked generation-store files, avoiding new untracked source/header files unless the integrator explicitly commits/tracks them later.
- Adversarial reviewer: D7. Review proposed architecture and current retained changes, focusing on ways the fast path could become unsound or slower than baseline.

Integrator rules:
- Subagents do not build, test, benchmark, run perf, or use git.
- Subagents must keep write scopes disjoint.
- Integration proceeds in waves: collect outputs, resolve conflicts, build locally, benchmark with 10 commits for iteration, then 100 commits only for candidate comparison against baseline run 938 or a freshly regenerated no-debug/no-stats baseline.

Integrator interim criteria before merging wave outputs:
- A change is worth keeping only if it reduces eager full-provenance work, reduces unavoidable serialization/copying, or improves soundness of lazy authority decisions.
- A file-native backend only helps if the read path can mmap/load compact routing and authority records without reconstructing trace capsules. A shadow writer that writes extra data on top of Git/SQLite is diagnostic only and should not be part of performance comparisons.
- Lazy exact hits need a proof object, not just a cached value. The proof must bind result bytes to evaluation identity, source identity, schema/provider epoch, hash algorithm, and the relevant observation authority kind.
- Attribute-set laziness must distinguish positive selection, known absence, full keyset completeness, and partial keyset observations. Treating these as the same is unsound.
- Candidate implementation slices should be benchmarked in this order: hot scalar/direct hit, hot attr selection hit, cold writeback without full manifest/capsule reconstruction, then full 100-commit comparison.

Wave 1 integration notes:
- Semantics review says lazy hits require explicit observation authority. Missing cache rows are never negative evidence. Attrsets need distinct authority for presence, absence, complete keyset, selection, and value ref. Lists need distinct length and element authorities. Control-flow/effect/error builtins require exact force-frontier/effect authority.
- Session identity is not semantic authority. Future design should use immutable generation identity binding source snapshot, eval options, evaluator/provider epoch, hash algorithm, input/store/ambient dependencies, and observation authority.
- Adversarial review says each lazy read must be pinned to one immutable snapshot/generation. Route/index rows are filters only; they are not authority. Authority bytes must be range-read and verified before serving.
- Tracked-file mmap prep landed in generation-format: fixed-width byte views and explicit wire decoding helpers. This avoids native struct casts and creates a path to mmap routing without eager vector decode.
- Dormant file-native segment-log prep landed in generation-store: segment metadata, HEAD helpers, dirty staging, and locked publication helpers. Lifecycle does not call them yet, so they should add no benchmark cost.
- Active lazy prep landed in sqlite-trace-storage: fresh dirty scalar/no-observation exact rows can attach direct ResultPayloadAuthority from the in-memory result payload cache and avoid capsule-backed materialization. Preconditions are conservative and fall back if missing.

Next integration checks:
- Build with local builders to catch compile/interface errors from subagent patches.
- If build passes, run a 10-commit no-debug/no-stats eval-trace-bench to measure whether the active fresh fast path changes hot behavior. Expect minimal cold effect because the file-native path is dormant.
- If build fails, fix only compile/interface issues first; do not expand the design during build repair.

Wave 1 build and benchmark result:
- Build passed after integration.
- Functional tests: 218 OK, 8 skipped.
- Expr tests: 1919 passed, 3 skipped, 2 disabled.
- 10-commit benchmark run: 1024, no debug/stats.
- Soundness: all outputs match reference.
- Baseline for this comparison: run 938, no debug/stats, same 10 commits.
- cold/1024 mean wall time: 16.03s.
- cold/938 mean wall time: 6.06s.
- hot/1024 mean wall time: 0.93s.
- hot/938 mean wall time: 0.88s.
- Interpretation: wave 1 is compile-valid and sound under existing tests, but does not materially improve performance. It confirms that dormant file-native APIs and fresh direct-serving prep are insufficient. To beat baseline, the next wave must remove full cold publication/replay work and make the primary hot path load compact authority/result records directly from pinned immutable generation bytes.

Next-wave implementation target:
- Do not benchmark shadow file-native writes as a win. They add work without replacing Git/SQLite.
- Integrate fixed-row wire views into startup/load so route indexes can be kept as bytes/mmap-like views instead of eager decoded vectors.
- Implement a primary fresh-process exact scalar serving path from immutable generation authority bytes, not from current process-only cache.
- Split authority/result objects from full trace provenance so hot hits do not call loadFullTrace/decode capsule.
- Only after that, wire file-native dirty append as a replacement publication path and compare shutdown/cold again.

## 2026-05-22 run 1030 after candidate-row session/recovery indexes

Change:
- Added per-session and per-recovery candidate-row indexes so `bulkLoadSessionRowsLocked()` no longer scans every historical candidate row when rebuilding current/recovery session routing state.
- Kept complete-probe exact-session preflight intact. Removing it failed `TraceStoreTest.CompleteProbeProjection_NonZeroCompleteProbeServesExactSessionPreflight` and `TraceStoreTest.ExactSessionPreflight_CompleteNonZeroDepBypassesFullVerification`, so it is part of the current exact-hit contract.

Build status:
- `nix build -L . --builders ''` passed.
- Functional tests and expression tests passed in the build output.

No-debug/no-stats short benchmark against baseline run `938`:

| run | cold mean wall | hot mean wall | soundness |
| --- | ---: | ---: | --- |
| `938` pre-schema SQLite baseline | 6.06s | 0.88s | PASS |
| `1029` before candidate-row session/recovery indexes | 6.29s | 0.91s | PASS |
| `1030` after candidate-row session/recovery indexes | 6.15s | 0.91s | PASS |

Interpretation:
- The candidate-row indexes improved cold variance/mean relative to `1029`, but they still do not beat baseline `938` and do not move hot time.
- Small routing/index maintenance tweaks are not enough. The remaining gap is in serving exact hits through the current verifier/replay shape and in cold precision/source-proof behavior.
- Next implementation focus should be a safe exact-hit fast path or lazy authority path that avoids unnecessary verifier/result/projection work while preserving complete-probe and full-key validation semantics.

## 2026-05-22 run 1031 after exact-replay gate and materialized-result-cache trial

Change:
- Added an early empty-trace gate before generation direct-serving authority object decoding. This is safe because the current v1 direct-serving authority only accepts empty/no-observation scalar exact hits; non-empty traces must fall back before authority-object decode.
- Trialed a `ResultId` + `ResultHash` materialized `CachedResult` cache after the existing trace/result/candidate authorization checks.

Build status:
- `nix build -L . --builders ''` passed.
- Functional tests: `218` passed, `8` skipped.
- Expression tests: `1919` passed, `3` skipped.

No-debug/no-stats short benchmark against baseline run `938`:

| run | cold mean wall | hot mean wall | soundness |
| --- | ---: | ---: | --- |
| `938` pre-schema SQLite baseline | 6.06s | 0.88s | PASS |
| `1030` candidate-row indexes | 6.15s | 0.91s | PASS |
| `1031` + direct-serving empty gate + materialized-result-cache trial | 6.22s | 0.91s | PASS |

Interpretation:
- The materialized-result-cache trial did not improve hot time and appears to add cold overhead/noise. It was removed after the benchmark rather than keeping unproven complexity.
- The early empty-trace gate is retained: it is a cheap fail-closed guard and removes wasted authority-object decoding for non-empty traces if that path is reached.
- Continued local verifier/cache tweaks are not enough to beat baseline. The next concrete target is cold writeback/startup overhead: skip eager verification/reread of already-present payload objects in the normal native append path, and then move toward indexed/lazy generation segments.

## 2026-05-22 run 1032 after earlier verified-trace memoization

Change:
- Moved the current-node `verifiedTraceIds` fast path before exact-replay descriptor lookup and exact-session/complete-probe preflights in both sync and async verifier paths.
- Kept result serving through `decodeCachedResult`, so the path-scoped trace/result capsule binding remains enforced.
- Added a small dirty-only reserve cleanup in native manifest encoding.
- Removed the materialized-result-cache trial before this build.

Build status:
- `nix build -L . --builders ''` passed.
- Functional tests passed (`218` OK, `8` skipped in the build output).
- Expression tests passed (`1919` passed, `3` skipped).

No-debug/no-stats short benchmark against baseline run `938`:

| run | cold mean wall | hot mean wall | soundness |
| --- | ---: | ---: | --- |
| `938` pre-schema SQLite baseline | 6.06s | 0.88s | PASS |
| `1030` candidate-row indexes | 6.15s | 0.91s | PASS |
| `1031` materialized-result-cache trial | 6.22s | 0.91s | PASS |
| `1032` earlier verified-trace memoization | 6.16s | 0.91s | PASS |

Interpretation:
- The memoization placement is semantically cleaner and avoids redundant preflight work when a trace is reused, but this benchmark did not show a wall-time win.
- Therefore the short-run hot gap is likely startup/native-manifest fixed cost, one-result-per-trace serving cost, or process noise rather than repeated same-trace preflight work.
- The next pass needs stats/perf attribution or a more structural change: indexed/lazy generation metadata and/or a real exact scalar authority serving tier.

## Benchmark notes: exact-session proof attempts 1033-1036

- Run 1033: GitRevisionIdentity exact-session proof was made less pessimistic. It moved misses from Git identity to TraceContext/StructuredProjection, but no no-debug performance win. Cold mean ~6.26s, hot mean ~0.91s vs baseline 938 cold 6.06s/hot 0.88s.
- Run 1034: naive TraceContext/StructuredProjection spot checks proved some rows but checked too many structured deps. Debug showed exact-session proof hits increased, but structured dep hash work dominated; no-debug hot regressed to ~0.95s.
- Run 1035: grouped structured source guard removed most repeated structured checks but still lost. Cold mean 6.27s, hot mean 0.90s, soundness PASS. Conclusion: spot-checking is not a profitable hot-path shape for the benchmark workload.
- Run 1036: reverted spot-check path and tried exact-session preflight before exact-replay descriptor. Build passed; soundness PASS. Cold mean 6.19s, hot mean 0.91s vs baseline 938 cold 6.06s/hot 0.88s. Conclusion: micro-ordering is not enough; next work must avoid whole classes of hot-hit verifier/load work rather than rearranging them.

## 2026-05-22 run 1037: StructuredProjection exact-session coverage

Change tested: allow `StructuredProjection` dependencies to be covered by exact-session Git authority when their governing repo id is included in the covered Git repo set.

Benchmark: no-debug/no-stats 10-commit run `1037`, compared to fixed baseline `938`.

Results:
- cold: `1037` mean wall `6.22s`; baseline `938` mean wall `6.06s`; PASS soundness.
- hot: `1037` mean wall `0.92s`; baseline `938` mean wall `0.88s`; PASS soundness.

Conclusion: not sufficient. The change is semantically plausible for source-derived structured projections, but it does not beat baseline. Next step is to stop spending cycles on isolated exact-session proof micro-tweaks and implement a direct-hit/lazy-authority path: candidate row selection should be O(1), authority should be compact and preclassified, and hot hits should avoid loading/decoding/verifying full traces unless a required observation is not covered.

## 2026-05-22 run 1038: TraceContext exact-session proof

Change tested: `exactSessionProofCovers` now handles TraceContext observations by recursively proving the referenced current node and comparing the recorded TraceValueContext/TraceParentSlot hash, rather than rejecting TraceContext unconditionally. This uses the same current-node proof semantics as full verification and preserves the cycle guard via `VerificationSession::inProgressTraceIds`.

Build: `nix build -L . --builders ''` passed. Expr tests: `1919` passed, `3` skipped.

Benchmark: no-debug/no-stats 10-commit run `1038`, compared to fixed baseline `938`.

Results:
- cold: `1038` mean wall `6.18s`; baseline `938` mean wall `6.06s`; PASS soundness.
- hot: `1038` mean wall `0.90s`; baseline `938` mean wall `0.88s`; PASS soundness.

Conclusion: this is semantically useful and recovers a small hot-path improvement versus `1037` (`0.92s -> 0.90s`), but still does not beat baseline. The agents' review aligns with the benchmark: the remaining win needs proof-carrying direct exact-hit authority before `loadFullTrace`, plus lazy startup/current-session indexes. Isolated proof tweaks are not enough.

## Benchmark note: runs 1039, 1016, 1040

Baseline remains no-debug/no-stats 10-commit run 938:
- cold mean wall: 6.06s
- hot mean wall: 0.88s
- soundness: PASS

Run 1039 tested the structured-projection source-content fallback in exact-session proof.
- cold mean wall: 6.15s
- hot mean wall: 0.91s
- soundness: PASS
- hot-debug run 1016 showed exact-session proof still missed on all 4 non-empty attempts and `uncovered.structuredProjection` stayed at 4 on every commit.
- Conclusion: the source-content fallback did not cover the observed blocker. It was removed to avoid retaining dead complexity.

Run 1040 tested moving complete-probe preflight before exact-replay descriptor validation.
- cold mean wall: 6.14s
- hot mean wall: 0.90s
- soundness: PASS
- Compared to 1039, this is effectively neutral: cold improved by ~0.01s and hot by ~0.01s, still slower than baseline.
- Conclusion: verifier ordering is not the primary gap. The next useful work needs to remove fixed startup/writeback cost or provide real proof-carrying direct exact-hit authority, not another preflight ordering tweak.

## Implementation note: lazy recovery candidate projections

Change: exact-session current heads remain eager at session setup, but `recoveryCandidatesByAttr` and `recoveryCandidatesByAttrAndSourceIdentity` are now invalidated and rebuilt lazily on the first recovery lookup.

Reasoning: hot exact-session hits should not pay to materialize recovery/history projections they never use. The append-only candidate log (`allCandidateRows` plus `candidateRowsByRecoveryKey`) remains the source of truth; the recovery maps are secondary projections, matching the log/projection split used by LSM-style systems.

Soundness notes:
- Fresh recovery publications update the recovery maps only if those maps are already materialized; otherwise the first lazy materialization scans the append-only candidate rows and includes the fresh rows.
- Session/recovery-key changes clear the projections and reset the loaded flag.
- Ordering is still by durable publication order, newest-first.

Expected impact: small-to-moderate startup win if hot evaluations do not enter recovery. No precision change; recovery paths still see the same candidates once materialized.

## Implementation note: persisted exact-replay descriptors for identity-covered deps

Benchmark run 1041 showed the lazy recovery projection is sound but not sufficient: cold averaged 6.18s vs baseline 938 at 6.06s, and hot averaged 0.91s vs baseline 0.88s. The next bottleneck is hot verification, not raw manifest loading.

Implemented a v6 native manifest row extension that persists exact-replay descriptors for fresh candidate rows only when the proof obligations are covered by immutable exact session/source identity. This deliberately rejects domains that can vary outside that identity, including environment variables, store-path availability, trace-context references, derived store paths, implicit-structure guards, and volatile inputs. Source-content domains require a Git identity obligation in the same proof.

This follows the action-cache/content-addressed-store pattern: descriptor rows are routing/proof metadata, but replay still requires session/source/policy/attr-path/trace/result digest equality and result payload validation before serving. Unsafe or malformed descriptors fail closed to the existing full verification path.

### Benchmark note: persisted exact descriptors rejected for now

Run `1042` tested native-manifest persistence of exact replay descriptors. It was sound, but regressed cold performance from baseline run `938` (`6.06s` mean) to `9.67s` mean and did not improve hot (`0.90s` vs `0.88s`). This confirms the adversarial concern: storing rich replay descriptors moves cost into cold writeback without first proving the hot path consumes them often enough to pay for itself.

Action taken: stop constructing/storing persistent exact descriptors for new rows. Keep the v6 reader/writer tolerant of the experimental field while the next benchmark verifies that the cold regression is removed. If we revisit exact replay, the next design must be lazy/index-first: prove a high exact-hit rate and avoid per-row descriptor serialization on cold writes unless the hot verifier is already bypassing full dependency replay.

### Implementation note: structuredProjection exact-proof fallback

Debug run `hot-debug/1017` showed the hot path still verified `128,143` deps and spent `267.5ms` in verification for 7 hits. Exact-session proof bypassed 3 hits, but 4 hits missed because `StructuredProjection` deps lacked a governing repo id. The corrected fast path now accepts only non-dirset structured projections with an embedded `sourceContentHash`, and only after hashing the current source file bytes to the same value. This keeps the action-cache invariant: replay is authorized by exact current input equality plus the exact semantic session, while avoiding a full structured reparse/verify pass.

## Exact-session DerivedStorePath current-equality coverage

- Debug run 1022 split the previous generic `other` exact-session misses and showed the remaining uncovered domain was `DerivedStorePath=4`.
- This is not soundly covered by Git identity alone: it observes Nix store-path derivation over a coerced source path, not just source bytes.
- Implemented a narrower action-cache proof: during exact-session coverage, recompute each `DerivedStorePath` through the existing current resolver and compare the resulting `DepHashValue` to the recorded dep hash. Missing or mismatched current values fail closed to normal verification.
- Kept `CurrentTraceDep` sealed by adding a verifier-owned helper in `verifier.cc`; `sqlite-trace-storage.cc` does not get a constructor escape hatch.

## Run 1045 after DerivedStorePath exact-session coverage

- Build: `nix build -L .#nix-cli --builders ''` succeeded.
- Debug run 1023 showed the intended correctness signal: exact-session misses dropped to 0, uncovered `DerivedStorePath` dropped to 0, and bypasses rose to 7.
- Normal 10-commit run 1045 versus fixed baseline 938:
  - `hot/1045`: mean wall 0.89s vs `hot/938` 0.88s, soundness PASS. This is a tie within noise, not a win.
  - `cold/1045`: mean wall 6.65s vs `cold/938` 6.06s, soundness PASS. Cold remains materially worse.
- Conclusion: exact-session proof broadening fixed a correctness/coverage gap but is not enough to beat the baseline. Next work should avoid whole classes of hot/cold work rather than adding more per-dep proof tweaks.

## Next architectural focus

- Hot: avoid full-trace/probe/result decode on exact hits where a compact proof authority is sufficient.
- Cold: reduce publication/writeback work; current cold remains slower than baseline even after hot-path misses are gone.
- Keep proof broadening as a correctness cleanup, not the primary performance strategy.

## Failed experiment: large complete-probe cap

- Change tried: increased `kTraceCapsuleProbeMaxDeps` from 4096 to 65536 so exact-session hot hits could bypass full trace capsule inflation.
- Debug signal: hot-debug/1025 showed `fullTraceBypasses=7` and `loadFullTraceUs=0`, so the mechanism worked mechanically.
- Normal 10-commit result: run 1047 regressed badly against baseline 938.
  - cold/1047 mean wall: 6.94s vs cold/938 6.06s.
  - hot/1047 mean wall: 1.26s vs hot/938 0.88s.
  - soundness: PASS.
- Conclusion: larger probes create too much normal-mode overhead. Debug phase counters were misleading here because avoiding full trace inflation did not compensate for bigger probe decode/hash/IO cost. Reverted the cap back to 4096.

## Reverted-probe baseline after failed experiment

- Built after reverting `kTraceCapsuleProbeMaxDeps` back to 4096.
- Normal 10-commit run: 1048.
  - cold/1048 mean wall: 6.63s vs cold/938 6.06s.
  - hot/1048 mean wall: 0.89s vs hot/938 0.88s.
  - soundness: PASS.
- Conclusion: the current implementation is back near the pre-large-probe state. Hot is effectively tied but not a win; cold remains materially behind. The next implementation needs to avoid whole classes of work, not just make exact-session proof coverage complete.

## Conservative V1 exact-hit sidecar export

- Change: re-enabled persistent exact-hit sidecar export only for the existing sound direct-serving subset: scalar exact-session rows with zero observations / empty traces.
- Normal 10-commit run: 1049.
  - hot/1049 mean wall: 0.87s vs hot/938 0.88s and hot/1048 0.89s.
  - cold/1049 mean wall: 6.62s vs cold/938 6.06s and cold/1048 6.63s.
  - soundness: PASS.
- Conclusion: useful but too narrow. It creates a small hot win without cold cost, but it cannot solve the main goal because cold writeback is still ~0.56s slower than baseline and non-empty hot hits still need a V2 exact authority format carrying canonical observation material.

## Rejected: durable raw payload metadata-only publication

- Change tried: skipped shutdown-time `file_size` / `exists` probing and outgoing pack reserve accounting for capsules that were already durable from an earlier generation.
- Normal 10-commit run: 1050.
  - cold/1050 mean wall: 6.63s vs cold/1049 6.62s and cold/938 6.06s.
  - hot/1050 mean wall: 0.87s vs hot/1049 0.87s and hot/938 0.88s.
  - soundness: PASS.
- Conclusion: no measurable benefit. Reverted because it only moved missing durable-object detection from publish time to read time, which is not worth keeping without performance improvement.

## Streaming native payload pack chunks

- Change: replaced construction of a contiguous outgoing `payloadPack` string with a chunked pack plan. The manifest still stores identical range offsets and a digest computed over the same byte sequence; publish writes chunks directly to the pack file.
- Normal 10-commit run: 1051.
  - cold/1051 mean wall: 6.60s vs cold/1049 6.62s and cold/938 6.06s.
  - hot/1051 mean wall: 0.87s vs hot/1049 0.87s and hot/938 0.88s.
  - soundness: PASS.
- Conclusion: small positive/neutral. It removes one contiguous allocation/copy for fresh capsule packs, but the remaining cold gap is still ~0.54s, so the dominant cost is elsewhere.

## 2026-05-22 iteration notes: rejected micro-optimizations

- Run 1052 rejected the no-stats replay/counter fast path plus recovery empty-attr gate: cold/1052 = 6.732110s, hot/1052 = 0.921208s on the fixed 10-commit no-debug/no-stats set. Both regressed relative to current 1051 and baseline 938.
- Run 1053 rejected the session-scoped FileBytes/RawBytes/DirectoryEntries cache experiment: cold/1053 = 6.814330s, hot/1053 = 1.002660s. The extra lookup/cache maintenance cost outweighed any file hash reuse on this workload.
- Run 1054 rejected removing the local partition flatten sort in isolation: cold/1054 = 6.821420s, hot/1054 = 1.063470s. Legacy flatten ordering should stay for legacy vector callers.
- Current experiment: carry `CapturedDeps` (typed partitions plus appended replay deps) through fresh publication and let `Recorder` perform the existing canonical key/value digest sort directly from the carrier. This preserves the action-cache/CAS commitment model and avoids using local intern-id order as authority.
- Run 1055 rejected the full `CapturedDeps` partition-carrier experiment: cold/1055 = 6.800620s, hot/1055 = 1.015610s. The legacy flat vector plus local sort appears to be helping downstream locality/codegen; bypassing it is not a win on this benchmark.

## 2026-05-22 iteration notes: kept copy-reduction optimizations

- Run 1056 kept pointer-sort materialization in `sortAndDedupDeps`: cold/1056 = 6.128370s, hot/1056 = 0.769031s. This is the largest current win: sorting small pointer/key records avoids repeated large `Dep` movement while preserving the canonical digest/key/value ordering.
- Run 1057 kept moving the already-sorted dependency vector into storage publication: cold/1057 = 6.137080s, hot/1057 = 0.765580s. Cold was slightly worse than 1056 but hot improved; the semantic direction is still right because storage owns the vector after publication.
- Run 1058 measured lazy result-cache insertion in `doInternResult`: cold/1058 = 6.133990s, hot/1058 = 0.774627s. The effect is noise/mixed, not a decisive win. It is kept for now because it removes an unnecessary hit-path payload copy without changing identity or replay semantics.
- Run 1059 rejected encoding fresh trace capsules from non-owning views: cold/1059 = 6.144180s, hot/1059 = 0.767943s. The added API/overload complexity did not reduce cold wall time on the fixed benchmark, so the view encoder was reverted.
- Run 1060 rejected the extra publication-time `persistentExactDescriptorSourceAvailable` eligibility check: cold/1060 = 6.141860s, hot/1060 = 0.770349s. The check was semantically safe but moved work onto every fresh publication and did not pay for itself.
- Run 1061 kept only the dirty-index scan cleanup in `snapshotPersistentExactGenerationSourcesLocked`: cold/1061 = 6.124750s, hot/1061 = 0.770298s. This is a small cold win over 1056 and preserves the existing dirty-row authority used elsewhere in writeback.
- Current experiment: specialize empty trace-capsule probe encoding for traces larger than `kTraceCapsuleProbeMaxDeps`. Large traces already emit zero probe rows; the specialized encoder should byte-preserve the sidecar while avoiding generic probe interning/setup.
- Run 1062 kept specialized empty-probe encoding for large trace capsules: cold/1062 = 6.114380s, hot/1062 = 0.769314s. This preserved the same empty probe authority model while removing generic interner/vector setup for large capsules.
- Run 1063 rejected grouped durable payload existence checks: cold/1063 = 6.144100s, hot/1063 = 0.770220s. The extra hash maps in manifest encoding cost more than repeated filesystem existence checks for this workload, so the change was reverted.

## 2026-05-22 run 1064: fd/writeFull payload pack writer rejected

- Experiment: replaced payload-pack `std::ofstream` chunk publication with `openNewFileForWrite` plus `writeFull`.
- Result on fixed 10-commit set: `cold/1064 = 6.142540`, `hot/1064 = 0.775103`.
- Baseline remains `cold/938 = 6.058960`, `hot/938 = 0.880468`; current kept best remains `cold/1062 = 6.114380`, `hot/1062 = 0.769314`.
- Finding: the fd writer regressed both cold and hot relative to `1062`, so the experiment was reverted. The dominant cold gap is not solved by replacing iostream chunk writes.

## 2026-05-22 run 1065: uncompressed capsule payloads rejected

- Experiment: stored capsule payload envelopes as raw uncompressed frames while preserving sidecars and content-addressed envelope hashing.
- Result on fixed 10-commit set: `cold/1065 = 6.147690`, `hot/1065 = 0.763964`.
- Interpretation: hot improved slightly, but cold regressed versus `1062` (`6.114380`). The cold gap is not primarily zstd compression cost for this workload; the larger raw payloads likely increase hashing/write/cache pressure enough to dominate any compression savings.
- Decision: reverted. Keep compressed storage v3.

## 2026-05-22 DirectHash recovery index experiment

Fixed 10-commit baseline remains run `938` (`cold=6.058960`, `hot=0.880468`). Best recent run before the experiment was `1062` (`cold=6.114380`, `hot=0.769314`).

Kept: the node-stamp history fix from run `1066`; it is correctness-relevant because recovered history must carry the candidate row's actual node stamp, not an unrelated latest-row stamp. Performance was worse than `1062` (`cold=6.134250`, `hot=0.772568`), but this should not be traded away for speed.

Rejected and reverted: the extra `AttrPathId -> TraceHash -> RecoveryCandidateRecord[]` recovery index measured in run `1067` (`cold=6.141310`, `hot=0.770144`). It made cold slower than `1066`, and it did not beat the best recent hot result. The added map also complicated trace-hash patching semantics, so it is not worth keeping.

Next implication: DirectHash candidate scanning is not the dominant loss. The remaining gap against baseline is still mostly partial-hit/lazy exact-hit recovery quality and verification materialization cost, not a missing hash bucket index.

## 2026-05-22 Persistent exact source eligibility tightening

Run `1068` after narrowing `persistentExactDescriptorSourceAvailable` to v1-eligible publication inputs: empty canonical deps plus direct scalar result payload present in memory.

Results on fixed 10-commit set:

- baseline pre-schema `938`: `cold=6.058960`, `hot=0.880468`
- best prior current `1062`: `cold=6.114380`, `hot=0.769314`
- post-node-stamp `1066`: `cold=6.134250`, `hot=0.772568`
- new `1068`: `cold=6.122340`, `hot=0.769158`

Decision: keep. This preserves semantics because v1 serving authority already rejects non-empty traces and unsupported result kinds later; the flag now avoids marking rows as possible descriptor sources when they can only fail closed. This recovers most of the node-stamp cold regression and gives the best hot result so far, but cold still trails the fixed baseline by about 63 ms.

Next implication: the remaining cold gap is unlikely to be descriptor snapshot scanning. Focus on history bootstrap, full-trace/capsule materialization, and exact-hit/recovery paths.

## 2026-05-22 Recovery material direction

The latest adversarial review argues that the remaining cold gap needs a recovery-material sidecar, not another SQLite/index micro-optimization. The core issue is that recovery/SV still often inflates full capsules or full dep vectors before it knows whether a candidate is useful.

Proposed architecture to investigate next:

- Add a range-readable `RecoveryMaterial` object to the generation format.
- Bind it to `schemaEpoch`, `providerEpoch`, hash algorithm, `fullTraceHash`, `traceHash`, `depSetHash`, `resultHash`, and the capsule/payload digest.
- Store canonical key material for trace-hash-contributing deps and value material only where guard/reidentity validation needs it.
- Keep startup lazy: load only manifest/index/ref metadata, not recovery sidecar bytes.
- Use it before `loadFullTrace` in GitIdentity, DirectHash acceptance, SV representative selection, and recovered-trace acceptance.
- Reject TraceContext, volatile, unsupported domains, and incomplete material initially; sidecar absence/corruption must fall back to the full capsule path.

Soundness invariant: manifest rows and candidate indexes are routing only. A sidecar can avoid full capsule decode only if its bytes and semantic digest validate against immutable trace/result commitments and it contains all material needed by the acceptance rule.

## 2026-05-22 Complete-probe recovery integration experiment

Run `1069` integrated `loadHashValidatedCompleteProbeDeps` into GitIdentity, DirectHash acceptance, and SV representative/candidate paths.

Results:

- `1068`: `cold=6.122340`, `hot=0.769158`
- `1069`: `cold=6.158250`, `hot=0.772736`

Decision: reject and revert the verifier integration. The likely cause is that validated-probe recovery adds extra lookup/sort/hash validation on paths where the full deps are already cached or where the complete probe is not selective enough. This reinforces that a real recovery-material sidecar must be keyed and shaped for recovery, not retrofitted from bounded capsule probes.

Kept for isolated measurement: the storage helper and exact-session preflight use may still be useful, but it should be measured independently after reverting the broad recovery integration.

## 2026-05-22 Complete-probe rollback result

Fixed 10-commit comparison set, baseline run `938` from `92a3df1ab706cf514357ae57b367050b373ff146`:

- `cold/938`: 6.058960s mean
- `hot/938`: 0.880468s mean
- accepted narrow exact-source fix `cold/1068`: 6.122340s mean
- accepted narrow exact-source fix `hot/1068`: 0.769158s mean
- restored complete-probe preflight cache `cold/1072`: 6.161780s mean
- restored complete-probe preflight cache `hot/1072`: 0.770329s mean

Conclusion: complete-probe recovery/preflight work is not recovering the cold regression. It is at best neutral for hot and consistently worse for cold than the narrow exact-source eligibility fix. Do not continue broadening the complete-probe verifier path until a more selective authority sidecar exists.

Next implementation focus: reduce cold fresh-record/writeback costs and/or add explicit recovery authority material. Low-risk candidates from review are dirty-only export reserve/iteration, avoiding eager full capsule copies, streaming payload-pack hashing, and a recovery material sidecar that can avoid full capsule decode without losing soundness.

## 2026-05-22 Recovery material sidecar implementation slice

Rationale: fixed 10-commit run `1073` showed the non-owning capsule encoder cleanup regressed cold (`6.187600s`) and did not improve hot materially (`0.769627s`). That patch was reverted. Both follow-up review agents converged on the same next target: cold remains dominated by recovery/materialization paths that inflate or decode full trace capsules before a candidate has enough proof material to be useful.

Implementation direction now in progress:

- Add `TraceRecoveryMaterial`, a projection sidecar containing the complete sorted dependency vector plus the trace/result hash headers.
- Treat the sidecar as a covering-index projection, not authority. `loadRecoveryMaterial` validates byte hash, sidecar headers, and recomputes `fullHash`, `traceHash`, and `depSetHash` before installing `traceCache.full`.
- Publish sidecars at shutdown from already-canonical in-memory deps for SQLite and native generation writeback.
- Keep startup lazy for native generations by storing recovery material as an immutable pack/object locator in the manifest. SQLite fallback bulk-loads sidecar bytes into memory, matching the existing SQLite disconnected snapshot model.
- Existing verifier call sites benefit through `loadFullTrace(..., backfillResultPayload=false)` trying validated recovery material before falling back to full capsule decode. Sidecar absence/corruption cannot become a miss; it falls back.

Adversarial constraints kept:

- No local intern ids are authority.
- The sidecar is bound to schema epoch, provider/hash algorithm, trace tuple, result hash, and payload byte hash.
- Unsupported/corrupt/missing sidecars fail closed to full capsule decode.
- No broad verifier semantics changed in this slice.

## Reverted experiment: broad recovery-material sidecars

- Attempted a separate recovery-material sidecar for every `(full_hash, result_hash)` capsule to avoid full capsule decode on recovery-oriented loads.
- Reverted before keeping it: it duplicated the full dependency projection, required a manifest/schema ABI bump, added eager SQLite/native publication work, and caused severe cold-eval regression in run 1075 before completion.
- Adversarial review found the design was also semantically incomplete: a recovery dep-vector sidecar did not provide result-sidecar replay, optional sidecars became a new failure surface, and the first implementation had decoder/coverage hazards.
- Current conclusion: do not publish broad full-dep sidecars. If sidecars return, they should be narrow, exact-hit/result-serving oriented, optional, fail-closed, and benchmarked without invalidating the fixed baseline identity.

## Benchmark after reverting recovery-material sidecars

10-commit no-debug/no-stats run after removing the broad recovery-material sidecar experiment:

- Fixed baseline `938`: cold `6.058960s`, hot `0.880468s`.
- Prior accepted narrow exact-source run `1068`: cold `6.122340s`, hot `0.769158s`.
- Current run `1076`: cold `6.357950s`, hot `0.768316s`.

Interpretation:

- Soundness: `eval-trace-bench runs` reports all outputs match.
- Hot path remains better than baseline and essentially tied with `1068`.
- Cold path is still not acceptable: current is ~4.9% slower than fixed baseline `938` and ~3.9% slower than `1068`.
- The slow cold commits are the record/writeback-heavy cases (~11s), while replay-heavy cases remain ~0.9s. Next work should focus on shutdown publication/dirty-generation work, not hot lookup.

## 2026-05-22: Recovery candidate routing index

Implemented an append-friendly recovery candidate projection:

- Recovery candidate vectors are now maintained oldest-first internally so fresh append-only publications avoid `insert(begin)` churn.
- Public recovery readers iterate vectors in reverse to preserve newest-first candidate semantics.
- Added `recoveryCandidatesByAttrAndTraceHash` so direct trace-hash recovery performs a keyed lookup instead of scanning every candidate for an attr.
- The trace-hash index remains non-authoritative routing metadata; verifier acceptance still depends on existing capsule/proof/result validation.

Expected impact: improves partial-cold/direct-recovery overhead in workloads where an attr has accumulated multiple recovery candidates. It will not fix cold misses where no candidate can be proven reusable.

## 2026-05-22: Reverted recovery trace-hash index experiment

10-commit benchmark `1077` after the recovery trace-hash index and oldest-first append buckets regressed against both `1076` and the fixed baseline:

- `cold/1077`: 6.500840s mean
- `hot/1077`: 0.776781s mean
- baseline `cold/938`: 6.058960s, `hot/938`: 0.880468s
- prior current `cold/1076`: 6.357950s, `hot/1076`: 0.768316s

Decision: remove the experiment. The workload's cold gap is not dominated by per-attr direct-hash scan overhead, and the additional projection/index maintenance was not justified.

## 2026-05-22: SV mismatch telemetry gating

Implemented cold-path cleanup in structural-variant recovery:

- Historical hash mismatch checks are now paid only when `eval-trace-structural-recovery-mismatch-telemetry` is enabled.
- Complete-probe mismatch index collection is also gated behind that setting.
- Removed a dead `repHashMismatch` early-reject branch. Structural-variant recovery still computes candidate hashes from current dep values and accepts only through the existing trace/result authority path.

Expected impact: small cold-path reduction on SV fallback candidates. This is diagnostic-only work, so soundness and precision should be unchanged.

## 2026-05-22: Reverted SV mismatch telemetry gating experiment

10-commit benchmark `1078` after gating structural-variant mismatch telemetry did not improve:

- `cold/1078`: 6.479530s mean
- `hot/1078`: 0.773097s mean
- prior current `cold/1076`: 6.357950s, `hot/1076`: 0.768316s

Decision: remove the experiment. Even though the guarded work is diagnostic, benchmark data does not justify carrying this patch. Move to larger publication/recording overheads or architecture-level cache reuse.

## 2026-05-22: Fused complete-probe capsule encoding

Implemented a fresh-recording write-path optimization:

- Complete trace capsule probes are now encoded from the already encoded full-capsule dependency rows.
- The probe is captured before dir-set member encoding appends extra local strings, preserving the compact probe shape.
- The full capsule remains the authoritative object; the probe remains a non-authoritative routing/covering projection bound back to immutable capsule hashes.

Expected impact: reduces duplicate dependency interning/encoding during fresh publication for traces under the complete-probe cap. This targets cold recording overhead, not candidate acceptance semantics.

## 2026-05-22: Reverted fused complete-probe encoding experiment

Benchmarks after the fused-probe encoder were worse:

- `cold/1079`: 6.464130s, `hot/1079`: 0.782816s
- repeat `cold/1080`: 6.479080s, `hot/1080`: 0.851334s
- prior current `cold/1076`: 6.357950s, `hot/1076`: 0.768316s

Decision: revert. Although the refactor was intended to preserve full-capsule authority and reuse encoded rows, it did not improve the measured workload. Do not pursue local probe-encoding micro-optimizations without finer profiling showing this path dominates.

## 2026-05-22: SV same-keyset prune restored
Moved the `e.keySetHash == oldHeader->keySetHash` skip before candidate probe loading in structural-variant recovery. This restores the baseline ordering: same-keyset candidates are direct-hash territory, so SV can prune them before sidecar/probe work. Acceptance should be unchanged; only candidate work is skipped.

## 2026-05-22: SV same-keyset prune result
Run `1083` after moving same-keyset pruning before probe loading: cold `6.574000`, hot `0.878163` on fixed 10 commits. This was worse than baseline `938` (`cold 6.058960`, `hot 0.880468`) and worse than current accepted `1076` (`cold 6.357950`, `hot 0.768316`). Reverted the source change. Interpretation: the skip is semantically safe, but this ordering did not improve the measured workload; the main cold deficit is still full replay/recovery work on heavy partial-hit commits.

## 2026-05-22: Compact cached-hit replay experiment
Changed cached-hit `TracedExpr::replayTrace` to prefer a single `TraceValueContext(pathId -> current trace_hash)` dependency instead of loading and appending the cached trace's full dependency vector. This targets the measured cold partial-hit deficit: parent traces should depend on verified child trace identity rather than flattening every child input observation. If the current trace hash cannot be resolved, the old `loadFullTrace` replay remains as fail-closed fallback. This is an action-cache / self-adjusting-computation graph-edge model; verification already resolves TraceValueContext through the current trace graph before accepting a dependent trace.

## 2026-05-22: Compact cached-hit replay result
Run `1084` with compact `TraceValueContext` replay: cold `6.577430`, hot `0.969020` on fixed 10 commits. This regressed both current accepted `1076` (`cold 6.357950`, `hot 0.768316`) and baseline `938` (`cold 6.058960`, `hot 0.880468`). Reverted the source change. Soundness review agreed the shape is only acceptable when using existing TraceContext semantics, which the experiment did, but the benchmark shows the bottleneck is not replay publication; it is earlier full trace inflation for recovery/key eligibility. Next direction: restore baseline-like key-only/probe-first verification and recovery paths.

## 2026-05-22: Validated complete-probe recovery projection
Implemented the first key-only/probe-first step: recovery acceptance and GitIdentity-indexed recovery now try a complete `TraceCapsuleProbe` before full capsule decode. The probe is accepted only when it covers all deps and recomputes to the trace row's authoritative `(trace_hash, dep_key_set_hash, full_hash)`. This preserves the CAS/covering-index split: probe bytes can cover the read, but only after rebinding to immutable object hashes. Expected benefit is on cold partial-hit commits where current code was full-decoding candidate capsules just to project keys or validate a small set of implicit guards.

## 2026-05-22: Validated complete-probe recovery projection result
Run `1085` with complete-probe recovery projection: cold `6.815480`, hot `0.860784`. This regressed baseline `938` (`cold 6.058960`, `hot 0.880468`) and current accepted `1076` (`cold 6.357950`, `hot 0.768316`). Reverted the source change. Interpretation: validating probe authority by recomputing `(trace_hash, dep_key_set_hash, full_hash)` per candidate is too expensive and/or probes are not covering enough of the heavy candidates. A useful key-only path needs a cheaper durable key-set projection keyed by `DepKeySetHash`, not per-candidate complete-probe hash recomputation.

## 2026-05-22: Complete-probe key-set-only projection experiment
After the full tuple probe validation regressed, narrowed the projection to key-set-only use. Recovery now tries a complete probe only to reconstruct/sort keys and verify `DepKeySetHash`; it may then skip full capsule decode only when no implicit structural guards exist. If guards exist, or if the probe/key-set validation fails, it falls back to `loadFullTrace`. This deliberately avoids using probe dep values as replay authority.

## 2026-05-22: Complete-probe key-set-only projection result
Run `1086` with key-set-only complete-probe projection: cold `6.672340`, hot `0.880989`. This regressed current accepted `1076` (`cold 6.357950`, `hot 0.768316`) and remained worse than baseline `938` (`cold 6.058960`, `hot 0.880468`). Reverted the source change. Interpretation: even key-only probe sorting/hash validation adds more overhead than it saves on this workload. A durable key-set projection would need schema/manifest/codec/writeback work, but the agent review warned its upside is narrow because probes already cover many traces. Do not pursue the large durable key-set design until profiling shows full capsule decode/key projection dominates.

## 2026-05-22 record-path instrumentation and raw-capsule experiment

- Added `NIX_SHOW_STATS`-gated record-path counters for sort, hash tuple, result encode/hash/intern, result backfill, publish-existing, trace lookup/cache install, capsule encode/hash, and state-change publication.
- Debug run `cold-debug/1032`, commit `3f7b5d89ca3e`: `record.timeUs=422174`, `sortUs=113521`, `hashUs=126024`, `traceCapsuleEncodeUs=179697`, `traceCapsuleHashUs=848`; result encode/hash/intern and DB/git writeback were negligible. This shows the remaining cold regression is canonical dep ordering plus full capsule serialization, not SQLite writeback.
- Tried a breaking v4 uncompressed trace-capsule full frame to remove zstd publication CPU. Debug run `cold-debug/1033` reduced `traceCapsuleEncodeUs` to `101469`, but expanded payload bytes from ~6.36 MiB to ~18.0 MiB and worsened verification/read costs.
- Non-debug 10-commit run `1087` confirmed raw capsules were worse: cold avg `6.6685s` vs accepted `1076` `6.3580s`; hot avg `0.9357s` vs accepted `0.7683s`. Reverted the raw-capsule format change. Keep focus on reducing canonical sort/hash/serialization work without inflating read-side bytes.

## 2026-05-22 capsule interner reserve experiment

- Tried reserving capsule interner string/data-path vectors and maps, plus dir-set digest set, before encoding large trace capsules.
- Debug run `cold-debug/1034`, commit `3f7b5d89ca3e`, regressed: `traceCapsuleEncodeUs` rose from `179697` (`1032`) to `274031`, and `record.timeUs` rose from `422174` to `515837`.
- Reverted. Allocation/rehash churn is not the obvious source of capsule serialization cost; naive over-reservation increases memory/cache pressure.

## 2026-05-22 zstd parallelism experiment

- Tried changing trace capsule full-frame compression from zstd level 1 with `parallel=true` to `parallel=false`.
- Debug run `cold-debug/1035`, commit `3f7b5d89ca3e`, only reduced `traceCapsuleEncodeUs` from `179697` to `169360` and had worse/noisy verify/decode counters.
- Non-debug 10-commit run `1088` regressed: cold first-10 avg `6.7279s`, hot first-10 avg `0.9326s`; accepted run `1076` was cold `6.3580s`, hot `0.7683s`.
- Reverted. Parallel zstd is not the bottleneck worth changing; preserving compact payloads matters for read/hot performance.

## 2026-05-22: dep-order value-digest removal experiment

Attempted to remove the dedicated value digest from canonical dependency ordering and sort by the canonical `DepHashValue` directly after the key digest. The adversarial premise was that `DepHashValue` is already canonical observed-value material, so hashing it only for ordering might be redundant.

Result: rejected and reverted. Debug counters on the large second commit improved record time (~422 ms -> ~342 ms), sort time (~114 ms -> ~86 ms), and capsule encode time (~180 ms -> ~127 ms), but the normal no-debug 10-commit benchmark regressed:

- baseline `cold/938` first-10 avg: ~6.059 s
- accepted current `cold/1076` first-10 avg: ~6.358 s
- experiment `cold/1089` first-10 avg: ~6.875 s
- baseline `hot/938` first-10 avg: ~0.880 s
- accepted current `hot/1076` first-10 avg: ~0.768 s
- experiment `hot/1089` first-10 avg: ~0.960 s

Conclusion: do not trust isolated debug-counter improvements here. Keep experiments only if they improve no-debug wall time on the fixed 10-commit subset.

## 2026-05-22: view-based capsule encode slice

Implemented a narrow record-path optimization: `encodeTraceCapsule` now has a non-owning `TraceCapsuleEncodeInput` that takes the encoded result by reference and deps by `std::span<const Dep>`. Updated fresh publication and shutdown/generation re-encode paths to use the view input instead of constructing a temporary `TraceCapsuleRecord` with a copied dependency vector. Also moved the sorted dep vector in `recordCurrentTraceWithExistingResult` when publishing reidentified traces.

Rationale: debug counters show cold record overhead is dominated by capsule encode/publication. This slice preserves the existing capsule wire format, hashes, result binding, and verification semantics; it only removes avoidable temporary object copies.

Next benchmark should compare against baseline `cold/938` first-10 avg ~6.059s / `hot/938` ~0.880s and accepted current `cold/1076` ~6.358s / `hot/1076` ~0.768s. If the no-debug 10-commit run does not improve materially, the next higher-impact target is duplicate complete-probe encoding inside `encodeTraceCapsule` or the larger exact-hit certificate path.

## 2026-05-22: view-based capsule encode benchmark

Run `1091` after the view-based capsule encode slice:

- `cold/1091` first-10 avg: ~6.782s, min ~1.109s, max ~14.913s
- `hot/1091` first-10 avg: ~0.970s, min ~0.933s, max ~1.000s

Comparison:

- baseline `cold/938`: ~6.059s; `hot/938`: ~0.880s
- accepted current `cold/1076`: ~6.358s; `hot/1076`: ~0.768s
- prior local `cold/1090`: ~6.893s; `hot/1090`: ~0.994s

Conclusion: keep as a small positive cleanup for now, but it is not enough. The next target must remove real duplicated work. The strongest local target is duplicate complete-probe encoding inside `encodeTraceCapsule`: current encoding builds a full dep encoding and then re-encodes the same deps into the probe sidecar. The larger architectural target remains exact-hit certificates as the serving authority.

## 2026-05-22: reuse encoded rows for complete probe encoding

Implemented the next record-path slice: complete trace probes now reuse the `EncodedDepRow` vector and capsule-local dictionary already built for the full capsule, instead of re-running dep-key encoding with a second `CapsuleInterner`. The encoder still emits the same probe wire format and falls back to the old independent probe encoder if the reused dictionary would violate probe limits. Large traces still emit the canonical empty probe.

Rationale: this removes duplicated CPU in `encodeTraceCapsule` while preserving the existing proof model. Complete probes remain projections that must rehash to the stored trace tuple before exact-session preflight can accept them.

## 2026-05-22: duplicate-probe reuse benchmark

Run `1092` after reusing full-capsule encoded dep rows for complete probe encoding:

- `cold/1092` first-10 avg: ~6.606s, min ~1.015s, max ~14.597s
- `hot/1092` first-10 avg: ~0.882s, min ~0.851s, max ~0.906s

Comparison:

- baseline `cold/938`: ~6.059s; `hot/938`: ~0.880s
- accepted current `cold/1076`: ~6.358s; `hot/1076`: ~0.768s
- local pre-slice `cold/1091`: ~6.782s; `hot/1091`: ~0.970s

Conclusion: keep this slice. It materially improves hot and cold by removing a real duplicate dep-encoding pass while preserving existing proof semantics. Cold remains ~0.55s slower than baseline first-10, so remaining local work should attack full capsule raw bytes/compression/result duplication or broaden exact-hit direct serving.

## 2026-05-22: result-sidecar-only raw capsule experiment

Tried changing the raw capsule format from complete trace payload (deps + result) to dependency-only raw payload plus a mandatory result sidecar in the same stored envelope. This was intended to reduce cold writeback by avoiding duplicate result serialization/compression. Build succeeded, but 10-commit no-debug benchmark regressed:

- baseline `cold/938` first10 avg: 6.058940530301333s
- prior current `cold/1092` first10 avg: 6.6063053411024155s
- experiment `cold/1093` first10 avg: 6.710610658407677s
- baseline `hot/938` first10 avg: 0.8804682381000021s
- prior current `hot/1092` first10 avg: 0.8824343548039906s
- experiment `hot/1093` first10 avg: 0.9808507933892543s

Adversarial review: the change is semantically viable only if the envelope, not raw capsule, is the integrity unit; all readers must reject missing/mismatched sidecars and result sidecars must remain hash-checked. However, the benchmark shows the extra sidecar-first/full-reconstruction path and loss of raw self-containment do not buy performance in the current architecture. Reverted this slice and kept the duplicate-probe reuse state as the best current point.

## 2026-05-22: cold-path micro-optimization results

Retained:

- Avoid copying already-owned capsule payloads into a temporary `std::string` in SQLite writeback staging. Build passed. 10-commit run `1094` improved cold slightly versus `1092` but likely within noise and did not help hot:
  - `cold/1092` first10 avg: 6.6063053411024155s
  - `cold/1094` first10 avg: 6.563302820897661s
  - `hot/1092` first10 avg: 0.8824343548039906s
  - `hot/1094` first10 avg: 0.9435676975001115s

Reverted:

- Dir-set encoder vector/sorted-definition shortcut. It was semantically low-risk but benchmarked worse:
  - `cold/1095` first10 avg: 6.795242495296407s
  - `hot/1095` first10 avg: 0.9850029098015511s

Current benchmark status against fixed baseline:

- Fixed pre-schema baseline `cold/938` first10 avg: 6.058940530301333s
- Fixed pre-schema baseline `hot/938` first10 avg: 0.8804682381000021s
- Best current after retained micro-slices is approximately `cold/1094` at 6.563302820897661s and `hot/1092` at 0.8824343548039906s, noting run-to-run noise.

Adversarial conclusion: micro-optimizing capsule encode/staging is not enough. Subagent review and debug counters both point to the same root cause: current cold path constructs/compresses full trace capsules as serving authority, adding substantial work over the old normalized SQLite implementation. To beat the baseline rather than chase it, the next implementation direction should make hot hits proof-carrying and lazy: richer current-node heads, exact-hit certificates/cursors, and container shape cursors that avoid full trace decode/replay for exact hits.

## 2026-05-22: Hot-head fast-path experiment result

- Built after removing the failed `CurrentNodeHead` routing map and restoring complete-probe preflight before exact replay.
- Benchmark run `1097` (10 commits, no stats/debug): cold `6.77390825029579s`, hot `0.9863010189001216s`.
- Baseline run `938`: cold `6.058940530301333s`, hot `0.8804682381000021s`.
- Conclusion: the attempted exact-descriptor-before-complete-probe fast path was wrong; it put expensive replay/descriptor work ahead of the existing cheaper hot preflight. The remaining copy-reduction changes are not sufficient to matter and may be within noise/regression. Next viable path is architectural: avoid producing and validating full trace capsules on the hot/cold path unless the observation actually needs them.

## 2026-05-22: Raw trace-capsule storage envelope experiment

- Change: new trace-capsule storage format version `4` stores the authoritative full capsule raw in the storage envelope instead of zstd-compressing it. Probe and result sidecars remain framed in front. Older `2`/`3` decode is still accepted during this experiment.
- Rationale: cold profiling pointed at capsule construction/compression; zstd was pure cache artifact overhead, not part of semantic trace identity. Soundness is retained because `full_hash`, `trace_hash`, `dep_set_hash`, and `result_hash` are still inside the capsule, and `payload_hash` still hashes the entire envelope bytes.
- Benchmark run `1098` (10 commits, no stats/debug): cold `6.577966729394393s`, hot `0.9185003419028362s`.
- Comparison: this improves over `1097` cold `6.77390825029579s`, hot `0.9863010189001216s`, but still misses baseline `938` cold `6.058940530301333s`, hot `0.8804682381000021s`.
- Adversarial note: raw envelopes increase cache artifact size and may hurt large-cache load/writeback if I/O becomes dominant. Keep only if subsequent 100-commit runs confirm wall-time improvement and size is acceptable.

## 2026-05-22: Deferred fresh capsule materialization experiment failed

- Change tested in run `1099`: removed record-time population of `encodedTraceCapsulePayloadsByPair` and relied on shutdown/generation fallback materialization.
- Result: hot improved to `0.8866419319994747s`, but cold collapsed to `12.224707770903478s` average on the fixed 10 commits. The previous partial/cross-commit cold hits disappeared.
- Conclusion: eager fresh capsule availability currently feeds the next-generation/cross-commit reuse path. Deferring it naively is not semantics-equivalent for benchmark behavior, even though the raw material exists in memory. Reverted this part only.
- Follow-up if revisiting: implement a single authoritative `materializeTraceCapsulePayloadLocked()` and prove every dirty/current candidate that can be used by the next commit is published. Treat missing materialization for dirty rows as an error, not a silent skip.

## 2026-05-22: verifier single-call collapse experiment

- Runs `1099` and `1100` showed that collapsing verifier work into one coarse storage call is not viable: hot stayed near baseline, but cold collapsed to roughly `12.2s` average over the fixed first 10 commits because the verifier held the storage lock across too much trace verification/recovery work.
- Restored the split verifier shape and rebuilt. Run `1101` recovered the mixed cold profile (`6.496633s` average) and hot (`0.890530s` average), but still misses baseline run `938` (`6.058941s` cold, `0.880468s` hot).
- Conclusion: keep fine-grained async/interleaved verifier storage calls. Do not retry coarse single-call verification unless the storage lock and verification work are fully separated.

## 2026-05-22: dir-set capsule encode simplification experiment

- Tried removing per-capsule dir-set member re-sorting and replacing the transient `unordered_flat_set` of dir-set digest ids with a `vector` + `sort/unique`.
- Result: run `1102` regressed versus `1101` (`cold 6.579810s` vs `6.496633s`, `hot 0.912588s` vs `0.890530s`) and remained behind baseline `938` (`cold 6.058941s`, `hot 0.880468s`).
- Reverted the patch. The likely explanation is that dir-set encode work is not material in this benchmark, and/or the changed member ordering disturbed locality/artifact behavior without reducing enough CPU.

## 2026-05-22: persistent exact-generation export eligibility guard

- Tried skipping exact-generation sidecar export when no dirty/all candidate row had an obvious descriptor source candidate (`persistentExactDescriptors` or exact-session descriptor-source availability).
- Result: run `1103` regressed versus `1101` (`cold 6.533451s` vs `6.496633s`, `hot 0.919098s` vs `0.890530s`) and remained behind baseline `938` (`cold 6.058941s`, `hot 0.880468s`).
- Reverted the patch. The optional exact-generation export path is not the dominant measured cost, or the guard did not skip anything meaningful on these runs.

## 2026-05-22: current-node fast-path fusion experiment

- Tried fusing current-node lookup, already-verified decode, exact-session preflight, complete-probe preflight, and exact-replay descriptor acceptance into one exclusive storage call while keeping full trace verification/recovery outside the fused call.
- Build initially failed because verifier-only shadow-audit types are local to verifier.cc; moved shadow back out and rebuilt successfully.
- Result: run `1104` regressed versus `1101` (`cold 6.541573s` vs `6.496633s`, `hot 0.912550s` vs `0.890530s`) and remained behind baseline `938` (`cold 6.058941s`, `hot 0.880468s`).
- Reverted the patch. Narrow coroutine/exclusive-hop fusion is not the measured bottleneck, and the extra coupling/inlining pressure is not justified.

## Finding: complete-probe cap reduction failed

Experiment: lowered `kTraceCapsuleProbeMaxDeps` from 4096 to 1024 to reduce stored preflight payload size. This is sound because incomplete probes fail closed to full verification, but it trades hit precision and hot-path short-circuiting for a smaller cold artifact.

Result: run 1105 regressed against retained run 1101. Cold first-10 average moved from 6.4966s to 6.5251s, and hot moved from 0.8905s to 0.9282s. Baseline run 938 remains 6.0589s cold and 0.8805s hot.

Decision: reverted the cap to 4096. The data says this knob is not a path to beating the baseline; it saves too little cold writeback/IO and loses too much preflight usefulness.

## Finding: sidecar-only result capsule vNext failed

Experiment: changed the trace capsule storage format so result bytes were stored only in the envelope result sidecar, not duplicated inside the dependency capsule body. The design was sound: the dependency frame and result frame were both bound to the same `(full_hash, trace_hash, dep_set_hash, result_hash)` tuple, and mismatches failed closed.

Result: run 1106 regressed against retained run 1101. Cold first-10 average moved from 6.4966s to 6.7115s, and hot moved from 0.8905s to 0.9611s. Baseline run 938 remains 6.0589s cold and 0.8805s hot.

Decision: reverted. Removing duplicate result bytes did not move the bottleneck in the right direction, and the decode-path reshaping introduced enough overhead to hurt hot replay. A future version should not revisit this as a broad `decodeTraceCapsule()` composition change; if we remove duplication later, it needs a zero-extra-parse fast path and direct sidecar validation in existing hot paths.

## 2026-05-22: direct-serving exact-session preflight experiment reverted

Benchmarked gated direct-serving preflight as run 1108 against baseline run 938 and retained current run 1101 using 10 commits, no debug/stats.

Results, first 10 commits:
- baseline 938: cold avg 6.058940530301333s, hot avg 0.8804682381000021s
- retained current 1101: cold avg 6.496633113897405s, hot avg 0.890530034294352s
- broad direct-serving 1107: cold avg 6.463323697005398s, hot avg 0.9267654057970504s
- gated direct-serving 1108: cold avg 6.4782706011028495s, hot avg 0.9217383008974138s

Decision: reverted. The gated version still regressed hot time and did not beat retained current cold time. The failure mode is structural: descriptor validation plus candidate lookup is too expensive to add ahead of the existing capsule decode unless direct payload eligibility is both common and cheaply known. A future version would need eligibility encoded in a cheaper per-current-node bit/index and should avoid rebuilding descriptor authority on the hot path.

## 2026-05-22: view-based result decode experiment reverted

Implemented a borrowed `ResultPayloadView` decode path plus allocation-free string-context token iteration, then built and benchmarked as run 1109 using 10 commits, no debug/stats.

Results, first 10 commits:
- baseline 938: cold avg 6.058940530301333s, hot avg 0.8804682381000021s
- retained current 1101: cold avg 6.496633113897405s, hot avg 0.890530034294352s
- borrowed decode 1109: cold avg 6.504826942409272s, hot avg 0.8954735357983736s

Decision: reverted. Avoiding the `ResultPayload` copy is not the bottleneck for this benchmark; noise/extra code shape moved hot and cold slightly worse. Do not revisit unless perf shows result materialization copying as dominant.

## 2026-05-22: native manifest v7 dictionaries kept for refinement

Implemented a native manifest v7 with adaptive segment-local dictionaries for repeated candidate/session hashes and runtime-root source strings. This is wire-only compression: decoded rows still store full semantic values, and refs are never durable identity.

Results, first 10 commits, no debug/stats:
- baseline 938: cold avg 6.058940530301333s, hot avg 0.8804682381000021s
- retained current 1101: cold avg 6.496633113897405s, hot avg 0.890530034294352s
- manifest dictionaries 1110: cold avg 6.427018363401293s, hot avg 0.8445429999032058s

Decision: keep for now. It improves cold by ~70ms versus retained current and makes hot faster than both retained current and baseline. It does not satisfy the goal because cold remains ~368ms slower than baseline. Next refinement should target remaining cold bytes/work rather than hot replay.

## 2026-05-22: native manifest compact trace/result refs

Refined v7 manifests after adversarial review: dirty segments now encode compact per-segment trace/result refs instead of process-global ids, and the decoder rejects zero/out-of-range refs rather than resizing sparse vectors. Also removed writeback-time `exists()` checks for already-durable payload locators; payload objects remain content-verified when consumed by `loadFullTrace()`.

Results, first 10 commits, no debug/stats:
- baseline 938: cold avg 6.058940530301333s, hot avg 0.8804682381000021s
- retained current 1101: cold avg 6.496633113897405s, hot avg 0.890530034294352s
- v7 dictionaries 1110: cold avg 6.427018363401293s, hot avg 0.8445429999032058s
- compact refs + durable-exists skip 1111: cold avg 6.425468193599954s, hot avg 0.8471965091972379s

Decision: keep. This fixes a real sparse-id fail-closed/performance issue and preserves the v7 performance gain. Remaining blocker from review: physical native segment boundaries are still erased by concatenating `.etm` files before decode; segment-local dictionaries should be decoded per physical segment or manifest records should become length-framed.

## 2026-05-22: native v7 segment-boundary benchmark

- Built successfully with per-file native hot-manifest loading: native `.etm` files are decoded as physical segments instead of one concatenated byte stream.
- Fast 10-commit run: `1112`.
- Fixed no-debug/no-stats baseline remains run `938` from `92a3df1ab706cf514357ae57b367050b373ff146`.
- First-10 comparison:
  - `cold/938`: avg 6.058940530301333s, min 0.8796358700055862s, max 15.818140367999149s.
  - `cold/1111`: avg 6.425468193599954s, min 0.9542233100219164s, max 14.277208785992116s.
  - `cold/1112`: avg 6.427873765697586s, min 0.9576581380097196s, max 14.327818830002798s.
  - `hot/938`: avg 0.8804682381000021s, min 0.8698865059996024s, max 0.8934190700019826s.
  - `hot/1111`: avg 0.8471965091972379s, min 0.8159494419815019s, max 0.875968501000898s.
  - `hot/1112`: avg 0.8449245182011509s, min 0.8072244719951414s, max 0.9118395840050653s.
- Decision: keep the segment-boundary hardening. It fixes the fail-closed invariant with no material cold regression and a small hot improvement.
- Remaining gap: cold is still about 0.369s slower than the fixed baseline. The next meaningful targets are bounded native replay via snapshot/compaction and/or reducing trace capsule/envelope bytes, not more v7 dictionary tuning.

## 2026-05-22: adaptive complete-probe cap experiment

- Experiment: cap complete trace-capsule probe payloads at 64 KiB and emit the canonical empty probe above that size. This is sound because empty probes are reject/filter metadata and force full-capsule verification.
- Fast 10-commit run: `1113`.
- Result:
  - `cold/1113`: avg 6.43078560649883s, worse than `cold/1112` avg 6.427873765697586s.
  - `hot/1113`: avg 0.8615222245978658s, worse than `hot/1112` avg 0.8449245182011509s.
- Decision: reverted. The cap removes useful hot preflight metadata and does not reduce long cold eval/writeback enough to matter.
- Implication: complete probes are not the dominant cold regression in this 10-commit workload, or the probe encoding/write bytes saved by a 64 KiB cap are below noise compared with other writeback/evaluation costs.

## 2026-05-22: recovery projection dedupe

- Change: after lazy recovery-index materialization, each attr/source recovery bucket is sorted newest-first and then compacted to the newest row for each `(traceId,resultId)` pair.
- Semantics: durable candidate rows are unchanged. This only compacts the in-memory recovery projection; recovery validates immutable trace/result capsule authority, so older duplicate trace/result rows add no precision. The newest duplicate is retained for latest-history `nodeStamp` behavior.
- Fast 10-commit run: `1114`.
- Result:
  - `cold/1114`: avg 6.395262349993573s, better than `cold/1112` avg 6.427873765697586s, still slower than baseline `cold/938` avg 6.058940530301333s.
  - `hot/1114`: avg 0.864217377404566s, worse than `hot/1112` avg 0.8449245182011509s but still better than baseline `hot/938` avg 0.8804682381000021s.
- Decision: keep for now. It improves the harder cold metric and keeps the hot metric ahead of the fixed baseline.
- Remaining gap: cold is still about 0.336s slower than baseline over first10. The first cold commit is faster than baseline; the remaining gap is concentrated in later long cold commits, so the next target should reduce partial-hit/recovery CPU or validation overhead across changed commits.

## 2026-05-22: blocking queue deque

- Change: `BlockingThreadPool` now stores queued work in `std::deque` and pops with `pop_front()` instead of storing work in `std::vector` and doing `erase(begin())` for every blocking task.
- Semantics: no eval-trace cache semantics changed. This only removes avoidable O(n) queue churn in the coroutine blocking bridge used by verifier/storage calls.
- Fast 10-commit run: `1115`.
- Result:
  - `cold/1115`: avg 6.386666440198314s, slightly better than `cold/1114` avg 6.395262349993573s, still slower than baseline `cold/938` avg 6.058940530301333s.
  - `hot/1115`: avg 0.8539511923008831s, better than `hot/1114` avg 0.864217377404566s and baseline `hot/938` avg 0.8804682381000021s.
- Decision: keep. It is semantics-free and modestly improves both retained current metrics.
- Remaining gap: first10 cold is still about 0.328s slower than baseline. Next high-impact target is verifier-path fusion or reducing duplicate capsule/result encode/copy/hash work.

## 2026-05-22: current-node verifier fusion experiment

- Experiment: fuse current-node lookup, already-verified decode, zero-dep exact-session preflight, complete-probe preflight, and exact-replay descriptor acceptance into one blocking/exclusive verifier hop.
- Fast 10-commit run: `1116`.
- Result:
  - `cold/1116`: avg 6.4382324831007285s, worse than `cold/1115` avg 6.386666440198314s and baseline `cold/938` avg 6.058940530301333s.
  - `hot/1116`: avg 0.8317811028071447s, better than `hot/1115` avg 0.8539511923008831s and baseline `hot/938` avg 0.8804682381000021s.
- Decision: reverted. Hot got faster, but cold regressed materially and cold remains the metric blocking the goal.
- Implication: current-node dispatch overhead matters for hot, but not for the cold long-run gap. The remaining cold cost is more likely record/writeback encode, result/capsule materialization, or partial-hit/recovery work, not current-node preflight hop count.

## 2026-05-22: manifest dictionary single-value fast path

Tried a single-value fast path for manifest dictionary benefit planning that avoided populating the hash/source counting maps until a second distinct value was seen.

Benchmark: 10-commit run `1117` against fixed baseline `938` and retained current `1115`.

- `cold/938`: avg `6.058940530301333`, min `0.8796358700055862`, max `15.818140367999149`
- `cold/1115`: avg `6.386666440198314`, min `0.9431707450130489`, max `14.362143631005893`
- `cold/1117`: avg `6.423465135300648`, min `0.948860324017005`, max `14.272964483010583`
- `hot/938`: avg `0.8804682381000021`, min `0.8698865059996024`, max `0.8934190700019826`
- `hot/1115`: avg `0.8539511923008831`, min `0.8176442240073811`, max `0.9085120709787589`
- `hot/1117`: avg `0.8751460538012907`, min `0.8187937669863459`, max `0.9100071199936792`

Decision: reverted. It regressed both cold and hot relative to `1115`; the retained direction should move away from manifest planning heuristics and toward reducing capsule/result encode, copy, hash, and decode duplication.

## 2026-05-22: result-sidecar authority experiment

Tried a versioned capsule/storage-envelope change that stored result bytes only in the result sidecar, leaving the raw capsule as dependency/proof material plus result hash. The build succeeded and benchmark output validation passed, but this was intentionally treated as provisional because it changes the authority boundary.

Benchmark: 10-commit run `1118` against fixed baseline `938` and retained current `1115`.

- `cold/938`: avg `6.058940530301333`, min `0.8796358700055862`, max `15.818140367999149`
- `cold/1115`: avg `6.386666440198314`, min `0.9431707450130489`, max `14.362143631005893`
- `cold/1118`: avg `6.442135274596512`, min `0.9531945739872754`, max `14.407528028998058`
- `hot/938`: avg `0.8804682381000021`, min `0.8698865059996024`, max `0.8934190700019826`
- `hot/1115`: avg `0.8539511923008831`, min `0.8176442240073811`, max `0.9085120709787589`
- `hot/1118`: avg `0.8497957580984803`, min `0.8188816699839663`, max `0.8915400710247923`

Decision: revert. The hot improvement is too small to justify the semantic risk and cold regression. Next target: keep the current authority model and reduce copies/hash repetition via view-based envelope projections and payload-hash memoization.

## 2026-05-22: view-based capsule decode and payload-hash memoization

Implemented the conservative reviewer-recommended path after reverting the sidecar-authority experiment:

- v4 raw stored capsules now decode the inner raw capsule through a borrowed suffix view instead of copying it into a new `std::string` before parsing.
- Probe/result sidecar extraction can borrow from an already-owned stored envelope or from an owned prefix read, avoiding another frame copy.
- Raw capsule payload hash verification is memoized per `RawTraceCapsuleSnapshot`; the exact materialized bytes are still hashed before the flag is set.
- The capsule authority model is unchanged: result bytes remain in the raw capsule and result sidecar; sidecars are replay projections, not authority.

Benchmark: 10-commit run `1119` against fixed baseline `938` and retained current `1115`.

- `cold/938`: avg `6.058940530301333`, min `0.8796358700055862`, max `15.818140367999149`
- `cold/1115`: avg `6.386666440198314`, min `0.9431707450130489`, max `14.362143631005893`
- `cold/1119`: avg `6.376984331200947`, min `0.9571840619901195`, max `14.275251189013943`
- `hot/938`: avg `0.8804682381000021`, min `0.8698865059996024`, max `0.8934190700019826`
- `hot/1115`: avg `0.8539511923008831`, min `0.8176442240073811`, max `0.9085120709787589`
- `hot/1119`: avg `0.8250001276930561`, min `0.7883577489992604`, max `0.8769939189951401`

Decision: keep. This improves retained current cold and hot without moving the authority boundary. Remaining blocker is cold average vs baseline: short cold runs are still roughly 80ms slower, while long cold runs are already competitive or better on max.

## 2026-05-22: direct stored-envelope construction experiment

Tried writing the stored capsule envelope and raw capsule into one final `std::string`, patching the raw-size field afterward, to avoid copying a separately-built raw capsule into the stored envelope. This preserved the wire format but changed allocation/write order.

Benchmark: 10-commit run `1120` against fixed baseline `938`, retained current `1115`, and view/hash-memo run `1119`.

- `cold/938`: avg `6.058940530301333`, min `0.8796358700055862`, max `15.818140367999149`
- `cold/1115`: avg `6.386666440198314`, min `0.9431707450130489`, max `14.362143631005893`
- `cold/1119`: avg `6.376984331200947`, min `0.9571840619901195`, max `14.275251189013943`
- `cold/1120`: avg `6.771510497297276`, min `1.0758743869955651`, max `14.884681065013865`
- `hot/938`: avg `0.8804682381000021`, min `0.8698865059996024`, max `0.8934190700019826`
- `hot/1115`: avg `0.8539511923008831`, min `0.8176442240073811`, max `0.9085120709787589`
- `hot/1119`: avg `0.8250001276930561`, min `0.7883577489992604`, max `0.8769939189951401`
- `hot/1120`: avg `0.9637949861033122`, min `0.8892000979976729`, max `1.049988051992841`

Decision: revert direct-envelope construction. Despite avoiding a copy, it regressed both cold and hot, likely due to worse allocation/cache behavior or changed write pattern. Keep investigating copy reductions only when they are locally view-based or demonstrably improve data.

## 2026-05-22: exact-generation export precheck experiment

Tried skipping dirty writeback exact-generation export construction unless dirty candidate rows appeared to need persistent exact descriptors.

Benchmark: 10-commit run `1121` against fixed baseline `938` and retained view/hash-memo run `1119`.

- `cold/938`: avg `6.058940530301333`, min `0.8796358700055862`, max `15.818140367999149`
- `cold/1119`: avg `6.376984331200947`, min `0.9571840619901195`, max `14.275251189013943`
- `cold/1121`: avg `6.578081681195181`, min `1.0612438949756324`, max `14.48313540100935`
- `hot/938`: avg `0.8804682381000021`, min `0.8698865059996024`, max `0.8934190700019826`
- `hot/1119`: avg `0.8250001276930561`, min `0.7883577489992604`, max `0.8769939189951401`
- `hot/1121`: avg `0.9759482396009844`, min `0.9165206759935245`, max `1.0565637830004562`

Decision: revert. The dirty-row scan/cache effects outweighed any skipped work, and dirty runs usually still need the export path.

## 2026-05-22: result decode view experiment

Changed capsule-backed cached result materialization to decode from non-owning `std::string_view` payloads already held in `resultPayloadCache`, while preserving the public owning `ResultPayload` API and all result/hash validation boundaries.

Benchmark: 10-commit run `1124` against fixed baseline `938`, retained view/hash-memo run `1119`, and current pre-change repeat `1123`.

- `cold/938`: avg `6.058940530301333`, min `0.8796358700055862`, max `15.818140367999149`
- `cold/1119`: avg `6.376984331200947`, min `0.9571840619901195`, max `14.275251189013943`
- `cold/1123`: avg `6.668951047299197`, min `1.0905218179977965`, max `14.434937955025816`
- `cold/1124`: avg `6.418320666800719`, min `0.9362589200027287`, max `14.477501399000175`
- `hot/938`: avg `0.8804682381000021`, min `0.8698865059996024`, max `0.8934190700019826`
- `hot/1119`: avg `0.8250001276930561`, min `0.7883577489992604`, max `0.8769939189951401`
- `hot/1123`: avg `0.9561546705954242`, min `0.9153970369952731`, max `1.0609060899878386`
- `hot/1124`: avg `0.9458369813975878`, min `0.9050707510032225`, max `1.031830064021051`

Decision: keep for now. The change is authority-neutral and improved the current measured state, but it still does not beat the fixed pre-schema baseline.

## 2026-05-22: narrow already-verified verifier fusion experiment

Tried decoding already-verified current-node results under the same storage access as current-node lookup, avoiding one coroutine/blocking-store round trip after `session.verifiedTraceIds` already contains the trace.

Benchmark: 10-commit run `1125` against retained run `1124`.

- `cold/1124`: avg `6.418320666800719`, min `0.9362589200027287`, max `14.477501399000175`
- `cold/1125`: avg `6.526492604106897`, min `1.0516461390070617`, max `14.394087797001703`
- `hot/1124`: avg `0.9458369813975878`, min `0.9050707510032225`, max `1.031830064021051`
- `hot/1125`: avg `0.9641060930967796`, min `0.8912784440035466`, max `1.0234135350037832`

Decision: reverted. Even the narrow fusion worsened mean wall time, likely because the longer exclusive section/cache effects outweighed the removed queue hop.

## 2026-05-22: decoded scalar result cache experiment

Tried memoizing decoded scalar `CachedResult` values by canonical result id and hash on top of the retained string-view decoder.

Benchmark: 10-commit run `1126` against retained run `1124`.

- `cold/1124`: avg `6.418320666800719`, min `0.9362589200027287`, max `14.477501399000175`
- `cold/1126`: avg `6.482716938096564`, min `1.0131536989938468`, max `14.302432628988754`
- `hot/1124`: avg `0.9458369813975878`, min `0.9050707510032225`, max `1.031830064021051`
- `hot/1126`: avg `0.9594541709084297`, min `0.8808443140005693`, max `1.0189953390217852`

Decision: reverted. It added a second result identity cache without addressing the architectural cost center: generating, hashing, and writing heavyweight trace capsules for records that can be represented by exact replay authority.

## 2026-05-22 architecture pivot: proof-first before cursor navigation

Goal reminder: beat baseline run 938 from commit 92a3df1ab706cf514357ae57b367050b373ff146 across cold and hot wall time, without soundness/precision loss.

Findings:
- Storage-only tuning is not enough. Safe native snapshotting with a 64-segment interval regressed cold runs back to ~27-30s on several of the first 10 commits, so the interval was restored to 1024 and that direction is not the primary lever.
- Broad post-hoc descriptor generation was too expensive because it reconstructed proof material for too many rows.
- Monolithic verifier locking made cold much worse, despite improving hot, because it serialized too much work behind one proof/recovery call.
- Cursor-style CLI attr-path navigation is promising but not safe to enable before TraceParentSlot/key-membership hardening; an unverified parent shell must not authorize stale child presence.

Current implemented slice:
- Recording now builds persistent exact proof descriptors immediately for eligible traces, while canonical deps and result payload are already available.
- The verifier can then use the existing exact replay descriptor preflight before loadFullTrace/current dep replay for Git/session-covered traces.
- This follows the AC/CAS pattern: descriptor/index objects route and authorize eligible exact hits, while full capsules remain the fallback authority for unsupported or incomplete proofs.

Next adversarial checks:
- Build and run a 10-commit benchmark; compare against baseline run 938 and pre-change run 1128.
- If cold improves materially without hot regression, run 100 commits.
- If descriptor construction worsens cold recording more than it saves verification, move descriptor construction to dirty writeback but keep a lightweight in-memory descriptor source for the current process.
- Before enabling cursor navigation, harden TraceParentSlot so parent key membership/shape is proven by exact parent authority, not by stale materialized shell metadata.

## 2026-05-22 result: record-time full exact descriptors rejected

Run 1133 was stopped after the first cold commit took 38.1s. The record-time descriptor slice was reverted because it paid full proof-descriptor construction cost during cold recording and did not satisfy the fast-iteration criterion. This confirms the proof-first direction needs either compact precomputed proof summaries or value-lazy verifier proofs, not broad descriptor construction on every fresh record.

Next direction:
- Implement CLI attr-path cursoring only after key-membership hardening for TraceParentSlot, or gate it so final value verification cannot be fooled by stale parent shape.
- Focus on avoiding ancestor verification work rather than creating more durable proof material during recording.

## 2026-05-22 implemented cursor-gated architecture slice

Implemented a fail-closed CLI attr-path cursor path:
- Cached parent containers are used only as navigation indexes, not semantic authority.
- The returned leaf remains a TracedExpr thunk and is forced through the existing verifier before observation.
- The shortcut is rejected if the leaf's current trace contains TraceParentSlot, because ParentSlot currently lacks child-key membership in the dep key. Those cases fall back to the existing fully verified root/findAlongAttrPath path.
- This targets cold overhead from repeatedly verifying ancestor containers during CLI attr-path navigation while preserving exact leaf verification.

Files touched for this slice:
- src/libexpr/eval-trace/cache/trace-session.cc
- src/libexpr/include/nix/expr/eval-trace/cache/trace-session.hh
- src/libexpr/eval-trace/context.cc
- src/libexpr/include/nix/expr/eval-trace/cache/trace-backend.hh
- src/libexpr/eval-trace/store/sqlite-trace-storage.cc
- src/libexpr/include/nix/expr/eval-trace/store/sqlite-trace-storage.hh
- src/libexpr/include/nix/expr/eval-trace/store/trace-value-types.hh
- src/libcmd/installable-attr-path.cc
- src/libcmd/installable-flake.cc

Adversarial caveat:
- If most benchmarked leaf traces contain TraceParentSlot, this will correctly fall back and show little benefit. The next architectural requirement would then be redesigning TraceParentSlot to encode selected child membership, not loosening the gate.

## 2026-05-22 architectural cut: remove eager exact-hit authority export from shutdown

Baseline goal remains run `938` from `92a3df1ab706cf514357ae57b367050b373ff146`: beat cold and hot wall time, not just match the pre-schema SQLite cache.

Finding from run `1135`: after resetting the snapshot interval, the first cold commit still took `30.1s`. Because a cold cache has no current rows, the lazy attr-path cursor falls back immediately; the 30s result is therefore not a cursor lookup problem. The dominant regression is shutdown publication doing eager whole-graph proof/serving-authority construction.

Change made: `encodeGitGenerationManifestLocked` now disables eager persistent exact-hit serving authority export. Generation publication writes immutable trace/result capsules and routing rows only; exact proof/serving authority is constructed lazily by the verifier for the demanded attr path. This is the AC/CAS and Git pack/index split: indexes route to content, but content/proof validation remains finalist-only.

Related tightening: `currentTraceHasParentSlotDep` now uses the trace capsule probe as a fail-closed covering index instead of loading the full trace merely to decide cursor safety. Missing/incomplete probes fall back to full verified navigation.

Adversarial caveat: this may improve cold writeback at the cost of some hot exact replay sidecar hits. That tradeoff is intentional for this iteration because the previous sidecar path taxed every cold publication and had not proven enough hot-path value to pay for itself. The benchmark must decide whether lazy proof from capsules is sufficient or whether we need a narrower finalist-only persistent proof index.

## 2026-05-22 probe cap architecture adjustment

Finding after run `1136`: eager sidecar removal recovered the catastrophic first cold commit (`30.1s` -> `15.4s`) and hot improved over baseline (`0.846s` vs `0.880s` for the first ten commits), but cold mean remains slower than baseline (`6.543s` vs `6.059s`). The loss is concentrated on commits that publish substantial new traces, around `+1.1s` to `+1.3s` each.

Next change: reduce complete trace-capsule probe emission from `4096` deps to `512` deps. The full capsule already contains the authoritative dependency table; complete probes duplicate dependency rows in an uncompressed prefix to speed exact-session hot preflight. Treating medium traces as probe-covered makes cold publication write deps twice. The new threshold keeps probes as small covering indexes while traces above the threshold continue to persist full authoritative capsules and fail closed to full dependency replay.

Adversarial caveat: this may lose some hot preflight wins. It is sound because the probe is an optimization only; acceptance still requires the full capsule/proof path when no complete probe is present. Benchmark must confirm whether the cold win is worth any hot-path regression.

## 2026-05-22 probe cap follow-up

Run `1137` with the probe cap reduced to `512` improved both cold and hot over `1136`:

- cold: `6.543238855799s` -> `6.445410812294s`
- hot: `0.846332063293s` -> `0.839823200396s`

Because hot improved rather than regressed, complete probes for non-empty traces are not paying for themselves on the 10-commit workload. Next iteration sets the complete-probe cap to `0`, so only empty traces emit a complete probe. Non-empty traces keep exactly one dependency authority: the full capsule. This simplifies the write format and avoids duplicating dependency rows during cold publication.

## 2026-05-22 architectural writeback projection rewrite

Goal reminder: beat baseline run `938` from commit `92a3df1ab706cf514357ae57b367050b373ff146` across cold and hot wall time. The current branch already beats hot but remains slower on cold, so the active target is deleting cold publication work, not tuning local thresholds.

Findings from subagent review:
- Disabling eager exact-hit sidecar export was necessary but not sufficient: normal shutdown still did broad manifest/capsule work.
- The recorder still encoded and hashed fresh full capsules before shutdown knew whether the row would remain a cache head.
- The lazy cursor is not yet an end-to-end lazy hit path; it still returns a thunk that usually flows into normal verifier/full-trace behavior.

Changes made in this slice:
- Dirty writeback now publishes only candidate rows that are still the current node for their attr path. Superseded in-process observations are no longer durable cache entries.
- `publishRecordWithExistingResult` no longer pre-encodes and hashes trace capsules. Capsule bytes are encoded at the shutdown projection boundary only for selected dirty heads, while already-durable content remains reusable by content hash.
- Normal command shutdown always writes a dirty segment. Full snapshots/checkpoints are explicitly treated as future compaction work, not part of the eval critical path.
- Reverted the probe cap from `0` back to `512` because run `1138` regressed relative to run `1137`.

Data point before this slice:
- `cold/938`: avg `6.058940530301s`
- `hot/938`: avg `0.880468238100s`
- `cold/1137`: avg `6.445410812294s`
- `hot/1137`: avg `0.839823200396s`
- `cold/1138` with probe cap `0`: avg `6.467758976002s`
- `hot/1138` with probe cap `0`: avg `0.846265706999s`

Expected effect:
- Cold should improve if many fresh observations are superseded or if record-time capsule construction was materially extending evaluation wall time.
- Hot may regress if full snapshots had been acting as accidental compaction. If so, compaction needs to become an explicit background/tooling path or a cheap head index, not normal command shutdown.

Next architecture tranche if cold still misses baseline:
- Replace the cursor shortcut with a true lazy hit path: child-specific parent-slot deps, preflight-only current-node acceptance, and a leaf thunk carrying a preaccepted current-node token.
- Remove the flake real-root confirmation path once exact-session proof covers child membership.

## 2026-05-22 result: writeback projection slice rejected

Benchmark run: `1139` after current-head-only dirty publication, delayed capsule encoding, and dirty-segment-only normal shutdown.

Results over the fixed first 10 commits:
- `cold/1139`: avg `6.540337687408s`, worse than retained `cold/1137` avg `6.445410812294s` and baseline `cold/938` avg `6.058940530301s`.
- `hot/1139`: avg `0.943338394593s`, worse than retained `hot/1137` avg `0.839823200396s` and baseline `hot/938` avg `0.880468238100s`.

Decision: revert the slice. The data says pruning dirty candidate publication and delaying capsule construction removed rows/projections the hot path still uses, and it did not delete the dominant cold cost. The writeback boundary alone is not enough; the next large change must move the hit path itself away from root forcing/full verification.

Refined direction:
- Implement child-specific parent-slot deps so cached shape navigation can prove selected child membership instead of rejecting `TraceParentSlot` leaves.
- Add a preflight-only exact-session/current-node acceptance path that never calls `loadFullTrace`, recovery, or real-root evaluation.
- Carry a preaccepted current-node token in the lazy leaf thunk, and have `TracedExpr::eval` serve it only if the current node still matches and the verifier session has accepted the trace.
- Remove the flake real-root confirmation once child membership is in the proof key.

## 2026-05-22 child-keyed ParentSlot and lazy flake confirmation removal

Implementation slice:
- `SiblingReplayCaptureScope::appendDeps` now records `TraceParentSlot` with the child attr path as the dep key and the parent result hash as the dep hash.
- `resolveTraceContextHash` resolves `TraceParentSlot` by mapping the child attr path back to its parent through the attr-path trie, then verifying the parent current node and comparing the parent result hash.
- `currentTraceHasParentSlotDep` now treats child-keyed parent-slot deps as cursor-safe and only rejects legacy parent-keyed deps.
- `InstallableFlake::toDerivedPaths` no longer forces the real root to confirm attr existence after a cursor hit. The intended authority is now the child-keyed parent-slot proof plus leaf verification, not an out-of-band real-tree confirmation.
- `InstallableAttrPath::toValue` delays constructing the traced root until the lazy cursor misses for non-empty attr paths.

Soundness argument:
- Before this slice, ParentSlot keyed only the parent, so a cached parent shell could not prove that the selected child still existed. Cursor hits therefore needed the real-root confirmation or a fail-closed guard.
- After this slice, the ParentSlot key is the selected child path and the hash is the parent result hash. If the parent shape changes such that the child is removed or retargeted, the parent result hash must change, so verification rejects the leaf. If the parent cannot be proven current, `resolveTraceContextHash` returns `nullopt` and verification fails closed.

Risk:
- Old generations with parent-keyed TraceParentSlot deps are intentionally treated as legacy and cursor-unsafe. They should fall back or miss rather than serve stale values.
- This still is not the full preaccepted-current-node architecture; `TracedExpr::eval` can still fall into normal verifier/full-trace paths. Benchmark data will decide whether this partial hit-path rewrite is enough or whether the preflight token must come next.

## 2026-05-22 result: child-keyed ParentSlot partial slice rejected

Benchmark run: `1140` after child-keyed ParentSlot deps, cursor-safe child-keyed parent slots, flake real-root confirmation removal, and delayed traced-root construction for non-empty attr paths.

Results over the fixed first 10 commits:
- `cold/1140`: avg `6.569188181398s`, worse than retained `cold/1137` avg `6.445410812294s` and baseline `cold/938` avg `6.058940530301s`.
- `hot/1140`: avg `0.919542901599s`, worse than retained `hot/1137` avg `0.839823200396s` and baseline `hot/938` avg `0.880468238100s`.

Decision: revert the partial slice. Child-keyed ParentSlot may still be required for the final architecture, but alone it sends cursor hits into a more expensive parent proof path without providing a preaccepted verifier result. The data says the next implementation must be all-or-nothing: preflight acceptance and a leaf token that avoids the normal full verifier path, not just a different dep key plus confirmation removal.

## 2026-05-22 membership-authority cursor rewrite

Goal reminder: beat baseline run `938` from `92a3df1ab706cf514357ae57b367050b373ff146` across cold and hot wall time. The retained branch beats hot but remains slower on cold, so this slice targets root/ancestor forcing and verifier-path shape rather than storage writeback micro-tuning.

Change made:
- Removed the cursor-time `TraceParentSlot` rejection path and deleted the backend/storage API that existed only to ask whether a leaf had a parent-slot dep.
- Cached parent containers are now pure routing metadata, like a Git tree/index or remote-action-cache lookup. They may choose a candidate leaf path, but they do not authorize serving the value.
- Leaf authority remains centralized in the verifier. If the leaf proof includes `TraceParentSlot`, that obligation is now allowed to do its job: recursively prove the current parent slot and reject stale shape/key-membership. If the proof fails, the normal miss/recovery/fresh path remains responsible.

Why this is not the rejected child-keyed partial slice:
- This does not change the recorded dependency key format, so it does not make old caches legacy or alter publication shape.
- It removes the wrong fallback boundary instead of adding a new partial proof rule. The verifier already knows how to resolve `TraceParentSlot`; cursor routing should not preempt it by forcing the real root.

Adversarial concern to benchmark:
- If parent-slot recursive verification is too expensive, hot may regress. If so, the next real architecture step is a preaccepted current-node/parent-slot token that short-circuits already-proven parent membership inside the verifier session, not returning to real-root confirmation.

## 2026-05-22 result: membership-authority cursor rewrite rejected

Benchmark run `1141` after allowing cursor-routed leaves with `TraceParentSlot` to enter the normal verifier instead of forcing the real root:

- `cold/1141`: avg `6.850511684312s`, worse than retained `cold/1137` avg `6.445410812294s` and baseline `cold/938` avg `6.058940530301s`.
- `hot/1141`: avg `0.886827948503s`, worse than retained `hot/1137` avg `0.839823200396s` and slightly worse than baseline `hot/938` avg `0.880468238100s`.

Decision: reverted. The data says parent-slot recursive verification through the normal verifier is too expensive when entered from cursor routing. Merely replacing real-root confirmation/fallback with verifier recursion is not the needed architecture.

Refined conclusion:
- The next viable large change must avoid both real-root forcing and normal recursive verifier work for cursor-selected leaves.
- That requires a verifier-session-level accepted-token/current-node path: prove ancestor membership once, memoize it as path-scoped authority, and let leaf verification consume that token without `loadFullTrace`, recovery, or repeated parent recursion.
- If we cannot make that token precise and cheap, the better direction is to abandon deep traced-thunk navigation for this workload and introduce a purpose-built attr-path/result index keyed by exact session identity plus canonical attr path, with dependency proof evaluated finalist-only.

## 2026-05-22 direct exact attr-path thunk experiment

Goal reminder: beat baseline run `938` across cold and hot, not just improve over recent runs.

Change under test:
- Replace cached-parent cursor navigation with direct construction of a traced expression chain for the known CLI attr path.
- The chain is a demand locator only: it does not decode cached parent containers, does not test parent shape, and does not authorize a value.
- `TracedExpr::eval` remains the authority boundary. On exact leaf hit it serves through verifier proof; on miss it performs fresh evaluation by navigating the real tree to the demanded leaf and records that leaf.
- Deleted the now-unused `NavigationResult`/`lookupCurrentNavigationResult`/`currentTraceHasParentSlotDep` surface so the code no longer retains the rejected cached-parent shell bridge.

Why this is a larger architecture change than the rejected cursor slices:
- It avoids both real-root pre-forcing and cached-parent materialization for known CLI attr paths.
- It should reduce cold work if the benchmark only needs one deep output path per commit: fresh evaluation records the demanded leaf path directly instead of forcing/materializing the root and ancestor containers first.
- Soundness still depends on leaf proof validation and fresh miss fallback. Parent shape is checked by actual real-tree navigation during fresh evaluation; cached hits are authorized by the existing verifier rather than by the constructed chain.

Adversarial risk:
- The direct chain may skip useful ancestor cache hits or sibling identity setup, causing more leaf misses or slower fresh navigation.
- If exact leaf current nodes are absent for the benchmark after prior cache population, hot will not benefit.
- If fresh leaf evaluation still forces most of the same nixpkgs graph, the wall-time improvement may be small despite deleting ancestor materialization.

## 2026-05-22 result: direct exact attr-path thunk rejected

Benchmark run `1142` after constructing a traced expression chain directly from the known CLI attr path and deleting cached-parent navigation:

- `cold/1142`: avg `6.596178061803s`, better than rejected `cold/1141` avg `6.850511684312s`, but worse than retained `cold/1137` avg `6.445410812294s` and baseline `cold/938` avg `6.058940530301s`.
- `hot/1142`: avg `0.854587082402s`, better than rejected `hot/1141` avg `0.886827948503s`, but worse than retained `hot/1137` avg `0.839823200396s`; still faster than baseline `hot/938` avg `0.880468238100s`.

Decision: revert the direct-thunk slice. It confirms the direction but not the implementation: bypassing cached-parent navigation helps relative to `1141`, but still routes through the traced-thunk verifier/fresh-eval machinery enough to lose against `1137` and miss the baseline cold target.

Refined conclusion:
- The next architecture must bypass traced-thunk navigation for exact CLI leaf hits entirely. A purpose-built exact-session attr-path/result index should return a materializable result only after finalist proof validation.
- On miss, normal evaluation can remain the fallback. On hit, no root traced thunk, no parent shell, no recursive parent-slot verifier, and no full trace load unless the compact finalist proof is unavailable.

## 2026-05-22 direct exact result replay implementation

Implementation slice under test:
- Added a command-layer direct exact-result path before cursor/root fallback.
- `TraceSession::tryResolveAttrPathExactResult()` parses the known CLI attr path into the canonical attr-path trie, asks the backend for direct exact authority, and materializes the accepted cached result without forcing the traced root or cached parent shells.
- `TraceBackend::tryAcceptDirectExactResult()` and `Verifier::tryAcceptDirectExactResult()` are intentionally narrower than normal verification: current exact node -> exact replay descriptor -> zero-dep exact-session preflight. No complete probe, no `loadFullTrace`, no TraceContext recursion, no recovery.
- On miss, the retained cursor/root behavior runs unchanged. On exact replay hit, flake `toDerivedPaths()` skips real-root confirmation because the accepted exact descriptor is the authority boundary.

Expected effect:
- Hot may improve by bypassing `TracedExpr::eval` and normal verifier sequencing for eligible exact leaves.
- Cold should not repeat the failed broad descriptor construction because this path does not publish new authority; it consumes only authority already present.
- If exact descriptor eligibility is low, this should be near-neutral except for one current-node/direct-authority miss check per demanded attr path.

Falsification criteria:
- Any cold regression relative to retained `1137` means even finalist-only direct lookup overhead is too expensive for the benchmark path.
- No hot improvement means exact replay descriptors/zero-dep preflight are not eligible often enough, and the next design must change what we publish for demanded leaves rather than adding lookup seams.

## 2026-05-22 exact installable query pivot

- Pivoted away from verifier/cursor micro-optimization toward an installable exact-query path. A successful direct-serving exact hit now avoids traced root construction, cursor parent shells, `Verifier::verifyAttr`, full trace loading, and recovery.
- Added a synchronous direct-serving exact-result API from `TraceSession` through `TraceBackend` into `SqliteTraceStorage`. This validates current-node routing, exact session/source/policy identity, exact replay descriptor identity, and direct result-payload authority before materializing a value.
- Changed flake attr-path lookup to try all lazy cache routes before forcing the root. This avoids letting an early default-candidate miss force full evaluation when a later canonical candidate is cache-resolvable.
- Removed real-root confirmation for cursor/direct flake hits. Cursor hits are routing only and the leaf still verifies or evaluates through the trace path; forcing the real root at selection time defeated the lazy architecture.
- Added a leaf-only cold miss path for known installable attr paths. On cache miss, it evaluates the requested real attr path under one dep-capture scope, records that leaf path, and materializes the cached result with traced children when possible. Root/intermediate traced nodes are no longer mandatory cold publication work for known installable queries.
- Re-enabled eager exact authority publication only for rows explicitly marked `exactAuthorityOnlyScope`, so zero-observation direct-serving candidates can avoid full trace capsule publication without reintroducing broad whole-graph descriptor export.

### Adversarial notes

- Direct-serving replay is still conservative and scalar-biased at the materialization boundary. Container-heavy workloads will benefit mainly from the leaf-only cold path and existing traced child materialization, not from fully direct container replay yet.
- The next larger target is an `InstallableExactQuery` demand model for derived paths. `toDerivedPaths()` should be able to serve a derivation/path result directly instead of materializing a derivation attrset and then forcing `.drvPath`, `outputs`, `meta`, and related fields.
- The benchmark gate for a true hot exact hit is: no `getRootValue`, no `findAlongAttrPath`, no `Verifier::verifyAttr`, no `loadFullTrace`, and no recovery on successful direct-serving queries.

## 2026-05-22 JSON output projection experiment

Goal restated: beat pre-schema SQLite baseline run `938` from commit `92a3df1ab706cf514357ae57b367050b373ff146`, across both cold and hot wall time, in the default no-debug/no-stats eval-trace-bench mode.

Implemented experiment: add an exact command-output projection for `nix eval --json` attr-path installables. Cold evaluation rendered the same JSON output and recorded a single string payload under an internal projection demand key derived from `(attrPath, json-output, pretty/compact)`. Hot evaluation tried to verify that projection and write the JSON bytes directly, avoiding reconstruction of the Nix value tree.

Build result: `nix build -L .#nix-cli --builders ''` passed after integration fixes.

10-commit benchmark: `nix run .#eval-trace-bench -- generate --num-commits 10` auto-selected run `1144` and all outputs matched the reference.

Results against baseline run `938` over the same first 10 commits:

- `cold/938`: avg `6.058940530301`, min `0.879635870006`, max `15.818140367999`
- `hot/938`: avg `0.880468238100`, min `0.869886506000`, max `0.893419070002`
- `cold/1144`: avg `10.947048179299`, min `10.328421434009`, max `14.372040497983`
- `hot/1144`: avg `10.390370079098`, min `10.202946623991`, max `10.625681389996`

Adversarial conclusion: whole-output JSON projection is the wrong granularity. It is sound, but it is all-or-nothing. It binds the final JSON output to the full traversal dependency surface, so any changed dependency forces a complete recomputation and hot hits spend roughly full-eval time validating the giant proof. It also destroys the partial reuse pattern that made several baseline cold commits sub-second. The command hook was disabled after the benchmark so this known-bad path does not remain active.

Next architectural direction: preserve lazy partial reuse. The output path should stream JSON from per-subtree/per-leaf exact hits, using cached container shape/keyset only when that shape proof is current, and fall back to evaluating only missing subtrees. This is closer to AttrCursor-style demand-driven traversal than to monolithic command-output caching. The target is a projection-aware JSON serializer that avoids materializing cached Values when exact subtree payloads are already verified, while retaining per-child invalidation and partial reuse.

## 2026-05-22 current run after disabling monolithic JSON projection

After disabling the command hook for the failed whole-output JSON projection, rebuilt with `nix build -L .#nix-cli --builders ''` and generated run `1145` with `nix run .#eval-trace-bench -- generate --num-commits 10`.

`eval-trace-bench runs --reference cold/1145 --runs cold/938,hot/938,cold/1145,hot/1145` reported all outputs match.

Mean wall time over the 10-commit window:

- `cold/938`: `6.06s`
- `hot/938`: `0.88s`
- `cold/1145`: `6.49s`
- `hot/1145`: `0.98s`

Status: current tree is sound but still behind the real baseline. Restored cursor-before-leaf ordering in the installable demand helper after noticing the worker patch tried cold leaf-only before lazy cursor navigation. The larger conclusion is unchanged: the path to beating baseline is not a monolithic final-output projection, but a partial/lazy output traversal that keeps container-shape and child-value reuse independent.

## 2026-05-22 lazy JSON serving pivot

Run `1147` tested the first cache-native JSON streamer. It was a negative result: `cold/1147` averaged about `8.07s` over 10 commits and `hot/1147` averaged about `7.50s`, versus baseline `hot/938` about `0.872s`. The failure mode is architectural, not a micro-optimization issue: the streamer recursively called the single-path verifier for each JSON node, so output traversal became thousands of verifier/storage/scheduler operations.

Adversarial review also found that the recursive streamer was not a sound final design: subtree fallback rebuilt typed selectors as attr-path strings, list indexes and numeric attr names shared path components, missed fallback bypassed trace recording, and recursion omitted normal JSON interruption/diagnostic behavior. I removed that command path rather than polishing it.

Current pivot: `nix eval --json` asks the installable for a projected JSON output before materializing the value. This lets `InstallableFlake` resolve candidate attr paths cache-natively before `resolvedAttrPath_` is set by `toValue()`, checks an exact projected JSON hit first, and on miss does one JSON evaluation/publication rather than recursive per-node probing.

Next architectural work needed to beat baseline rather than merely recover soundness:

1. Add first-class JSON/shape authority distinct from `ExactValue`: attrsets prove keyset/order and child refs, lists prove length and child refs, scalars prove JSON leaf payload.
2. Add `verifyMany` / `verifyJsonFrontier` so a frontier of child path IDs is checked under one storage/scheduler boundary and can reuse `verifiedTraceIds`, decoded results, and current-node lookups.
3. Use complete-frontier hit or nearest-boundary fallback initially. Avoid mixed cached/real siblings until typed selector paths and subtree publication are designed.
4. Only after the fragment authority unit exists, move the hot storage path to native content-addressed objects or mmap/range-read snapshots; replacing SQLite first would still replay the wrong granularity.

## 2026-05-22 projected JSON run 1148

Run `1148` tested installable-level whole-output JSON projection after removing the recursive streamer from the command path. Results over 10 commits: `cold/1148` avg `10.83s`, `hot/1148` avg `10.41s`; baseline `hot/938` is about `0.872s` over 100 commits. This confirms that broad whole-output dependency replay is not viable through the normal verifier.

Correction under test: JSON projection payloads are scalar strings, so exact-session hot hits should first try the existing direct-serving exact descriptor path before falling back to normal verifier replay. If that does not make hot dramatically faster, whole-output projection should be kept disabled except as a diagnostic experiment, and work should proceed directly to fragment/frontier authority.

## 2026-05-22 projected JSON run 1149

Run `1149` added direct-serving lookup before normal JSON projection verification. Results over 10 commits: `cold/1149` avg `10.81s`, `hot/1149` avg `10.00s`. Direct-serving did not make the projection eligible in this workload. I disabled the whole-output JSON projection implementation path while keeping the installable-level API seam for a future fragment/frontier JSON server.

Conclusion: whole-output projection is the wrong granularity. It is all-or-nothing for dependencies, destroys partial reuse, and adds cold publication cost. The next implementation must represent JSON-serving authority at the container/leaf fragment level and verify a frontier in batches.

## 2026-05-22 JSON fast-path architecture notes

- Baseline remains `cold/938` avg `3.2927806688501007s` and `hot/938` avg `0.8717789067701961s` over 100 commits from `92a3df1ab706cf514357ae57b367050b373ff146`.
- Run `1150` (batched frontier using normal verifier) improved the prior catastrophic JSON attempts but still missed baseline: cold avg `7.143002960996819s`, hot avg `0.8950873710942687s` over 10 commits.
- Run `1151` added root current-session probing and direct JSON projection lookup before frontier serving. It did not solve the main regression: cold avg `6.602923856896814s`, hot avg `0.9108449486957397s` over 10 commits. Conclusion: stale JSON-frontier probing was not the main cold bottleneck.
- Architecture review conclusion: `verifyMany` is only boundary batching. It still loops over full `SqliteTraceStorage::verify`, which can perform descriptor attempts, exact-session preflight, full trace loads, proof replay, and recovery for every JSON node.
- Current implementation direction: JSON frontier serving now uses a new exact-result batch API that accepts only current-session exact replay/preflight authority and never calls full verification or recovery from the command-level fast path. Misses fall back to normal evaluation. This is the necessary separation between fast-path action-result replay and general recovery.
- Remaining large architectural tasks: publish first-class JSON projection certificates during normal JSON output, add JSON-specific result views/shape authority to avoid full `CachedResult` decode, and move the durable hot path from SQLite to a file-native append-only CAS segment store with mmap indexes.

## 2026-05-22 exact-batch JSON benchmark

- Run `1152` replaced JSON frontier `verifyMany` with `tryAcceptDirectExactResults`, which accepts only current-session exact replay/preflight authority and never invokes full verification/recovery from the fast path.
- Same 10-commit baseline slice from run `938`: cold avg `6.058940530301333s`, hot avg `0.8804682381000021s`.
- Run `1152`: cold avg `6.55172005110362s`, hot avg `0.8982935637963237s`.
- Interpretation: exact-batch serving is directionally correct for hot versus `1151`, but still not enough. The remaining gap is not lock/scheduler overhead alone. We need first-class JSON/shape projection authority, plus a file-native segment store to remove SQLite startup/shutdown and writeback costs.

## 2026-05-22 architectural pivot: remove eager JSON frontier

The eager cache-native JSON frontier was a failed direction. It tried to reconstruct the entire output by batch-accepting exact cached values and then rendering JSON from `CachedResult`. That improved neither cold nor hot enough, and whole-output JSON projection publication was catastrophically wrong granularity:

- Run 1152, exact-batch frontier: cold avg 6.55172005110362s, hot avg 0.8982935637963237s for the 10-commit slice.
- Run 1153, whole-output JSON projection publication: cold avg 10.551535114701256s, hot avg 10.187461602099939s for the same slice.

The next architectural cut removes the command-layer JSON fast path entirely. `nix eval --json` now falls through to the traced `Value` graph, so JSON rendering interleaves traversal with `TracedExpr` verification/recording instead of doing a complete pre-render BFS over cached values. This follows the Nix eval-cache `AttrCursor` shape and the Adapton/demand-driven incremental-computation model: validate or recompute at demand boundaries, not by eagerly materializing the whole output graph first.

Expected result to test: hot should improve by avoiding duplicate cached-value reconstruction/rendering; cold may improve if the eager JSON path was causing extra whole-output work, but the real cold bottleneck may still be writeback/publication.

## 2026-05-22 action-output cache status

- Baseline remains run 938 at commit 92a3df1ab706cf514357ae57b367050b373ff146.
- Run 1156, same 10-commit window:
  - cold avg 6.570732599197072s vs baseline 6.058940530301333s: still slower.
  - hot avg 0.6283286134013906s vs baseline 0.8804682381000021s: exact command-output action cache beats baseline hot.
- Interpretation: the command-output action cache is the first change that wins on hot hits. Cold still loses because the miss path was evaluating/publishing through the trace graph before writing the action artifact.
- Follow-up change: make JSON action-cache misses evaluate the real root and publish only the exact command output. This is the architectural cut needed for cold: do not construct a traced root for a cache product that only needs final JSON bytes.
- Soundness note: the command-output artifact is exact for the action key. The remaining design issue is verifiable observation capture for dynamic/impure inputs; a session-key-only action cache is only as sound as the semantic session key. The durable design needs an observation manifest/checker for action-cache hits, analogous to the trace verifier but scoped to command outputs.

## 2026-05-22 pre-backend action lookup result

- Run 1157 tried real-root-only action-cache misses. Result: hot stayed fast, but cold regressed to 7.3325541208963845s avg because partial trace reuse was bypassed. Rejected as a blanket strategy.
- Run 1158 moved action-cache lookup before trace backend construction by computing the semantic session key from session-open inputs. Result:
  - cold avg 7.121421331798774s vs baseline 6.058940530301333s.
  - hot avg 0.6243687973998021s vs baseline 0.8804682381000021s.
- Interpretation: pre-backend lookup only improves hot by ~4ms over run 1156 but adds substantial cold overhead, likely from duplicating session-open input capture/key construction before the traced fallback also opens a session. This form should not be kept.
- Next architecture direction: keep command-output action cache as a hot tier, but avoid duplicate cold key construction. To beat cold baseline, target trace publication/recovery granularity and cross-commit cold reuse, not prechecking action-cache entries with expensive semantic-key construction.

## 2026-05-22 selective action-fill rejection

- Run 1160 tried preserving existing trace hits but skipping cold trace publication on JSON action-cache misses.
- Result: cold avg 8.071660119699663s, hot avg 0.7533434777084039s; both worse than run 1159 and cold worse than baseline.
- Root cause: cold trace publication is currently what feeds cross-commit partial reuse. Skipping it reduces the first full miss but starves later commits, converting cheap partial hits into full real evaluations.
- Decision: reverted. The next viable architecture must add a real cross-commit command-action recovery index/certificate, or make trace publication cheaper, rather than skipping publication without an equivalent replacement.

## 2026-05-22 parallel adversarial review: verified findings

Goal remains fixed: beat pre-schema SQLite baseline run `938` from
`92a3df1ab706cf514357ae57b367050b373ff146` across cold and hot wall time,
without soundness or precision loss.

Verified implementation facts:

- `nix eval --json` can return from `InstallableValue::tryServeJsonOutput()`
  before `toValue()` or normal trace replay. Therefore any JSON fast path is
  an authority path, not just an optimization.
- Current `eval-trace-json-action-v1` is keyed by semantic session key,
  requested attr path, and pretty/compact shape, then stores raw `.json` and
  `.ctx` files. It does not bind payload bytes/context to a certificate,
  renderer ABI, proof digest, or current observation validation.
- File-eval session identity falls back to logical source identity based on the
  absolute file path plus lookup/policy inputs when no Git identity snapshot is
  available. For non-git `--file`, editing the file can keep the same semantic
  session key. The current v1 JSON action cache is therefore not sound as an
  unconditional hit path for file eval or any other session whose identity does
  not prove all dynamic inputs.
- `TraceSession::InstallableDemand::Projection::JsonOutput` already exists, but
  the current retained JSON path asks for `Projection::Value` and then renders
  JSON as a side artifact. That is the wrong authority boundary.
- The failed `1157`/`1160` experiments are explained by the same structural
  fact: trace publication currently provides cross-commit partial reuse. Skipping
  it without a replacement command-output certificate/recovery product makes
  later cold commits slower.

Subagent findings accepted after verification:

- Keep the hot action-cache result only as a direction, not as the final
  authority model. It must become a certified JSON action/projection artifact or
  be gated to cases where trace/proof authority has already accepted the result.
- Indexes are selectors only. JSON payload bytes and context must be bound by
  typed descriptors and content hashes, and replay must fail closed on unknown
  schema/provider/renderer/proof domains.
- JSON output is distinct from `CachedResult`: it authorizes command bytes and
  string context for one JSON demand, not a reusable Nix value.
- A file-native append-only generation/action segment remains the likely durable
  data-plane target, but it is not the first fix. The immediate blocker is the
  authority model around JSON projection/action output.

Refined next implementation plan:

- Strictly gate `eval-trace-json-action-v1` hits for sessions whose
  semantic key is not known to be exact authority for the output. Do not rely on
  semantic session key alone for non-git file eval or unsupported impure inputs.
- Promote JSON output to a first-class `JsonOutput` projection artifact:
  demand descriptor includes canonical attr path, compact/pretty mode,
  `copyToStore=false`, renderer ABI/provider/schema epochs, and string-context
  policy.
- Store JSON payload as bytes plus serialized string context, with a payload
  digest binding both. Do not re-dump floats/strings on hot hits.
- Add an exact JSON projection serving path that validates proof/result/payload
  descriptors and returns `JsonInstallableOutput` directly, without
  `decodeCachedResult`, `materializeResult`, or `printValueAsJSON`.
- Keep normal trace publication on misses until a certified cross-commit JSON
  recovery index exists. Cold performance cannot improve by starving the current
  partial-reuse mechanism.
- After exact JSON projection authority exists, add a compact in-memory/file
  manifest for action/projection entries so misses do not perform per-candidate
  filesystem probes.
- Longer-term: move JSON projection/action certificates and exact-hit descriptors
  into append-only generation segments with fixed-width mmap indexes and atomic
  `HEAD` publication. SQLite then becomes an oracle/fallback during transition,
  not the hot data plane.

Implementation note:

- Added the first safety gate for the retained v1 JSON action cache. Flakes may
  use it only when the locked-flake fingerprint is exact and evaluation is pure.
  Attr-path/file installables may use it only under pure eval and exact expression
  identity or Git identity. Non-git `--file` and impure eval now miss the v1
  action cache and use the trace-backed path. This deliberately gives up the
  unsound hot win for `-f release.nix` until `JsonOutput` becomes a certified
  projection artifact.
- Build after this gate passed with
  `nix build -L .#nix-cli --builders ''`.
- 10-commit benchmark run `1161` after the gate:
  - cold avg about `6.60s`, still slower than baseline `cold/938` about
    `6.06s` and about equal to/slightly slower than retained unsafe
    `cold/1159` about `6.55s`.
  - hot avg about `1.0s`, slower than baseline `hot/938` about `0.88s` and
    much slower than unsafe `hot/1159` about `0.62s`.
  - all outputs matched reference.

Conclusion from run `1161`: the old hot win was real but came from skipping
authority. It cannot be the performance target. The next implementation must
recover the same direct-byte serving shape through a proof-backed `JsonOutput`
artifact, not by loosening this gate.

## 2026-05-22 failed whole-output JsonOutput projection experiment

Tried the narrow proof-backed version: render JSON bytes/context under a
`Projection::JsonOutput` attr path and store them as a string `CachedResult`
through the existing trace recorder. Hot serving would then use the existing
exact/direct-serving verifier rather than the v1 side cache.

Implementation was intentionally conservative: on projection miss, evaluate and
render from the real root under a dependency-capture scope before recording the
JSON string. This avoided recording empty deps from an already-materialized
cached value, but it also bypassed the partial trace reuse path.

Result from partial run `1162`:

- Cold regressed immediately. Commits that were sub-second in `cold/938` and
  `cold/1159` became about `10.5s` each.
- Hot did not recover. The first hot commits were about `11s`, indicating the
  persisted JSON projection was not accepted as a useful direct hit for this
  workload.
- Stopped the run and removed the activation/code rather than leaving a
  known-bad path in-tree.

Conclusion: whole-output `JsonOutput` as a normal trace string is the wrong
first implementation. It is all-or-nothing, bypasses partial reuse on miss, and
does not become a cheap accepted direct-serving artifact in the current verifier
shape. The next viable implementation needs either:

- fragment/frontier JSON authority that preserves container/child partial reuse,
  or
- a dedicated current-node action artifact accepted by direct-serving verifier
  without going through real-root whole-output publication.

## 2026-05-23 authorized projection prototype review and run 1194

User request: dig deeper with parallel review, adversarially inspect the
authorized shape projection prototype, and keep a durable record of what was
attempted and observed.

Parallel review performed:

- Recovery/proof-semantics review: found that the monolithic JSON projection
  row is just a normal synthetic attr-path trace with about 36k deps. Direct
  serving does not accept it because non-empty dependency obligations are not a
  directly checkable proof in the current model. Whole-output recovery is sound
  but losing; changed source bytes/listings make `DirectHash` miss. Exact JSON
  projection probes should use a no-recovery check, not full recovery.
- Baseline/benchmark review: confirmed that the measured hot win is mostly the
  action-cache bypass and is not proof-projection evidence. The cold target
  remains `cold/938` at about `6.06s`; `hot/938` at about `0.88s` is only a
  valid comparator when the hot path is proof-authorized, not action-cache
  skipped.
- Adversarial review: found that `tryServeOutPathJsonObject` treated projection
  route-chain acceptance as advisory. That is unsound for reconstructing nested
  output from cached fragments; route-chain acceptance must be mandatory.

Implementation changes kept after the review:

- Made route-chain acceptance mandatory before serving the reconstructed
  `outPath` JSON object path.
- Changed JSON projection exact probes to `verifyNoRecovery` so stale recovery
  does not hide the real exact-hit behavior.
- Disabled publication of target/child/outPath fragment rows in this prototype;
  only the final JSON projection row is written. This reduced debug publication
  from 12 rows to 2 rows on the three-commit debug slice.
- Tightened direct-serving proof eligibility: compact/direct descriptors now
  reject non-empty canonical dependencies, and `servingV1ProofIsDirectlyCheckable`
  accepts only empty obligations/coverage after the normal well-formed checks.
- Cached decoded parent shapes in `AuthorizedProjectionIndex` only after using
  the existing proof path for authorization. This removes duplicate decode work
  but is not itself new authority.

Measured result:

| Run | Cold mean | Hot mean | Notes |
| --- | ---: | ---: | --- |
| `938` | `6.058940530301333s` | `0.8804682381000021s` | pre-schema SQLite eval-cache baseline |
| `1192` | `7.00672331629612s` | `0.3729082848032704s` | earlier projection rewrite comparator |
| `1193` | `6.636674648197368s` | `0.3324738475988852s` | intermediate comparator |
| `1194` | `6.571475537799415s` | `0.3751932549988851s` | final-row-only JSON projection prototype |

Hot did improve materially versus the same-10 `hot/938` comparator
(`0.375s` vs `0.880s` for `1194`, and about `0.37s` for follow-up runs), but
the debug counters showed this was not authorized route projection evidence:
hot stats were effectively zero because the command-output/action cache served
before the projection verifier path.

Correctness matched on the 10-commit slice when comparing `cold/1194` against
`cold/938`, `hot/938`, `cold/1192`, `hot/1192`, `cold/1193`, and `hot/1193`,
and comparing `hot/1194` against `cold/1194`.

Debug observations:

- `cold-debug/1084`: `routeAttempts=3`, `routeAccepted=0`,
  `routeRejected=3`, `verifyCount=3`, `verifyPassed=1`,
  `verifyFailed=2`, `recoveryAttempts=0`, `loadTraceCount=2`,
  `recordCount=2`.
- Final-row-only publication reduced cache size on the 10-commit slice from
  about `116M` for `cold/1192`/`cold/1193` to about `28M` for `cold/1194`.
- Hot stats were zero in the debug run, confirming that the hot number is an
  action-cache bypass measurement, not an authorized projection navigation
  measurement.

Decision:

- Keep the soundness fixes and final-row-only cleanup. Do not claim the
  prototype beat the real baseline; it did not beat `cold/938`.
- `AuthorizedProjectionIndex` remains useful secondary infrastructure for
  positive navigation once root/parent shape authority exists, but this workload
  did not benefit because no root route was authorized (`routeAccepted=0`).
- Whole-output JSON projection exact hits help only repeated exact-dependency
  commits. They cannot recover changed-source commits and still pay the wrong
  proof shape when represented as normal trace rows with large dependency sets.
- The next viable path is a first-class JSON projection/action certificate, or a
  proof-level route/shape certificate, that can authorize non-empty structured
  dependencies without replaying the full 37k-dependency verifier path. More
  route-index tuning is not enough until that authority exists.

Covered and ruled out in this slice:

- Runtime whole-generation refresh in the lookup hot path: removed from the
  benchmark target because it obscures whether projections help.
- Advisory route-chain checks: rejected as unsound.
- Full recovery of monolithic JSON projection rows: rejected as a cold/hot
  performance loser.
- Publishing child fragment rows in this prototype: rejected for now because it
  adds writeback and storage without accepted route authority.

## 2026-05-23 follow-up projection variants after parallel review

After the parallel design/adversarial review, two narrower implementation
variants were tested against the same 10-commit gate.

Variant `1195`: make `tryServeJsonOutputProjection` exact/current serving only
and let misses fall back to the normal value-demand renderer. This removed the
final JSON projection publisher, but it caused the fallback renderer to record
the child derivation and outPath rows again. Result:

- `cold/1195` mean `6.799658301402815s`.
- `hot/1195` mean `0.3710460706963204s`.
- Cache size about `88M`.
- Outputs matched the selected reference runs.

Rejected. This proved that the final-row-only publisher in `1194` was doing
useful writeback avoidance even though it did not beat baseline. Removing it
made cold slower and cache size larger.

Variant `1196`: publish target shape plus per-child `drv-output` JSON string
projection rows, and allow serving from a verified target keyset plus accepted
child output projections. Debug run `1090` showed the variant was real, not
dead code: on the second commit it reduced record rows from 7 to 4 and full
trace loads from about 11 to 4, and it served the third commit through the
outPath-object path. It still had to recompute about 30k dependency hashes for
the child output projection and recovery missed. Result:

- `cold/1196` mean `6.686723071397864s`.
- `hot/1196` mean `0.3717677208973328s`.
- Cache size about `58M`.
- Outputs matched the selected reference runs.

Rejected. It improved over the miss-only variant but regressed `1194`
(`6.571475537799415s`) and still missed the real cold baseline
(`6.058940530301333s` for the same 10 commits). The child-output projection
reduced materialization/writeback but did not remove the expensive proof work:
the verifier still recomputed the large structured-projection dependency set.

Current retained code state after this review:

- Keep `1194` shape: mandatory route-chain checks for fragment serving,
  `verifyNoRecovery` for exact JSON projection probes, and final-row-only JSON
  publication on miss.
- Do not keep `1195` or `1196` code. Their benchmark results are useful design
  evidence but not better implementation states.
- The next credible implementation is still proof-level, not route-index-level:
  add a session-aware direct-serving path backed by complete-probe validation
  or a first-class JSON/action certificate that authorizes non-empty structured
  dependencies without replaying the full verifier.

## 2026-05-23 parallel implementation leads and cleanup

Follow-up work was split into three leads, but the subagents initially ran in
the shared checkout rather than isolated workspaces. They did confirm they were
on the current dirty branch (`vibe-coding/file-based-eval-cache` at
`21939debb9e5586ba26967a717b63d1e42f81f13`), but their patches were treated as
candidate edits requiring review, not accepted implementation.

Lead A: JSON action-cache certificate.

- Changed the command JSON action-cache ready marker into a JSON certificate
  containing cache kind, key digest, renderer id, and SHA-256 digests for the
  JSON/context files.
- Built `.#nix-cli` and ran a one-commit smoke benchmark as run `1197`.
- Rejected and reverted. This was an integrity wrapper around the exact-action
  hot cache, not a proof path for non-empty JSON projection dependencies. It
  did not address the cold baseline miss and would add work to the already
  caveated hot action-cache path.

Lead B: authority-backed positive route cache.

- Sketched a `PositiveRoute` map and `tryAcceptAuthorizedProjectionChainFromAuthority`
  path that would accept requested parent->child steps from an already verified
  descendant authority.
- Rejected and reverted as incomplete. It had declarations/wrappers but no
  verifier implementation or call site, so it would not build or make
  `routeAccepted` move. The design remains plausible only if the authority row
  has first been accepted by a real proof path for the same demanded descendant.

Lead C/local probe: remove repeated target deps from child forcing before final
JSON projection writeback.

- Hypothesis: final-row-only JSON publication in `1194` might be paying raw
  dep-vector sort/hash overhead by appending `targetCached->deps` once per
  child even though the final trace needs the target deps only once.
- Built `.#nix-cli`, then ran `cold-debug/1091` and `hot-debug/1091` for three
  commits.
- Result versus `cold-debug/1084`: no useful movement. `verify.depsChecked`
  remained `37377`/`38569` on the relevant commits, route acceptance remained
  `0`, and record hash/sort timings were effectively unchanged
  (`record.hashUs` about `38-40ms`, `record.sortUs` about `61-65ms`).
- Rejected and reverted. The duplicate raw dep vector was not the cold
  regression lever.

Current cleanup status after these parallel leads:

- `nix build .#nix-cli -o result --builders ''` passes.
- Retained source state remains the `1194` shape: mandatory route-chain checks,
  `verifyNoRecovery` exact JSON projection probes, and final-row-only JSON
  publication on miss.
- The next implementation should not add more route-index plumbing until it
  creates real authority. The two still credible directions are:
  session-aware complete-probe-backed direct serving for non-empty exact JSON
  projection rows, or a first-class projection/action certificate that can be
  validated without replaying the full 37k-dependency verifier path.

Additional local follow-up: fragment rows with child-direct deps only.

- Hypothesis: `1196` may have lost because each child `drv-output` projection
  row still inherited target keyset deps. A sound split would store target
  shape authority separately and let child output rows carry only the deps
  captured while forcing that child/output.
- Prototype: enabled fragment publication and passed no inherited deps to the
  child `recordForcedValue` call.
- Built `.#nix-cli` and ran `cold-debug/1092`/`hot-debug/1092` for three
  commits.
- Rejected. Route acceptance still stayed at `0`, verifier deps stayed at
  `37377`/`38569`, record rows increased from `1` to `6` on the publishing
  commits, record time rose from about `110-122ms` to about `430-438ms`, and
  debug cache size grew from `11M` (`1084`) to `47M` (`1092`).
- Reverted to `1194` shape and rebuilt successfully with
  `nix build .#nix-cli -o result --builders ''`.

Isolated workspace correction.

- New isolated worktrees were created at
  `/tmp/nix-projection-worktrees/complete-probe` and
  `/tmp/nix-projection-worktrees/projection-cert`.
- Both are detached at the current branch commit
  `21939debb9e5586ba26967a717b63d1e42f81f13`, not `master`, with the dirty
  branch patch applied.
- The first isolated build attempts exposed a flake-source trap: untracked
  files from the dirty checkout were present on disk but not visible to Nix.
  The worktrees now mark those new files with `git add -N`, so future
  worktree-local `nix build` runs see the same source shape as the shared
  checkout.

Additional isolated follow-up: larger complete probes for JSON projection
rows.

- Hypothesis: the non-empty JSON projection row might become cheap enough if
  the capsule stores a complete probe for that row, allowing the existing
  exact-session probe preflight to accept it without inflating the full trace.
- Prototype: added a per-capsule probe cap and raised it only for
  `json-output` projection paths.
- Built `.#nix-cli` in the shared checkout and ran
  `cold-debug/1093`/`hot-debug/1093` for three commits.
- Rejected. Outputs matched `cold-debug/1084`, but wall time did not improve
  (`1093` was about `11.20s` mean debug cold versus about `11.03s` for
  `1084`), cache size grew from `11M` to `22M`, full payload bytes doubled
  from about `5.5MB` to `11.3MB`, and `verify.depsChecked` stayed at
  `37377`/`38569`.
- Diagnosis: the complete-probe exact-session preflight currently runs only
  for current exact rows. The reusable JSON projection row is found through
  history/recovery on the next commit, so verifier still falls into the full
  trace path. Raising the probe cap only duplicated dependency rows into the
  payload; it did not create cross-commit serving authority.
- Reverted. Retained state is again the `1194` shape.

Updated direction after adversarial review.

- Hot improvements observed so far are real but mostly belong to the command
  JSON action-cache/direct-output lane, not to authorized projection routing.
  The decisive debug counter is still `projection.routeAccepted == 0`.
- The current route-index abstraction is not inherently unsound, but it cannot
  beat the baseline by itself. It needs an accepted authority row first; route
  lookup without a proof only adds control-flow work.
- The credible next implementation is a history/recovery authority for this
  exact demand: either a first-class projection/action certificate that
  validates compact coverage facts for the full dependency set, or a different
  publication shape where reusable rows have small, independently accepted
  recovery deps. More hot-path route plumbing should wait until one of those
  authority paths can make `routeAccepted` or direct projection acceptance move.

## 2026-05-23 corrected isolated parallel leads

After correcting the workspace setup, the next parallel candidates were based
on the current dirty branch, not `master`: each isolated worktree was detached
at `21939debb9e5586ba26967a717b63d1e42f81f13` with the shared checkout's
dirty patch applied, and new files were marked with `git add -N` so Nix's flake
source saw the same file set.

Lead D: narrow direct child `outPath` publisher.

- Hypothesis: the final JSON output publisher was forcing too much child
  derivation structure; directly forcing only each child derivation's `outPath`
  could reduce dependency capture, trace replay, and wall time.
- Implemented in an isolated worktree by adding a direct child-outPath
  projection helper, then integrated temporarily into the shared checkout for
  benchmarking.
- Built `.#nix-cli` and ran `cold-debug/1094`/`hot-debug/1094` for three
  commits.
- Rejected and reverted. Cache size dropped from about `11M` to about `5.5M`,
  but wall time was slightly worse and the expensive counters did not move:
  `depTracker.scopes=15226`, `ownDepsTotal=63466`, `ownDepsMax=31733`,
  `replay.added=5918`, `weakRangeStoredDeps=4548549`, `verify.depsChecked`
  still `37377`/`38569`, `loadTrace.count` still `1`, and
  `projection.routeAccepted` still `0`.
- Diagnosis: the workload really is reading the two system derivation
  `outPath`s, but producing those `outPath`s still instantiates/evaluates large
  NixOS system derivations. Narrowing the local publication shape reduced
  storage only; it did not reduce the proof work that dominates the cold miss.

Lead E: complete-probe recovery helper.

- Hypothesis: complete capsule probes could avoid decoding the full trace for
  history/recovery hits, not just current exact-session hits.
- Implemented in an isolated worktree as a verifier helper that can validate
  complete probe deps before falling back to the full trace.
- Built successfully in isolation, but not integrated into the shared checkout.
- Rejected for this workload unless paired with a separate compact proof
  design. Current retained capsule probes are capped too low for the roughly
  37k-dependency JSON projection row, while raising the cap alone had already
  doubled payload size without reducing `verify.depsChecked`. Even if complete
  probe recovery avoided `loadFullTrace`, the measured full-load/decode cost is
  only about `10ms`; `verifyTrace` and dependency resolution are about
  `285ms`.

Lead F: stale-source bootstrap filter.

- Hypothesis: rejecting history candidates whose stored source identity digest
  differs from the current source identity could avoid expensive doomed
  recovery attempts.
- Implemented locally in `lookupLatestHistoryForAttr`, built `.#nix-cli`, and
  ran `cold-debug/1095` for three commits.
- Rejected and reverted. The third commit regressed from the useful recovered
  path (`~1.3-1.5s`) to full evaluation (`~13.9s`): `verify.depsChecked=0`,
  `loadTrace.count=0`, and `record.count=1`.
- Diagnosis: cross-source history hits are real and important. A changed
  source identity is not by itself a proof of invalidity; the verifier can
  still prove that the relevant output stayed valid.

Current conclusion after these leads:

- The retained source state remains the `1194` shape.
- More route-indexing or publication-shape tuning is not the main lever until
  an authority path accepts a reusable projection row.
- The dominant remaining cost is dependency resolution and verification on
  cross-source history hits, not full-trace loading or child materialization.
  Representative counters for a recovered hit: `verifyTraceCallUs` about
  `285ms`, `depHash.contentUs` about `142ms`, `structuredOuterUs` about
  `48ms`, `storePathBatchUs` about `39ms`, and `loadFullTraceUs` about `10ms`.
- Next search target: a compact or batched source-content proof for history
  hits, likely by validating many file/directory/structured deps against Git
  tree/source metadata in bulk instead of resolving every dependency one at a
  time.

## 2026-05-23 current-state parallel follow-up

Workspace correction:

- Created fresh implementation worktrees from the shared checkout's current
  branch state, not `master`:
  `/tmp/nix-projection-worktrees/source-cert-current-20260523192802`,
  `/tmp/nix-projection-worktrees/dep-batch-current-20260523192802`, and
  `/tmp/nix-projection-worktrees/candidate-selection-current-20260523192802`.
- Verified each worktree is detached at
  `21939debb9e5586ba26967a717b63d1e42f81f13` and has the same tracked patch
  hash and untracked-content hash as `/home/connorbaker/nix`.
- The first worker spawn attempt used an unavailable model override and failed;
  the workers were relaunched with the default agent model against the same
  verified worktrees.

Lead G: widen `sourceContentHash` Nix structured shortcut to candidate deps.

- Hypothesis: `Dep::Key::sourceContentHash` is object-bound proof data, not
  merely a current-trace subsumption mark. If a candidate structured Nix dep's
  current source file bytes hash to that guard, the stored binding hash can be
  reused without reparsing the file.
- Implemented locally by removing the `Origin == CurrentTrace` gate around the
  Nix `sourceContentHash` shortcut in `dep-resolution-service.cc`; built
  `.#nix-cli` successfully and ran `cold-debug/1099`/`hot-debug/1099` for three
  commits.
- Rejected and reverted. Counters showed
  `structuredNixSourceHashHits=0`, `structuredNixSourceHashMisses=0`,
  `structuredNixParseUs=0`, and `structuredNixUs=0` on the relevant cold and hot
  commits. The workload's replay cost is not Nix binding structured deps.
- Timings were effectively neutral/noisy: `cold-debug/1099` was about
  `18.55s`, `14.15s`, `1.37s`; hot was about `0.32-0.35s`.

Lead H: history candidate scoring before bootstrap.

- Hypothesis: `lookupLatestHistoryForAttr` might be choosing a newest but large
  or source-identity-bearing candidate when an older, smaller, complete-probe, or
  no-source candidate could route to cheaper authoritative verification.
- Implemented in an isolated current-state worktree by scoring the in-memory
  history bucket using trace headers, already-loaded deps, probe sidecars, raw
  payload size metadata, and durable recency as a tie-breaker. The worker build
  succeeded.
- Integrated temporarily into the shared checkout and ran
  `cold-debug/1100`/`hot-debug/1100` for three commits.
- Rejected and reverted. No `eval-trace/history: selected candidate` debug line
  fired, `verify.depsChecked` stayed `37376`/`38568`, cache effectiveness stayed
  the same, and wall time was effectively unchanged/slightly worse:
  `cold-debug/1100` mean `11.41s` versus `cold-debug/1099` mean `11.35s`.
- Diagnosis: the tested buckets either have a single plausible candidate or the
  newest candidate is already the one this heuristic would choose. Candidate
  routing is not the current 3-commit bottleneck.

Lead I: source-proof/certificate search.

- The source-certificate worker found no sound no-schema verifier-side
  acceptance path from the material persisted today.
- `sourceIdentityDigest`, candidate scope rows, proof summary fields, and
  complete probes are routing/projection material. They do not form an
  object-bound proof that every governed `FileBytes`, `DirectoryEntries`,
  `StructuredProjection`, and `ImplicitStructure` dep is covered for a history
  candidate.
- Next minimal persisted fact: an object-bound source-coverage certificate tied
  to the immutable trace/result candidate. It needs the candidate trace capsule
  identity, governing repo/source identity, a canonical covered-obligation
  digest, and typed reject/coverage facts for unsupported observation domains.

Current conclusion:

- The no-schema search space is close to exhausted for the measured cold gap.
  Positive route projections, child publication shape, stale-source filtering,
  exact source identity equality, per-candidate Git identity recompute,
  per-file clean-Git hashing, Nix source guard widening, and history candidate
  scoring have all failed to reduce the measured verifier replay path.
- The design is not fundamentally incapable, but beating `cold/938` now appears
  to require changing persisted authority, not just lookup routing: either a
  compact source-coverage certificate for history hits or a first-class
  JSON/action projection certificate that can be accepted without replaying the
  monolithic dependency vector.

Lead J: history-bootstrapped projection route chain.

- Hypothesis: `projection.routeAccepted == 0` might be an implementation gap,
  not a design limit. The retained route index only calls
  `lookupCurrentNode(parent)`, so cold cross-generation runs reject immediately
  at `root -> closures` even when history rows exist.
- Prototype 1: let route construction call normal `verify(parent)` to
  bootstrap a historical parent before decoding cached parent shape; also
  temporarily enabled JSON fragment rows. Built `.#nix-cli` and ran
  `cold-debug/1101`/`hot-debug/1101`.
- Result: still rejected at `root -> closures` because the JSON publisher did
  not persist ancestor container rows. Wall time stayed effectively neutral, but
  record count rose from `1` to `6` on publishing commits.
- Prototype 2: also published ancestor container rows (`root`, `closures`) from
  the existing navigation capture. Built and ran `cold-debug/1102`/`hot-debug/1102`.
  This proved the route abstraction can work: `projection.routeAccepted` moved
  from `0` to `2`, and the third commit served the outPath object through the
  fragment path.
- Rejected and reverted. The accepted route did more proof work than the
  monolithic JSON projection: commit `3f7b...` rose to `verify.count=3`,
  `depsChecked=100834`, `loadTrace.count=16`, and still fell back to fresh eval
  after child-shape recovery failed; commit `534...` rose from about `1.3s` to
  about `2.0s` while `depsChecked` increased from about `38.6k` to `65.8k`.
- Prototype 3: published ancestor rows using the target row's dependency set so
  ancestor/current-node memoization could share a trace. Built and ran
  `cold-debug/1103`/`hot-debug/1103`.
- Rejected and reverted. The counters remained essentially the same as `1102`
  (`routeAccepted=2`, `depsChecked=100834`/`65847`) and wall time stayed worse.
- Diagnosis: positive route projection is a useful routing abstraction, but it
  cannot beat the baseline while each projected container is authorized by
  replaying ordinary large dependency vectors. The missing piece is still a
  compact object-bound authority for the dependency coverage, not more route
  publication.

Current no-debug gate after rejected Lead J:

- Regenerated retained-state cache as `cold/1105` and `hot/1105`, no debug and
  no `NIX_SHOW_STATS`, on the same 10 commits as `cold/938`/`hot/938`.
- Result versus `cold/938`: `cold/1105` mean `6.52s` versus `6.06s`, so the
  retained branch is still about `1.08x` slower cold.
- Result versus `hot/938`: `hot/1105` mean `0.33s` versus `0.88s`, so the
  retained branch is materially faster hot.
- Soundness comparison passed.
- Per-commit shape: first commit is faster than baseline, exact/hot action-cache
  hits are much faster, but cross-commit cold recovered hits are still about
  `1.3s` and repeated full-eval misses are about `10.9-11.1s`. The next useful
  work needs to target cold replay/miss overhead without regressing the hot
  path.

Lead K: proof-authorized JSON/action certificate sidecar.

- Hypothesis: the benchmark's recovered cold hits are really command-level
  `closures.gnome` JSON outPath results. A first-class action certificate can be
  accepted before opening a trace session, avoiding full trace load,
  monolithic verifier replay, and materialization.
- Implemented a prototype sidecar under
  `eval-trace-json-action-proof-v1/<proof-key>/<head>.json` keyed by stable
  file-eval command inputs. The exact fast key still includes the current Git
  head; the proof key deliberately excludes it and must be authorized by proof
  material before serving.
- Initial full-dep certificate:
  - Encoded `FileBytes`, `RawBytes`, `DirectoryEntries`, `ExistenceCheck`,
    `NarIdentity`, `StorePathAvailability`, environment/session deps,
    `DerivedStorePath`, and structured projection deps.
  - Directory-format structured deps with aggregate dirsets are authorized only
    when all contributing directory listings are verified.
  - `DerivedStorePath` initially replayed dry-run store-path observations.
  - Result: certificates published and accepted, but cold proof hits were about
    `1.4s` and hot regressed until proof hits also seeded the exact fast action
    cache.
- Git-diff guard:
  - Added an object-bound guard from the certificate's path-like deps:
    exact file paths, directory-listing paths, and recursive NAR/derived paths.
    On a clean Git checkout, `git diff --name-status -z --no-renames <cert-head>
    <current-head>` rejects stale certs before hashing files.
  - Fixed an over-conservative first version that rejected on ancestor directory
    deps; directory listing guards now reject only direct non-`M` changes.
  - Omitted path-like deps from the persisted dep list once the Git guard is
    present. The cert keeps only non-path deps plus compact store-path
    availability.
  - Result: recovered cold hits dropped to roughly `0.38-0.40s`; hot remained
    about `0.33s`.
- Publication/rejection optimizations attempted:
  - Compact store path availability arrays: neutral.
  - Build Git guard directly from `Dep` objects instead of encoding all path
    deps to JSON first: best run improved to `cold/1124` mean `6.38s`.
  - Newest-mtime-first certificate lookup: neutral/noisy.
  - Pass in-memory outPath projection deps to the sidecar writer to avoid
    `queryEvalInfo()` reloading the just-recorded trace: neutral/noisy.
  - Split a `.guard` file to reject stale certs before parsing the full cert:
    neutral/noisy; the guard is still about `387K`.
  - Newline-encoded store-path availability text: neutral/noisy.
- Best measured no-debug 10-commit run so far:
  - `cold/1124` mean `6.38s`, min `0.39s`, max `14.81s`.
  - `hot/1124` mean `0.33s`.
  - Per-commit cold: proof hits are `~0.39-0.40s`; full-eval misses are still
    `~11.65-11.85s`; first commit is `~14.81s`.
- Diagnosis:
  - The action-certificate abstraction is useful. It removes verifier replay and
    materialization from recovered cold hits and gets them below the old hot
    baseline.
  - It does not yet beat the stated cold baseline because misses dominate the
    10-commit mean. Baseline miss commits were about `10.0s`; retained current
    misses are about `10.9-11.1s`; the sidecar prototype still leaves misses
    around `11.6-11.9s` after publication/rejection overhead.
  - The design is not fundamentally incapable: if fallback misses returned to
    retained timing while proof hits stayed at `~0.39s`, the 10-commit mean would
    be around the `6.0s` target. The remaining work is miss-path overhead, not
    recovered-hit replay.
- Next viable leads:
  - Move sidecar publication out of the measured fallback path or make it part
    of the recorder without an extra pass over deps. The current prototype still
    builds and writes a large proof object synchronously.
  - Attack the retained miss regression directly. Even perfect proof hits cannot
    beat `cold/938` if every changed-output commit remains `~1s` slower than the
    baseline.
  - Consider a smaller action certificate format: path guard plus a compact
    availability digest/proof, not thousands of store-path strings.
  - Revisit route/shape projection only after the same compact authority can be
    reused there; route replay with ordinary dep vectors was already rejected in
    Lead J.

## 2026-05-23 late pass: sidecar soundness and projection miss accounting

Context:

- The current retained prototype is still benchmark-first. The goal remains to
  beat `cold/938` (`~6.06s`) and `hot/938` (`~0.88s`) on the 10-commit
  no-debug/no-`NIX_SHOW_STATS` gate, not just to be faster than no cache.
- The hot-path improvement is real and recorded: retained safe runs around
  `1198`/`1199` measured `hot` at about `0.33s`, materially faster than the
  `0.88s` baseline.
- The cold path is still not there: retained safe runs around `1198`/`1199`
  measured `cold` at about `6.47-6.49s`, slower than `cold/938`.

Parallel review findings folded into the retained tree:

- Proof JSON action sidecars must be written only from complete proof deps for
  the exact JSON bytes being served. Writing them from recursive fallback JSON
  rendering or from a backend query of a nearby value path can certify an
  incomplete dependency set. The fallback publisher remains disabled.
- Proof hits must not seed the exact fast action cache when dynamic deps are
  involved. The retained path returns proof hits directly and does not populate
  the fast cache from them.
- Old action-certificate artifacts are not trusted as current proof material:
  the proof key/cache namespace was bumped to `v2`, and certs now require
  `version = 2`. This prevents stale unsafe `v1` prototype certs from
  contaminating measurements.
- Git/history exact-session proof for history bootstraps was tried and rejected.
  Run `1202` looked attractive (`cold` about `3.35s`, `hot` about `0.33s`) but
  failed reference comparison on commit `7fb36e13811a`. Root cause: history rows
  are selected by stable attr/policy identity, not current source revision, and
  the existing exact-session Git identity cache-miss rule is safe only for
  current-node exact-session hits.

Projection miss accounting:

- Added cheap counters under `evalTrace.projection.outPathPublish` to split the
  JSON/outPath projection fallback into navigation, target capture, child
  capture, payload, JSON dump, record, child count, and dep count.
- Debug stats showed the slow cold miss path is dominated by child capture, not
  SQLite writeback or JSON dumping. Representative commits spent about
  `12.9-16.6s` in `childCaptureUs` for two child derivations and about `63k`
  deps, while `recordUs` was only about `0.11s`.
- Re-enabling JSON projection proof publication when deps were present improved
  some recovered cold hits to about `0.38-0.40s`, but it added roughly a second
  or more to changed-output misses by doing an extra pass over the large dep
  vector. Net `cold` got worse (`~6.61-6.67s`), so that publication path is
  disabled again.
- Splitting the action certificate into `.guard` and `.store-paths` sidecars did
  not fix the cold miss regression. The costly part was constructing/encoding
  the large proof material, not parsing the cert during accepted hits.

Current interpretation:

- The abstractions are still useful as routing and authority boundaries:
  positive projection routes are the right shape for lazy navigation, and action
  certificates are the right shape for command-level JSON replay.
- They are not enough by themselves. If a projection route or sidecar publisher
  still authorizes by replaying or rebuilding the same large dependency vector,
  it cannot beat the old cold baseline.
- The design is not fundamentally incapable: the accepted proof-hit path is
  already much faster than the old hot baseline. The blocker is changed-output
  cold misses and proof publication overhead.

Next leads worth pursuing:

- Move action-proof construction into the existing recorder/capture pipeline so
  publication does not make a second large pass over deps on the measured miss
  path.
- Split `childCaptureUs` further into force time, cached-result materialization,
  direct dep capture, dep merge/dedup, and child cached-result build. The
  current counter proves where the time is, but not which sub-operation is the
  lever.
- Find or persist a compact object-bound source/action coverage certificate for
  history hits. Current persisted facts are routing facts and hashes; they are
  not enough to authorize history bootstrap without replay.
- Keep route/shape projection shelved until it can reuse the same compact
  authority. Lead J showed ordinary route replay increases verifier work.

## 2026-05-24 source-identity guard role and proof-publication checkpoint

Parallel review result:

- The async verifier had already been fixed to match the sync verifier:
  history rows with source-identity deps must not be directly full-verified as
  current. They go through recovery unless an object-bound proof covers them.
- The action-certificate `.guard` sidecar needed to be bound to the JSON cert.
  The retained format now stores `gitGuardRef = { kind, size, sha256 }` and
  verifies those bytes before allowing `pathDepsOmitted`.
- The action cert and key now bind `schemaEpoch`, `providerEpoch`, and the
  eval-trace hash algorithm. The proof cache namespace was bumped to `v3`.

Git identity correction:

- Treating ordinary `GitRevisionIdentity` as an ignorable guard was unsafe. A
  conservative exact-check build (`cold/1203`, `hot/1203`) proved the point:
  `hot` stayed fast at about `0.33s`, but `cold` fell to about `11.22s` because
  cross-revision proof hits were no longer authorized.
- The current prototype adds an explicit action-certificate role:
  `file-eval-root-source-identity`. This role is accepted only when the
  cert-bound Git path guard has already accepted the observed path set.
  Ordinary `gitRevisionIdentity` entries in `deps` still require exact current
  Git identity equality.

Publication re-enabled:

- Re-enabled proof publication from the fresh JSON projection branch, but only
  with exact projection deps and the `v3` cert format.
- `cold/1205` restored recovered proof hits at about `0.37-0.38s`, with
  `hot/1205` about `0.36s`, but cold mean was still `6.57s` versus
  `cold/938` at `6.06s`. Changed-output miss commits were about `12.0-12.2s`.
- Added exact local `(Dep::Key, DepHashValue)` dedup before recording/publishing
  JSON projection deps. This reduced duplicated inherited deps in the action
  cert path:
  - representative source guards: `3 -> 1`
  - retained env deps: `8 -> 4`
  - store-path sidecar lines: about `13.1k -> 12.2k`
  - store-path sidecar bytes: about `768K -> 716K`
- After dedup, `cold/1206` improved to about `6.26s`; `hot/1206` remained
  about `0.36s`. Outputs matched the selected reference window. This still does
  not beat `cold/938`.

Current hard blocker:

- Debug run `cold-debug/1125` showed the fresh miss path is dominated by actual
  traced child derivation forcing: `forceUs` was effectively all of
  `childCaptureUs` (`~12.6-16.2s` for two children). `depFinalizeUs`,
  `depMergeUs`, `buildCachedUs`, `jsonDumpUs`, and `recordUs` were small.
- With proof publication enabled, the remaining cold gap is mostly synchronous
  proof publication on changed-output misses. The proof-hit side is already
  faster than the baseline; the miss side is still too expensive.

Next implementation candidates:

- Integrate action-proof material construction into the existing record/capture
  pipeline so the miss path does not do a separate large proof-publication pass.
- Persist a compact object-bound source/action coverage certificate from the
  recorder, not from command-level post-processing. Route/action indexes should
  select candidates; acceptance still needs certificate-bound guard bytes and
  exact dynamic dep checks.
- Keep the `file-eval-root-source-identity` role narrow. If future code records
  semantic Git revision observations, those must not be encoded as root-source
  guards.

## 2026-05-24 projection/action-certificate checkpoint

- Soundness fix retained: JSON action proof sidecars must be written only from
  the exact dependency vector captured for the rendered JSON bytes. Fallback
  recursive JSON rendering and backend re-query publication were rejected
  because they can certify incomplete dependency sets. Proof hits also must not
  seed the exact fast action cache when their dependency set contains dynamic
  observations.
- Safe retained runs after disabling unsafe JSON projection sidecar writes were
  roughly `cold/1198 = 6.49s`, `cold/1199 = 6.47s`, and `hot = 0.33s` over the
  10-commit gate. Outputs matched in the same-run comparison, but cold still did
  not beat `cold/938 = 6.06s`.
- Debug counters showed the miss path is dominated by outPath projection child
  capture, not SQLite record/writeback: on the slow commits,
  `projection.outPathPublish.childCaptureUs` was about `12.9-16.6s` for only
  two children and about `63k` merged deps; `record.timeUs` was only about
  `0.11s`.
- Re-enabling JSON projection proof sidecar writes only when in-memory deps were
  available improved recovered cold proof hits to about `0.38-0.40s`, but
  regressed the slow miss commits by about `1s` because the sidecar construction
  still walked/encoded the large dependency vector. Net result was worse:
  `cold/1200 = 6.61s`; the `.guard`/`.store-paths` split remained worse at
  `cold/1201 = 6.67s`.
- Temporarily enabling exact-session proof for history bootstraps looked fast
  (`cold/1202` around `3.35s`) but failed reference comparison on
  `7fb36e13811a`. Root cause: `gitIdentityMatchesCurrentExactSession()` treats
  missing current Git identity cache entries as acceptable for current-node
  exact-session hits, but history-bootstrap rows are selected by stable
  attr/policy identity and cannot inherit that rule. This experiment was
  reverted and must not be restored without a separate path-bound current-source
  guard.
- Added a follow-up diagnostic split under
  `evalTrace.projection.outPathPublish`: `forceUs`, `depFinalizeUs`,
  `depMergeUs`, and `buildCachedUs`. The next debug run should use these to
  decide whether the child-capture bottleneck is real evaluator forcing or
  post-force dependency/cached-result materialization.

## 2026-05-24 authorized projection benchmark checkpoint

Parallel review / adversarial review:

- Spawned parallel reviewers against the current dirty branch, not `master`.
  The useful implementation leads were: keep the positive proof-authorized
  action/projection hit path, reject fragment-row publication until it has
  compact authority, tighten Git/path guard soundness, and look for repeated
  aggregate-proof work on changed-output misses.
- The design is not fundamentally incapable of beating the SQLite pre-schema
  eval-cache baseline. The proof-authorized JSON action path has now beaten the
  hot baseline by more than 2x; the remaining question was whether changed-output
  cold misses could be made cheap enough.

Attempted / rejected:

- Child `drv-output.out` fragment rows with full child deps were tried and
  rejected. They made cold performance much worse because fragment rows carried
  tens of thousands of deps and triggered expensive recovery without useful
  accepted hits.
- Recovery-backed child projection probes were disabled. They did expensive
  trace recovery before the normal force path and hid whether the projection
  authority was actually helping.
- Building persistent exact descriptors for very large dependency vectors was
  bounded to avoid spending miss time on descriptor work that the prototype does
  not yet use for serving.
- Prefix-caching symlink checks in the Git guard was tried as a possible
  `depEncodeUs` optimization. Short debug run `1230` showed it was not the main
  bottleneck: proof dep encoding stayed around `0.50s` per changed-output miss.

Retained fixes:

- Directory-listing Git guards were too strict. A directory-listing observation
  is about the direct entry names/types of that directory; modifying a descendant
  file under an existing direct entry should not invalidate the listing. Exact
  file/structured deps still reject the descendant if it was actually read. This
  restored fast cold hits on commits 3, 5, 7, 8, and 9.
- Aggregated structured directory-set deps were expanding the same 1,042-entry
  directory set repeatedly while building the action proof guard. Caching accepted
  dir-set ids per certificate cut proof `depEncodeUs` from about `0.50s` to
  about `0.04s` per changed-output miss in debug run `1231`.

Benchmark results:

- `cold/1229` after the directory-listing guard fix but before dir-set caching:
  mean wall `6.214790s`; outputs matched.
- `hot/1229`: mean wall `0.410222s`; outputs matched.
- `cold-debug/1230` with only symlink-prefix caching: proof dep encoding stayed
  around `0.49-0.51s`; not a useful lead by itself.
- `cold-debug/1231` after dir-set caching: proof dep encoding dropped to
  `0.040-0.043s` on the first two changed-output misses.
- `cold/1232` no debug/no stats, 10 commits: mean wall `5.974480s`, beating
  `cold/938 = 6.058960s`. Outputs matched.
- `hot/1232` no debug/no stats, 10 commits: mean wall `0.389435s`, beating
  `hot/938 = 0.880468s`. Outputs matched.
- Exact-code rerun `cold/1234` after the conservative symlink-cache cleanup:
  mean wall `5.998170s`, still beating `cold/938 = 6.058960s`. Outputs
  matched.
- Exact-code rerun `hot/1234`: mean wall `0.390135s`, still beating
  `hot/938 = 0.880468s`. Outputs matched.
- Run `cold/1233` was a useful variance warning: one first-cold-miss outlier
  (`17.27s` on `7d7616894ec4`) pushed the mean to `6.326670s` even though the
  other commits followed the expected pattern. The retained conclusion is that
  hot is decisively better, while cold now beats the 10-commit baseline but with
  a narrow margin because the first changed-output force is still noisy.

Current interpretation:

- The useful abstraction is the proof-authorized action/projection certificate,
  not route-shape metadata by itself. Route and persisted rows are candidate
  routers; the cert-bound Git guard and exact dynamic deps are the authority.
- Positive lazy navigation is still a sound shape for future work, but this
  benchmark was won by proving and serving the command-level JSON/outPath action
  directly. Serving negative membership, suggestions, or `attrNames` from cached
  shape remains out of scope until complete keyset authorization exists.
- The remaining cold cost is real derivation forcing on changed-output misses
  (`forceUs` dominates), not SQLite writeback or JSON rendering. Further gains
  need either recorder-native compact certificates or avoiding one of the two
  child derivation forces, not more post-hoc sidecar encoding.

Next leads:

- Replace the prototype command-level post-processing publisher with
  recorder-native construction of the compact action certificate, preserving the
  exact dependency set while avoiding another pass over large proof vectors.
- Investigate whether the `outPath` projection can be captured as a first-class
  object/value certificate during derivation forcing, instead of forcing and
  materializing both child derivations as full cached attrsets.
- Keep fragment row publication off until child fragments have compact,
  object-bound authority and a benchmark proves they reduce changed-output miss
  cost rather than adding recovery work.

## 2026-05-24 second parallel review / continuation checkpoint

User request: run another parallel design-space/adversarial review against the
current dirty branch, then adjust and continue.

Parallel reviewer findings:

- Soundness review: the proof JSON action sidecar is still too standalone. The
  current hot path accepts a local certificate after the sibling Git guard and
  dynamic deps validate, then returns `cert["json"]`. That is acceptable as a
  benchmark prototype under a self-generated local cache assumption, but it is
  not the final proof-authority shape: a forged or corrupted local sidecar can
  still make mutable cache bytes into output authority. The final design must
  bind the action cert to an immutable trace/projection result id/hash, or build
  the cert at the recorder point that already observed the outPath facts.
- Design-space review: the best cold lead is not general route projection. It
  is a recorder-native compact outPath/action certificate, built before the
  command-level post-processing loop has to force both child derivations as full
  cached attrsets. Child `drv-output.out` fragments with full deps remain a
  trap: they publish too much dependency material and do not establish a cheap
  authority path.
- Benchmark audit: the 10-commit hot improvement is real, but it measures the
  proof action sidecar path, not route-projection acceptance. The cold win is
  narrow and noisy. The retained comparison is against the first-10 slice of
  `cold/938`/`hot/938`, not the full 100-commit `938` aggregate.

Adjustment made:

- Added benchmark-subject provenance to generated run manifests: resolved Nix
  binary path, Nix binary SHA-256, Nix checkout HEAD, dirty flag, and dirty diff
  SHA-256. The source repository provenance records path/head/dirty state but
  deliberately does not hash the nixpkgs diff; the benchmark checkout can be
  large and is already represented by the commit window. This closes the
  provenance gap that made exact dirty-tree comparisons ambiguous.

100-commit benchmark attempt:

- Started no-debug/no-stats run `1235` for 100 commits. The cold phase
  completed and the hot phase reached commit 23 before Git failed in
  `/home/connorbaker/nixpkgs` with `No space left on device` while creating
  `.git/index.lock`.
- The partial `1235` artifacts were removed to recover space. Older debug
  artifacts `cold-debug/1230`, `hot-debug/1230`, `cold-debug/1231`, and
  `hot-debug/1231` were also removed; their key observations are preserved
  above, but the raw artifacts are gone.
- `/home/connorbaker/nixpkgs` is now in a partial benchmark checkout state with
  thousands of changed paths. Do not run another benchmark until that checkout
  is deliberately restored and more disk headroom is available.

Current decision:

- Keep the action sidecar only as a performance prototype, not a final
  soundness claim. The next implementation change should make the sidecar an
  accelerator for an already-authorized trace/projection result, or move compact
  certificate construction into the recorder/outPath observation point.
- Do not spend more time on broad shape routes, negative membership, attrNames,
  or suggestions until the positive outPath/action certificate has immutable
  authority and the 100-commit benchmark can be rerun with provenance.

## 2026-05-24 fresh baseline run 0

The deleted benchmark results were regenerated from scratch, with run `0` as
the new baseline reference for this branch.

Setup/restoration:

- Restored `/home/connorbaker/nixpkgs` to clean `master`.
- Built the baseline Nix with:
  `nix build -L --builders '' ".?rev=92a3df1ab706cf514357ae57b367050b373ff146"`.
- The `result` output is
  `/nix/store/x43bp7y47aqaj6h354mrqbsfnr61iyca-nix-2.35.0pre20260509_92a3df1`;
  `result/bin/nix` resolves to
  `/nix/store/3j3qgg7cdi2qfx10n62ycl3qrn16ci7l-nix-2.35.0pre20260509_92a3df1/bin/nix`.
- Binary SHA-256 recorded in the manifest:
  `1f301376e2c54718a8ae98c89b55371bb68e72b6f978d9cbf7871e3e88d54051`.

Command:

- `env -u NIX_SHOW_STATS -u NIX_SHOW_STATS_PATH nix run .#eval-trace-bench -- generate --num-commits 100 --run-number 0`

100-commit results:

- `reference`: mean wall `6.006794s`, median `5.948058s`, max `10.026224s`.
- `cold/0`: mean wall `3.316277s`, median `0.938353s`, max `13.428621s`.
- `hot/0`: mean wall `0.886366s`, median `0.885450s`, max `0.932551s`.
- `eval-trace-bench runs --runs reference,cold/0,hot/0 --reference reference`
  reports all outputs match reference.

First-10 slice from the same run, for comparison with the old deleted
10-commit baseline:

- `reference`: mean wall `6.383973s`.
- `cold/0`: mean wall `5.863616s`.
- `hot/0`: mean wall `0.891211s`.

Harness fixes in this checkpoint:

- `generate` now removes `_state/` trees through a chmod-then-rmtree helper, so
  read-only local-store paths do not leave the checkout stuck when a run is
  reset or restarted.
- `runs --reference reference --runs cold/0,hot/0` now includes the available
  reference run before loading the dataset, instead of silently using the first
  selected run as the reference.
- Verified with `ruff check`, `ruff format --check`, `git diff --check`,
  `nix run .#eval-trace-bench -- runs --runs cold/0,hot/0 --reference reference`,
  and `nix build -L .#checks.x86_64-linux.eval-trace-bench --no-link`.

## 2026-05-24 candidate run 1 against baseline run 0

Current-branch setup:

- The default `nix build -L --builders ''` of the full package graph failed in
  the functional-test dependency, after the stale `CachedAttrEntry` initializer
  compile error was fixed. The benchmarkable CLI package was built directly
  with `nix build -L --builders '' .#nix-cli`.
- Candidate `result/bin/nix`:
  `/nix/store/ws3l30axm9fzljjp0nqf2dszj4d3hmza-nix-2.35.0pre20260515_dirty/bin/nix`.
- Candidate binary SHA-256:
  `8f6759f02fa3033de8962d875c36f23fc9c5945787863090b109127e09d30a66`.
- `/home/connorbaker/nixpkgs` was clean on `master` before and after the run.

Command:

- `env -u NIX_SHOW_STATS -u NIX_SHOW_STATS_PATH nix run .#eval-trace-bench -- generate --num-commits 100 --run-number 1 --runs cold,hot`

100-commit results:

- `cold/0`: mean wall `3.316277s`, median `0.938353s`, max `13.428621s`.
- `cold/1`: mean wall `2.961237s`, median `0.478946s`, max `14.004789s`.
- `hot/0`: mean wall `0.886366s`, median `0.885450s`, max `0.932551s`.
- `hot/1`: mean wall `0.525684s`, median `0.520795s`, max `1.591931s`.

Comparison:

- `eval-trace-bench runs --runs cold/1,hot/1 --reference reference --baseline-run 0`
  reports all outputs match `reference`.
- Same-mode baseline comparison:
  - `cold/1` vs `cold/0`: `0.89x` mean wall time.
  - `hot/1` vs `hot/0`: `0.59x` mean wall time.

Interpretation:

- This is now a real 100-commit no-debug/no-`NIX_SHOW_STATS` comparison against
  a freshly regenerated run `0` baseline from the baseline binary.
- Hot performance is materially better than the regenerated baseline and below
  the old `0.88s` target.
- Cold performance also improves against regenerated `cold/0`, but remains
  dominated by changed-output misses around 10-11s. The median improvement is
  much larger than the mean improvement because the miss count still controls
  the tail.
- The benchmarked candidate binary store path/SHA is identical for `cold/1` and
  `hot/1`. The manifest working-tree diff hash differs because the Python
  reporting harness was patched while the long benchmark was running; that did
  not change the already-built candidate Nix binary.

Additional harness fix:

- `runs` now accepts `--baseline-run <n>`. When a selected run has the form
  `mode/k`, the report automatically includes `mode/<n>` if present and prints
  a `Performance vs run <n>` table with same-mode wall-time ratios. This makes
  run `0` usable as the performance reference while keeping the separate
  `reference` run as the soundness oracle.

Verification after this checkpoint:

- `ruff check benchmarks/eval-trace-bench/src/eval_trace_bench/subcommands/generate_cmd.py benchmarks/eval-trace-bench/src/eval_trace_bench/subcommands/runs_cmd.py benchmarks/eval-trace-bench/tests/test_semantics.py`
- `ruff format --check benchmarks/eval-trace-bench/src/eval_trace_bench/subcommands/generate_cmd.py benchmarks/eval-trace-bench/src/eval_trace_bench/subcommands/runs_cmd.py benchmarks/eval-trace-bench/tests/test_semantics.py`
- `git diff --check -- benchmarks/eval-trace-bench/src/eval_trace_bench/subcommands/generate_cmd.py benchmarks/eval-trace-bench/src/eval_trace_bench/subcommands/runs_cmd.py benchmarks/eval-trace-bench/tests/test_semantics.py src/libexpr-tests/eval-trace/traced-data/materialization/variant-roundtrip.cc eval-trace-cache-rewrite-tasks.md`
- `nix build -L .#checks.x86_64-linux.eval-trace-bench --no-link`
- `nix build -L --builders '' .#nix-expr-tests --no-link`

## 2026-05-24 child proof-action prototype runs 6-10

Purpose:

- Test whether per-child proof-action certificates for derivation `outPath`
  JSON fragments improve the proof-authorized projection path, instead of
  relying only on broad aggregate JSON action certificates.
- Keep normalized trace storage as the source of truth; the new files are only
  benchmark-prototype sidecars.

Implementation notes:

- `TraceSession::JsonOutputProjection` now carries optional child fragments
  with each child name, JSON string payload, string context, and proof deps.
- `tryServeJsonOutputProjection` can load child output fragments and can return
  newly captured child fragments when it has to force derivations.
- `InstallableAttrPath::tryServeJsonOutput` now stores child fragments from the
  JSON publishing branch. Earlier run `6` missed this and only stored aggregate
  certificates.
- `tryLoadProofJsonActionCache` now tries the exact current-`HEAD` certificate
  path before scanning sibling certificates. An attempted change in run `8`
  moved the broad aggregate scan after projection; that regressed cheap commits
  to about `1.0s`, so it was reverted before run `9`.

Important intermediate results:

- Run `6` had child-loader code but did not persist child certificates from the
  publishing branch. It produced only the aggregate proof-key directory.
  Results: `cold/6` mean `2.939364s`, `hot/6` mean `0.528548s`.
- Run `7` fixed child fragment persistence for the 10-commit gate. It produced
  three proof-key dirs and 15 cert JSON files (aggregate + two child keys).
  Results on the first 10 commits: `cold/7` mean `5.805241s`, `hot/7` mean
  `0.379681s`, outputs matched reference.
- Run `8` tested delayed broad aggregate lookup. It regressed the first 10:
  `cold/8` mean `6.096517s`, `hot/8` mean `0.681999s`. Rejected.
- Run `9` restored broad aggregate lookup before projection while keeping
  exact-current-HEAD first inside the loader. First 10: `cold/9` mean
  `5.802514s`, `hot/9` mean `0.382320s`, outputs matched reference, three
  proof-key dirs and 15 cert JSON files.

Final measured prototype in this checkpoint:

- Candidate binary:
  `/nix/store/0ii7bzy5q0a0y43jdv6zfzbxrcfvb1a6-nix-2.35.0pre20260515_dirty/bin/nix`.
- Candidate binary SHA-256:
  `2dec161707946ded30acb9d90d5d024af6186f0a0e092976c21c6d45d7b2b85f`.
- Command:
  `env -u NIX_SHOW_STATS -u NIX_SHOW_STATS_PATH nix run .#eval-trace-bench -- generate --num-commits 100 --run-number 10 --runs cold,hot`.

100-commit results:

- `cold/0`: mean `3.316277s`, median `0.938353s`, p90 `10.527905s`, max
  `13.428621s`.
- `cold/1`: mean `2.961237s`, median `0.478946s`, p90 `10.825202s`, max
  `14.004789s`.
- `cold/10`: mean `3.070835s`, median `0.481119s`, p90 `11.400538s`, max
  `14.017373s`.
- `hot/0`: mean `0.886366s`, median `0.885450s`, p90 `0.906434s`, max
  `0.932551s`.
- `hot/1`: mean `0.525684s`, median `0.520795s`, p90 `0.654142s`, max
  `1.591931s`.
- `hot/10`: mean `0.477112s`, median `0.431395s`, p90 `0.655186s`, max
  `0.783659s`.

Correctness/artifacts:

- `cold/10` and `hot/10` outputs byte-match `reference` for all 100 commits.
- `cold/10` and `hot/10` each contain three proof-key directories and 72 cert
  JSON files: 24 aggregate certs plus 24 certs for each of two child output
  keys.

Interpretation:

- This variant beats regenerated run `0` in both modes: `cold/10` is about
  `0.93x` run-0 mean wall time, and `hot/10` is about `0.54x`.
- Relative to run `1`, the tradeoff is mixed: hot improves from `0.525684s` to
  `0.477112s`, but cold regresses from `2.961237s` to `3.070835s`.
- Child proof certificates are now real and useful for hot 100-commit behavior,
  but they do not fix the expensive cold commits. Cold remains dominated by 24
  commits around `10.5-14.0s`, where derivation-output generation still happens.
- The rejected run `8` shows broad candidate scans cannot simply be delayed
  behind child projection; doing so makes cheap commits pay multiple child-dir
  scans. A better next step is an indexed proof-candidate selection strategy
  (for example, direct exact commit lookup plus a compact guard/index for likely
  cross-commit candidates) rather than more ad hoc reordering.

## 2026-05-24: Case 2 Store-Context Fallback Test

Finding:

- Local-store GC should not delete an intermediate `.drv` while a valid
  dependent `.drv` remains, because derivations record input derivations as
  store references and local-store deletion refuses paths with referrers.
- The actionable soundness case is therefore the returned context root itself:
  if the cached JSON output's top-level `.drv` / store context path is gone,
  the proof-backed JSON action certificate must not be served.

Implementation/test note:

- Added a warning path in the proof-backed JSON action loader. When a candidate
  certificate reaches the store-availability proof and an expected-valid store
  path is currently missing, the loader records that path. If no candidate is
  accepted, it warns once that the JSON action cache entry requires a missing
  store path and is falling back to evaluation.
- Tightened the context-only store proof after adversarial review. A context
  proof may omit a recorded valid store-path availability dep only when that dep
  is in the current filesystem closure of the JSON result's returned context
  roots. Missing deps, and valid deps outside that closure, remain explicit
  store-path proof facts.
- The verifier now enforces the certificate's `storePathProofScope`, so toggling
  between context-only and full store proof cannot silently accept a certificate
  written under the other scope.
- Added `tests/functional/eval-trace-json-action-store-proof.sh`. The test
  uses file-based eval inside a clean Git repo, seeds a proof-backed JSON action
  certificate for a derivation JSON result, deletes the returned `.drv`, then
  asserts that the next eval warns, falls back to fresh evaluation, re-records,
  mentions the deleted `.drv`, and recreates the deterministic `.drv`.

Causal expectation:

- This warning does not change the successful hot path: accepted certificates
  still return before warning emission.
- The rejected case is specific to context-store proof mismatch, so routine
  source mismatches and unsupported proof candidates remain quiet debug misses.

Parallel adversarial review follow-up:

- Removed the standalone `generation-segment-log` files from the staged set.
  They duplicated segment-log declarations that now live in `generation-store`
  and would conflict if the standalone header were ever included.
- `NIX_ALLOW_EVAL` is now ignored in the proof-backed JSON action routing env
  scope, like `SHLVL` and `_`. This is safe only because accepted certificates
  recheck explicit `EnvironmentLookup` deps; the helper is named/commented as a
  proof-routing scope and must not be reused for unproven action-cache hits.
- Direct exact trace/result serving still lacks a first-class returned-context
  store-availability obligation. The JSON action layer handles the command
  output case locally, but the longer-term proof descriptor should expose
  structured store-context failure so shared serving paths can warn/fallback
  consistently.
- The strongest next performance lead after this soundness pass is an exact-HEAD
  fast JSON action lane with returned-context validity checking, followed by a
  clean 100-commit no-debug/no-stats benchmark of the current default. The
  broader positive route projection should stay secondary until it has a compact
  authority row to accept.

Validation run after the soundness pass:

- Built `.#nix-cli` successfully after the context-closure proof tightening.
- `tests/functional/eval-trace-json-action-store-proof.sh` passed against the
  rebuilt CLI.
- Ran `cold/25` and `hot/25` over 100 commits, no debug and no `NIX_SHOW_STATS`.
  All outputs matched `reference`.
- Versus refreshed run `0`: `cold/25` mean wall `3.132s` vs `cold/0` `3.316s`
  (`0.94x`), and `hot/25` mean wall `0.329s` vs `hot/0` `0.886s` (`0.37x`).
- Artifact shape stayed at the intended aggregate-plus-alias default:
  `24` cert JSONs, `76` aliases, `24` git guards, and `24` store-path sidecars
  in each of `cold/25` and `hot/25`.
- Interpretation: the closure-filtered context proof preserves the benchmark
  win while closing the unsound "skip all store availability deps" shortcut. The
  cold tail is still changed-output/first-time derivation-output forcing, not
  proof sidecar routing.

Debug split after run `25`:

- Ran `cold-debug/26` and `hot-debug/26` for the first 10 commits with
  `NIX_SHOW_STATS`.
- `hot-debug/26` had `10/10` proof JSON action hits, `10` candidate checks, and
  no recording or full verifier work. Total proof-action load time was
  `43.982ms` across all 10 commits: `30.509ms` guard verification,
  `12.596ms` cert/store-path verification, and `9.860ms` store-path query time.
- `cold-debug/26` had `5` proof hits and `5` misses/stores. The misses spent
  `69.515s` in `projection.outPathPublish.totalUs`, with `68.379s` in
  `forceUs` and `68.588s` in `childCaptureUs`.
- Causal read: an exact-HEAD fast action cache that only bypasses proof-action
  guard verification can at best remove a few milliseconds per 10 hot commits.
  It is not the cold lever. The cold tail is still the cost of computing child
  derivation `outPath`s for changed outputs.
- The next benchmark question is whether child fragments help after the
  context-store proof tightening. Earlier ablations said no, but they predated
  the smaller context-store sidecars; rerun the ablation before spending more
  implementation time on a direct child-outPath capture path.

Child-fragment ablation after run `25`:

- Ran `cold/27` and `hot/27` over the same 100 commits, no debug and no
  `NIX_SHOW_STATS`, with
  `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_CHILD_FRAGMENTS=1`.
- All outputs matched `reference`.
- Versus refreshed run `0`: `cold/27` mean wall `3.265s` vs `cold/0`
  `3.316s` (`0.98x`), and `hot/27` mean wall `0.326s` vs `hot/0` `0.886s`
  (`0.37x`).
- Versus the default aggregate-only proof-action run `25`: cold regressed from
  `3.132s` to `3.265s`, while hot was effectively unchanged (`0.329s` to
  `0.326s`).
- Artifact shape: `cold/27` and `hot/27` each wrote `72` cert JSONs, `76`
  aliases, `72` git guards, and `72` store-path sidecars. That is 3x the
  cert/guard/store-path sidecar count of run `25` for no cold improvement.
- Causal read: smaller context-store sidecars did not make child fragments
  useful. The slow cold commits still spent about `11.4-14.8s` on changed
  outputs, matching the force-dominated `childCaptureUs` profile from
  `cold-debug/26`. This rejects child-fragment sidecar publication as the next
  default path.
- Next direction: stop adding more per-child proof-action files. The remaining
  cold problem is first-time derivation-output forcing for changed aggregate
  outputs. The next useful investigation is why the existing child fragments do
  not get reused on changed aggregate outputs: distinguish "no reusable child
  fragment exists", "candidate routing misses", "proof rejects", and "the
  forced derivation path is not actually stable across the changed commits".

Child-fragment debug follow-up:

- Ran `cold-debug/28` and `hot-debug/28` for the first 10 commits with child
  fragments enabled.
- `cold-debug/28` reproduced the slow pattern: commits 1, 2, 4, 6, and 10 were
  slow even though commits 1, 2, 3, and 4 printed identical JSON output.
- Aggregated counters: `20` proof-load attempts, `5` hits, `15` misses, `38`
  candidate checks, `5` guard accepts, and `33` guard rejects. There were `15`
  proof stores: aggregate plus two child certificates for each of the five slow
  commits.
- The slow commits still spent `69.010s` in `childCaptureUs`, almost entirely
  `forceUs` (`68.961s`). Target capture was only `4.3ms`.
- Per slow changed-output commit, all three load attempts were guard-rejected
  before certificate parsing or store-path checks. Example `3f7b5d89ca3e`:
  `3` attempts, `3` candidate checks, `3` guard rejects, `0` hits, then `3`
  stores.
- The stored "child" guards are not narrow: the first child guard listed about
  `3,980` source paths, the second about `3,981`, and the aggregate guard about
  `3,985`.
- Source-diff check: `7d7616894ec4..3f7b5d89ca3e` changed only two files,
  including `pkgs/top-level/aliases.nix`; `53443ee9cdff..1575c9f64e00` changed
  seven files, including `pkgs/top-level/python-aliases.nix` and
  `pkgs/top-level/python-packages.nix`. Those top-level files are present in
  both child guards and the aggregate guard.
- Across `cold/27`, one output JSON hash appeared `80` times, but `13` of those
  entries were still slow. That is not a candidate-index failure; it is proof
  invalidation by broad source observations that happen not to change this
  requested output.
- Causal read: child fragments cannot help unless their proof authority becomes
  much narrower than the aggregate proof. The current implementation captures
  child derivation `outPath` under evaluation that already depends on broad
  nixpkgs top-level files, so the child guard rejects on the same commits as the
  aggregate guard. Serving through those changes without evaluation would need
  a new semantic/source proof; blindly ignoring changed imported files would be
  unsound because syntax/errors and top-level definitions can affect evaluation
  even when this benchmark's output string happens to remain unchanged.

Structural action-proof experiment:

- Hypothesis: the command-level JSON action proof was more conservative than
  the normal trace verifier because it collapsed `FileBytes` and
  `StructuredProjection` into a single Git guard. If the action certificate
  retained Nix `StructuredProjection` deps and moved structurally-covered source
  files out of the strict exact guard, changed-but-equivalent package files
  might be accepted without forcing child derivation `outPath`s.
- Implemented this as a gated v4 prototype, enabled only with
  `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_STRUCTURAL_PROOF=1`. Default certificates
  remain v3 in `eval-trace-json-action-proof-v3`; the v4 path uses separate
  proof keys, a v3 git guard frame, and `sibling-v3` guard refs.
- Debug run `cold-debug/29` / `hot-debug/29` tested v4 on the first 10 commits.
  Outputs matched, but the slow cold commits were unchanged:
  `cold-debug/29` mean `7.833s`, median `0.425s`, max `17.751s`;
  `hot-debug/29` mean `0.378s`.
- Counter read for `cold-debug/29`: `10` proof-load attempts, `5` hits,
  `5` misses, `18` candidate checks, `13` guard rejects, `338.957ms` proof
  load time, and `68.440s` of `childCaptureUs`. Store publication retained
  `186,885` proof deps across the five stores and spent `1.106s` encoding
  retained deps.
- Counter read for `hot-debug/29`: `10/10` proof hits and `10` candidate
  checks, but proof-load verification rose to `242.964ms` because each v4 cert
  retained `9,553` structured deps.
- A representative v4 guard had `776` exact paths, `3,209` structural exact
  paths, `1,042` directory-listing paths, and `3,625` recursive paths. It did
  structurally cover `pkgs/top-level/python-packages.nix`, but still kept
  `pkgs/top-level/aliases.nix` and `pkgs/top-level/python-aliases.nix` in the
  strict exact set and `pkgs/by-name/an` in the directory-listing set.
- The causal blocker is visible in the source diffs. For example,
  `7d7616894ec4..3f7b5d89ca3e` changed/removes
  `pkgs/top-level/aliases.nix` and renames a package under `pkgs/by-name/an`;
  `53443ee9cdff..1575c9f64e00` changes `python-aliases.nix`,
  `python-packages.nix`, and several package files. The output JSON hash was
  identical across these commits, but the proof correctly rejected because the
  current certificate cannot prove negative membership/keyset stability through
  alias attrsets and by-name directory listings.
- Adversarial semantic read: accepting these changes would require more than
  positive route projection. Alias files and directory listings participate in
  merge/member lookup semantics, so additions/removals of unused names are safe
  only if the proof records the relevant positive and negative membership facts
  or a complete authorized keyset. The current prototype intentionally avoids
  cached absence, so treating these files as harmless would be the soundness hole
  the design was trying to avoid.
- Rebuilt and reran default v3 as `cold-debug/30` / `hot-debug/30` after gating
  the v4 path. The default returned to the validated shape:
  `cold-debug/30` mean `7.812s`, median `0.354s`, max `18.020s`;
  `hot-debug/30` mean `0.341s`, with proof-load verification back down to
  `12.637ms` across all 10 commits. The v4 path stays gated as an experiment,
  not a default direction.
- Added debug-only guard rejection diagnostics after doing this read. Future
  debug runs now report whether a proof-action guard rejected because of an
  exact source path, directory-listing path, or recursive path, and include the
  changed path. This is instrumentation for design search only; it does not
  change acceptance semantics or the no-debug hot path.
- Validation after the diagnostic patch: `git diff --check` passed,
  `nix build -L --builders '' .#nix-cli` produced
  `/nix/store/yfnfmjsv4glhf7bwp5wipcnqsixni34j-nix-2.35.0pre20260515_dirty`,
  and `tests/functional/eval-trace-json-action-store-proof.sh` passed against
  that CLI.

Decision after the structural experiment:

- Do not pursue more command-action structural proof unless the next design
  explicitly records authorized negative membership / keyset facts for Nix
  attrsets and directory-backed package sets. Without that, the implementation
  can only clear some `FileBytes` false positives while the real alias/by-name
  blockers remain.
- The useful abstractions are still the authority split and the projection
  router: cached routes are routing material; proof acceptance is separate and
  fail-closed. The unhelpful part was trying to retrofit a broad source guard
  into a precise attrset/keyset proof without recording the missing semantic
  facts.
- Near-term implementation should either stay with the aggregate v3 JSON action
  proof, which already beats the regenerated baseline, or start a new proof
  slice for keyset/membership authority. It should not add more child sidecars
  or structural dep retention to the current v3 hot path.

## 2026-05-24 gated directory-structured proof probe

New experiment under the already-gated v4 structural action proof:

- Motivation: `cold-debug/29` showed one remaining cold-tail guard class is
  directory-listing invalidation, especially `pkgs/by-name/an`. The existing
  trace dependency model already records structured directory facts such as
  `hasKey`, `keys`, `type`, and child entry type. The command-action proof was
  collapsing those facts into broad directory-listing guards, which means a
  rename/addition of an unrelated by-name package invalidates every cached
  output that observed the directory listing.
- Change under test: for v4 only, retain directory `StructuredProjection`
  proof deps next to Nix structured deps, and on load recompute their current
  hash from the source accessor. Supported facts are deliberately narrow:
  single directory `hasKey`, child entry type, root `keys`, root `type`, and
  multi-directory root `hasKey`. Unsupported shapes fail closed.
- Semantic risk: unlike the original positive-route prototype, directory
  `hasKey=false` is cached absence. It is only acceptable as an experiment
  because the proof checks the exact structured hash recorded by the trace
  dependency system and remains disabled unless
  `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_STRUCTURAL_PROOF=1` is set. This must not
  become a default path unless the authority story for negative membership is
  made explicit and tested.
- Expected causal effect if the hypothesis is right: exact/directory-listing
  guard rejects for by-name directory changes should disappear or move later to
  alias/keyset rejects. Cold tail should improve only for commits whose blocker
  was directory membership; hot may regress because v4 retains and verifies more
  structured deps. If alias files remain the dominant blocker, this experiment
  should be rejected as too narrow.
- Build status: `git diff --check` passed and
  `nix build -L --builders '' .#nix-cli` passed with result
  `/nix/store/7wzpigc9ah897x9k9hg445032yr1v908-nix-2.35.0pre20260515_dirty`.

Probe results:

- `cold-debug/31` / `hot-debug/31` tested the first implementation. It was
  wrong architecturally, not semantically: certificates hit, but hot replay
  spent about `11.2s` per commit in proof verification because every retained
  directory structured dep was recomputed unconditionally. The v4 cert shape was
  `13,333` JSON deps: `9,553` Nix structured deps plus `3,776` directory
  structured deps and `4` env deps. `hot-debug/31` mean wall time was
  `11.968s`, so this version was rejected immediately.
- Refinement: reuse the existing changed-path filter for structured deps.
  Added an empty-diff fast path for Nix structured deps and a
  directory-listing-aware changed check for directory structured deps. Directory
  facts now recompute only when the Git diff can affect the direct listing of
  the observed directory / directory set. Same-head hot replay skips the
  directory reads entirely.
- `cold-debug/32` / `hot-debug/32` after the filter restored correctness and
  removed the catastrophic hot regression, but it still did not beat default
  v3. Output JSON matched `reference` for all 10 commits.

Run comparison for the first 10 debug commits:

- Default v3 `cold-debug/30`: mean `7.812s`, median `7.453s`, max `18.020s`;
  `5` proof hits, `5` misses, `13` guard rejects, proof verify `6.565ms`,
  parse `0.153ms`.
- Directory-structured v4 `cold-debug/32`: mean `8.606s`, median `8.151s`,
  max `18.691s`; still `5` proof hits, `5` misses, `13` guard rejects, proof
  verify `3.267s`, parse `1.410s`.
- Default v3 `hot-debug/30`: mean `0.341s`; proof verify `12.637ms`, parse
  `0.335ms`.
- Directory-structured v4 `hot-debug/32`: mean `1.068s`; proof verify
  `3.809s`, parse `3.088s`.

Causal read:

- Directory structured retention did not increase the 10-commit hit count. It
  mostly changed the cost of accepted hits.
- The remaining directory-listing guard still contains `.` because some root
  `DirectoryEntries` observations are still represented only by the broad Git
  guard, not by retained structured directory facts. Moving `DirectoryEntries`
  out of the guard might remove that false positive, but the slow commits also
  have stronger blockers: `pkgs/top-level/aliases.nix` and
  `pkgs/top-level/python-aliases.nix` remain exact guarded, and later commits
  reject on recursive doc/module observations. Clearing root directory listing
  alone would not change the miss count for the important alias commits.
- The experiment is therefore useful as evidence but not as a keeper in its
  current form. Directory/keyset authority needs a compact representation and a
  proven increase in hits before it can justify the cert-size and parse/verify
  cost. Do not enable v4 directory retention by default.

Follow-up reject replay:

- The debug log reports only the first guard failure per candidate, so I decoded
  the v4 `.json.guard` sidecars and replayed the Git diffs for the slow
  `cold-debug/32` commits to list all failing guard classes.
- `3f7b5d89ca3e` against the prior `7d7616894ec4` certificate fails on the
  broad root directory-listing guard for the by-name rename and also on exact
  `pkgs/top-level/aliases.nix`.
- `1575c9f64e00` against `3f7b5d89ca3e` fails on the broad root directory
  guard for `pkgs/development/python-modules/av_13/default.nix` and exact
  `pkgs/top-level/python-aliases.nix`.
- `fd3fbe0cfd62` against `1575c9f64e00` fails on recursive
  `doc/release-notes`.
- `7fb36e13811a` against `fd3fbe0cfd62` fails on recursive
  `nixos/modules/services/networking/nm-file-secret-agent.nix`.

This splits the remaining cold tail into three independent blockers:

- Root directory-listing false positives: the guard treats any add/delete under
  `.` as changing the root listing, even when the direct root entry (`pkgs`,
  `doc`, etc.) stays a directory. A precise Git tree-entry comparison could
  reduce candidate rejections, but this is not enough by itself for the alias
  commits because exact alias files also reject.
- Alias attrset source files: `aliases.nix` and `python-aliases.nix` are exact
  guarded. They are eligible-looking Nix attrsets (`lib: self: super: ... in
  mapAliases { ... }`), but current command-action authority has no compact
  proof that the demanded top-level/package keys are absent from or unaffected
  by those alias sets. Accepting these changes requires real keyset/negative
  membership authority, not another path whitelist.
- Recursive source observations: `doc/release-notes` and the networking module
  path are recursive guards. If those come from true NAR/derived-store-path
  observations, descendant modifications really invalidate the observation.
  They need either narrower upstream recording or a different semantic proof;
  treating them as harmless in the action guard would be unsound.

Observed-key alias/keyset lead:

- I extracted the unique Nix structured binding names retained by the v4 cert
  for the first commit. There are `1,053` observed Nix binding names. The alias
  diffs that block the first two slow commits are tiny:
  `aliases.nix` removes `anonymousPro`, and `python-aliases.nix` removes
  `av_13`. Neither `anonymousPro`, `anonymous-pro-fonts`, `av_13`, nor `av`
  appears in the observed binding-name set for `closures.gnome`.
- This supports the causal hypothesis: those alias changes are irrelevant to
  the demanded output, but the current action proof cannot say that. It only
  knows the whole alias file changed.
- A plausible next prototype is a compact "observed key" proof for eligible Nix
  attrset files:
  - derive the observed key universe from retained Nix structured binding deps;
  - for each exact-guarded eligible alias file, store a scope hash plus binding
    hashes for observed keys that are present in that file;
  - on replay, reject if the file's scope hash changes, if any previously
    present observed binding changes, or if any previously absent observed key
    becomes present.
- Adversarial read: this is cached negative membership for the observed key set.
  It is not the original positive-only projection. It may be acceptable as a
  gated experiment, but it needs to be labelled as such and must not become the
  default without targeted tests. It also does not solve the directory-backed
  package-set blockers by itself; the by-name/python-module directory guards
  need analogous observed-key authority, or they will still reject the same
  commits.

Cleanup after rejection:

- Reverted the directory structured retention code. The only code kept from
  this pass is the empty-diff fast path for Nix structured deps in the gated v4
  verifier. That is semantics-preserving: if the certificate head and current
  head have no Git diff, a structured dep cannot have changed through Git source
  content, so there is no need to normalize the file path before skipping it.
- Build after cleanup: `git diff --check` passed and
  `nix build -L --builders '' .#nix-cli` passed with result
  `/nix/store/yc6f7v8phc7vdwjw7p7r1653izv4v1l7-nix-2.35.0pre20260515_dirty`.
- Validation run `cold-debug/33` / `hot-debug/33` with
  `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_STRUCTURAL_PROOF=1` matched `reference`
  outputs for all 10 commits and returned to the pre-directory shape:
  `cold-debug/33` mean `7.878s`, median `7.500s`, max `17.950s`; `5` hits,
  `5` misses, `13` guard rejects. `hot-debug/33` mean `0.378s`, `10/10` hits.
  Hot proof verify fell from `242.964ms` in run `29` to `210.520ms`, but wall
  time is effectively unchanged. This is a small cleanup, not a direction
  change.

## 2026-05-25: v7 keyset hardening validation

Build and focused checks after the mixed keyset/positive-membership hardening:

- `bash -n` for the observed-key and observed-directory functional fixtures
  passed.
- `git diff --check` for the touched implementation, test, and design-log files
  passed.
- `nix build -L --builders '' .#nix-cli` passed with result
  `/nix/store/4cly9y23rmbkq86ism3r4qq37vzvapp6-nix-2.35.0pre20260515_dirty`.
  The build still reports pre-existing unused-helper warnings in
  `installable-attr-path.cc`; they are not from the v7 guard change.
- `eval-trace-json-action-observed-directory-proof.sh` passed through the
  manual dev-shell harness. The fixture now checks both that membership-only
  by-name sibling churn still hits and that `attrNames + hasAttr` on the same
  directory rejects after an unobserved child addition.
- `eval-trace-json-action-observed-key-proof.sh` passed through the same
  harness.

10-commit no-debug/no-stats smoke:

- Command: `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1 nix run
  .#eval-trace-bench -- generate --num-commits 10 --runs cold,hot`.
- Auto-selected run `57`; all outputs matched `reference`.
- `cold/57`: mean wall `5.67s`; slow stores at `7d7616894ec4`,
  `3f7b5d89ca3e`, `fd3fbe0cfd62`, and `7fb36e13811a`.
- `hot/57`: mean wall `0.37s`.
- Artifacts: `4` JSON certs, `6` aliases, `4` guards, `4` store sidecars,
  cache size about `25M`.
- Compared against prior observed-key `cold/55` on the same 10 commits,
  v7 regressed cold mean from `4.44s` to `5.67s` while hot stayed unchanged
  (`0.37s`). The artifact delta is causal: v6 stored `3` certs / `7` aliases,
  while v7 stores `4` certs / `6` aliases.

Adversarial read of the regression:

- The extra v7 cold miss is `3f7b5d89ca3e`. The relevant diff from the first
  reusable cert includes a by-name rename under `pkgs/by-name/an` plus
  `pkgs/top-level/aliases.nix`.
- Decoding the first cert's guard shows why:
  - v6 recorded `336` observed by-name directories, including
    `pkgs/by-name/an = { analyzerHooksSupport, ant, anthy }`, so the
    anonymous-pro rename was treated as unobserved sibling churn.
  - v7 records only `3` observed by-name directories. Explicit
    `StructuredProjection #keys` deps exist for most by-name shards, so
    observed-child relaxation is suppressed for `pkgs/by-name/an`.
- This confirms both sides of the tradeoff. The v6 win was not just an
  implementation optimization; it relied on ignoring complete keyset deps for
  by-name package indexes. That is unsound in the direct mixed case now covered
  by the functional test (`attrNames` feeds the output), even though it happens
  to be output-equivalent for the benchmark's anonymous-pro churn.
- A better implementation is not "turn the v6 relaxation back on." It needs a
  proof that the complete keyset observation is only internal to constructing a
  lazy package index and that the changed child is not selected by any demanded
  path. The current flat action proof cannot express that; it lacks a
  dependency graph from keyset consumers to the final JSON/context result.

Next design implication:

- Keep v7 as the sound variant while measuring whether it still beats the
  regenerated baseline over 100 commits.
- The next performance lead is provenance precision for lazy attrset
  construction: either record by-name package indexes as per-key lazy shape
  facts instead of flat complete-keyset deps, or attach enough consumer
  provenance to complete keyset deps to prove that an unobserved child cannot
  affect the demanded output. Domain/path whitelists would recover the v6
  benchmark win, but they would not be a defensible architecture.

100-commit v7 result:

- Command: `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1 nix run
  .#eval-trace-bench -- generate --num-commits 100 --runs cold,hot`.
- Auto-selected run `58`; all outputs matched `reference`.
- Exact timing summary:
  - `cold/0`: mean `3.316277s`, median `0.939342s`, p90 `10.527905s`,
    max `13.428621s`.
  - `hot/0`: mean `0.886366s`, median `0.885558s`, p90 `0.906434s`,
    max `0.932551s`.
  - `cold/56`: mean `2.031514s`, median `0.505995s`, p90 `12.901800s`,
    max `16.373397s`.
  - `hot/56`: mean `0.380999s`, median `0.378214s`, p90 `0.427365s`,
    max `0.664130s`.
  - `cold/58`: mean `2.871764s`, median `0.482283s`, p90 `13.012589s`,
    max `15.832111s`.
  - `hot/58`: mean `0.354958s`, median `0.338234s`, p90 `0.404220s`,
    max `0.677825s`.
- Artifact count:
  - `cold/56`: `12` JSON certs, `88` aliases, `12` guards, `12` store
    sidecars, cache about `75M`.
  - `cold/58`: `19` JSON certs, `81` aliases, `19` guards, `19` store
    sidecars, cache about `118M`.
- Extra strict v7 fresh stores relative to run `56`:
  `3f7b5d89ca3e`, `e52bb036c1f4`, `d9a578f872f2`, `0a1751721b23`,
  `abed87246b62`, `e6e9864f360f`, and `aff7fbe5b1b1`.
- Decision: v7 passes the baseline gate (`cold/58` and `hot/58` both beat
  regenerated run `0`) and fixes a real mixed-keyset soundness hole, but it is
  not the final performance answer. The lost 0.84s cold mean versus run `56`
  is the value of the next precision problem.

Cleanup after v7 validation:

- Removed dead inline JSON guard helpers from `installable-attr-path.cc`
  (`insertJsonSet`, JSON guard encoder/builder, JSON dep coverage helper, and
  the old JSON compact store-path helper). The active text sidecar guard path is
  unchanged.
- Rebuilt after cleanup with `nix build -L --builders '' .#nix-cli`; result:
  `/nix/store/a3ik9v62klw3izw0adiqi64xs5wqd0ji-nix-2.35.0pre20260515_dirty`.
  The prior unused-helper warnings from this file are gone.
- Re-ran the focused observed-directory and observed-key functional fixtures
  through the manual dev-shell harness; both passed.

Current v7 debug read:

- Ran `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1 nix run
  .#eval-trace-bench -- generate --num-commits 10 --runs cold,hot
  --with-debug` as `cold-debug/51` and `hot-debug/51`.
- `cold-debug/51`: `10` proof-action load attempts, `6` hits, `4` misses,
  `15` candidate checks, `6` guard accepts, and `9` guard rejects. The four
  misses stored four certs with `149508` proof deps. They spent
  `57.106262s` in `outPathPublish.forceUs` and `57.243498s` in
  `outPathPublish.childCaptureUs`.
- `hot-debug/51`: `10/10` proof-action hits, `10` candidate checks, no guard
  rejects, and no outPath publication work.
- Authorized projection is not the current cold lever: in `cold-debug/51` it
  attempted `4` routes and accepted `0`. The slow path is proof-guard rejection
  followed by child derivation `outPath` forcing, not a materialization problem
  inside an accepted projection route.
- The complete-keyset suppression counter is emitted while building/storing the
  guard, not while loading it. Stats now also expose it under
  `evalTrace.projection.proofJsonAction.store.observedDirectorySuppressedKeyset`
  so the functional fixture and future debug reads point at the right phase;
  the old load-side field remains for continuity.
- Validation after moving the fixture assertion to the store-side counter:
  `git diff --check` passed, `bash -n` passed for the observed-key and
  observed-directory fixtures, `nix build -L --builders '' .#nix-cli` passed
  with result
  `/nix/store/22919x4fml4dxj65hj8xvaf0ayj578fy-nix-2.35.0pre20260515_dirty`,
  and both focused fixtures passed through the manual dev-shell harness.

Updated adversarial read:

- The flat action proof is the bottleneck abstraction now. It can say "this
  complete directory keyset was observed" and it can say "this child was
  positively observed", but it cannot say whether the complete keyset was only
  consumed while constructing a lazy package index and therefore did not flow to
  the demanded JSON output.
- Reopening v6's broad by-name relaxation would recover cold wins by ignoring
  those complete keysets, but the mixed `attrNames + hasAttr` functional fixture
  shows the direct unsound case. A path/domain whitelist would have the same
  problem hidden under nixpkgs-specific assumptions.
- The next credible implementation direction is provenance precision, not more
  sidecars: either record by-name package-index construction as per-key lazy
  routes so unselected child churn is outside the proof, or carry enough
  consumer provenance to prove that a complete keyset observation is not an
  output dependency for the demanded projection.

## 2026-05-25: complete-keyset provenance ablation

Added phase-split counters for `tryPublishOutPathObject` so the complete
directory keyset source can be localized:

- `navigationDeps` / `navigationCompleteKeysets`
- `targetDirectDeps` / `targetDirectCompleteKeysets`
- `childDirectDeps` / `childDirectCompleteKeysets`
- `externalChildDeps`

Validation:

- `nix build -L --builders '' .#nix-cli` passed with result
  `/nix/store/hbc0mph090gw3sbdgc1kbrhpin12zls2-nix-2.35.0pre20260515_dirty`.
- Focused observed-directory and observed-key functional fixtures passed.

10-commit strict v7 debug run:

- Command: `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1 nix run
  .#eval-trace-bench -- generate --num-commits 10 --runs cold,hot
  --with-debug`.
- Auto-selected run `52`.
- `cold-debug/52`: `10` proof-action load attempts, `6` hits, `4` misses,
  `15` candidate checks, `6` guard accepts, `9` guard rejects.
- `outPathPublish`: `4` attempts / `4` successes, `149508` retained deps,
  `8` navigation deps, `12` target direct deps, `253844` child direct deps,
  `6008` child direct complete keysets, `0` navigation complete keysets,
  `0` target direct complete keysets.
- `forceUs` was `57.551722s`; `childCaptureUs` was `57.695896s`.
- `hot-debug/52`: `10/10` proof-action hits, no outPath publication work.

Causal read:

- The strict v7 cold loss is not route/navigation baggage. Complete keysets are
  introduced while forcing the child derivations' `outPath`s. That means
  "positive projection navigation" is the wrong abstraction level for the
  remaining cold tail.
- Relaxing only navigation deps would not help: the measured
  `navigationCompleteKeysets` count is zero.
- The next useful question is whether complete by-name keysets inside child
  derivation forcing are semantically necessary for the demanded output or are
  loader-construction artifacts.

Implemented an intentionally experimental v8 ablation:

- Env: `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_RELAX_COMPLETE_KEYSET_BY_NAME=1`
  plus `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1`.
- The flag lets observed by-name children relax complete directory keysets, but
  only under proof version `8`. Default v7 will not accept these sidecars.
- Added `observedDirectoryRelaxedKeyset` counters to make the weakening visible.

Safety/adversarial check:

- Default v7 focused fixtures still pass after the v8 ablation code.
- Running the observed-directory fixture with the experimental flag exits `1`:
  the mixed `attrNames + hasAttr` case no longer records
  `observedDirectorySuppressedKeyset >= 1`.
- This confirms v8 is not sound as a default design. It is a measurement tool
  for the missing provenance rule.

10-commit experimental v8 runs:

- No-debug run `59`: `cold/59` mean `4.404826s`, median `0.411107s`,
  p90 `12.708580s`, max `15.994091s`; `hot/59` mean `0.373640s`,
  median `0.357635s`, p90 `0.405653s`, max `0.498188s`. Outputs matched.
- Debug run `53`: `10` proof-action load attempts, `7` hits, `3` misses,
  `10` candidate checks, `7` guard accepts, `3` guard rejects.
- `cold-debug/53` recorded `6525` relaxed keyset observations, `8`
  observed-directory acceptances for unobserved children, and `0` suppressed
  keysets. It still had `4506` child direct complete keysets in the three
  remaining outPath publications.

100-commit experimental v8 result:

- Command: `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_RELAX_COMPLETE_KEYSET_BY_NAME=1
  nix run .#eval-trace-bench -- generate --num-commits 100 --runs cold,hot`.
- Auto-selected run `60`; outputs matched the comparison reference.
- `cold/60`: mean `2.004710s`, median `0.508824s`, p90 `12.775082s`,
  max `15.794430s`.
- `hot/60`: mean `0.377289s`, median `0.360098s`, p90 `0.422324s`,
  max `0.735898s`.
- Artifacts: `12` JSON certs, `88` aliases, `12` guards, `12` store-path
  sidecars in both cold and hot state. This matches the old v6 artifact shape.
- Against regenerated baseline run `0`: `cold/60` is `0.60x` the baseline mean
  and `hot/60` is `0.43x`.
- Against strict v7 run `58`: `cold/60` improves mean from `2.871764s` to
  `2.004710s`; `hot/60` is slightly worse than `hot/58` (`0.377289s` vs
  `0.354958s`) but still far below baseline.

Decision:

- The complete by-name keyset relaxation is causal for the lost cold
  performance. v8 essentially recovers run `56` (`cold/56` mean `2.031514s`,
  `hot/56` mean `0.380999s`) on the regenerated branch state.
- The same relaxation is also demonstrably too broad: the functional keyset
  fixture fails under the experimental flag.
- Continue with default v7 as the sound baseline. The next implementation
  target is a proof form that can express "this complete by-name keyset was
  consumed only by the package-index loader for positive per-key routing" rather
  than treating every complete keyset as an output dependency or dropping it by
  path whitelist.

### Follow-up semantic read: why a path whitelist is not enough

Read the current Nixpkgs by-name loader:

- `pkgs/top-level/stage.nix` imports `./by-name-overlay.nix ../by-name` as
  `autoCalledPackages` and applies that overlay before `all-packages.nix`.
- `by-name-overlay.nix` constructs `packageFiles` with
  `mergeAttrsList (mapAttrsToList namesForShard (readDir baseDirectory))`.
- `mapAttrsToList` is `attrValues (mapAttrs f attrs)`, so evaluating the
  package index records complete keysets for the top-level shard directory and
  for each read shard.
- `callPackageWith` later computes `allArgs = intersectAttrs fargs autoArgs //
  args`, so adding a new package can change an existing package when the new
  package name matches a formal parameter of a forced package expression. This
  is guarded by the existing per-key `#has` deps from `intersectAttrs`, not by
  the complete by-name shard keyset.

Single-commit strict v7 debug checkpoint:

- Command: `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1 nix run
  .#eval-trace-bench -- generate --num-commits 1 --runs cold --with-debug`.
- Auto-selected run `54`, commit `7d7616894ec4`, wall `19.15s`.
- `outPathPublish`: `1` attempt / `1` success, `37377` deps,
  `2` navigation deps, `3` target direct deps, `63461` child direct deps,
  `1502` child direct complete keysets, `0` navigation complete keysets.
- The v7 proof action stored only four retained JSON deps because path deps
  were in the git guard sidecar. The guard had `3981` exact paths, `1042`
  directory-listing paths, `3625` recursive paths, and `754` guarded
  `pkgs/by-name/<shard>` directories.
- Only three by-name shard directories had accepted observed children under
  strict v7 (`op`, `gl`, `go`); `2175` observed child opportunities were
  suppressed because the corresponding shard also had complete keyset
  observations.

Adversarial conclusion:

- It is not sound to say "all by-name shard additions are irrelevant." They are
  irrelevant only when the new child is neither selected nor used as an
  automatic argument by any forced package expression, and when no evaluated
  code observes the package-set keyset as data.
- The existing `intersectAttrs` per-key recording handles the auto-argument
  case: if a forced package had formal `foo` and `pkgs.foo` was absent, adding
  `pkgs/by-name/fo/foo` should be rejected by the recorded `#has:foo` miss.
- The remaining unsound case for v8 is real keyset observation, for example a
  derivation whose name or output depends on `attrNames pkgs` or on
  `attrNames (readDir ../by-name/he)`. That produces the same complete
  directory keyset shape as the loader construction, so the action proof cannot
  currently distinguish it.

Design implication:

- The useful abstraction is not "observed directory children for by-name paths";
  that is only the performance symptom.
- The useful abstraction is "construction-only shape observation" versus
  "result-visible shape observation." Complete keysets used only to construct a
  package index should be reducible to per-key route/has deps for the demanded
  projection. Complete keysets consumed by user-visible operations like
  `attrNames`, equality, JSON/XML serialization, missing-attr suggestions, or
  function formal checking must remain invalidating deps.
- Without that distinction, any implementation that recovers v8's cold wins is
  either an unsound domain heuristic or a benchmark-specific assumption.

### Callsite telemetry and v9 construction-keyset ablation

Added attr-key observation telemetry so complete keyset records are grouped by
the Nix source location that caused the observation, not just by the C++ helper
that recorded it.

Single-commit strict v7 debug run:

- Command: `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1 nix run
  .#eval-trace-bench -- generate --num-commits 1 --runs cold --with-debug`.
- Auto-selected run `59`, commit `7d7616894ec4`, wall `19.4s`.
- `attrKeys.records=4686`, `directoryRecords=4530`.
- `directoryByReason`: `attrNames=4508`, `attrValues=10`,
  `functionFormals=12`.
- Dominant observation sites:
  - `pkgs/top-level/splice.nix:70:33`: `2988` directory keysets from
    `lib.listToAttrs (map merge (lib.attrNames mash))`.
  - `lib/customisation.nix:292:33`: `1520` directory keysets from
    `removeAttrs fargs (attrNames allArgs)` in the `callPackageWith`
    missing-argument check.
  - `lib/customisation.nix:180:18`: `12` directory keysets from function formal
    checking.
  - `lib/attrsets.nix:1085:30` and `lib/attrsets.nix:374:49`: `10` total
    directory keysets from `attrValues`.

This falsified the earlier narrow hypothesis that the lost cold performance was
mainly the by-name loader's `attrValues` implementation. The expensive complete
keysets are mostly construction/control-flow `attrNames` calls:

- `splice.nix:70` is constructing the spliced package set. For a demanded
  positive route to a package's `outPath`, unrelated new keys are not directly
  result-visible, but exposing the whole package set or its names would make the
  keyset visible.
- `customisation.nix:292` is the automatic-argument missing-argument check.
  The semantic hazard is adding a new package whose name matches a formal of a
  forced package; that should be guarded by the existing `intersectAttrs`
  per-key `#has` deps, not by retaining every complete `allArgs` keyset.

Implemented an intentionally narrower v9 ablation:

- Env: `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_SUPPRESS_CONSTRUCTION_KEYSETS=1`
  plus `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1`.
- Proof JSON action version becomes `9` only under this flag.
- Suppresses directory-format `attrNames` `#keys` deps only at the two measured
  construction sites:
  - `/pkgs/top-level/splice.nix:70:33`
  - `/lib/customisation.nix:292:33`
- Does not suppress `attrValues`, function-formal, JSON/XML, equality, or
  unknown keyset observations.
- The flag is added to the proof-action ignored-env list so the proof version,
  not an unrelated env miss, separates v9 artifacts from default v7 artifacts.

Validation after implementation:

- `nix build -L --builders '' .#nix-cli` passed.
- Focused default proof fixtures passed:
  - `eval-trace-json-action-observed-directory-proof.sh`
  - `eval-trace-json-action-observed-key-proof.sh`

Single-commit v9 debug result:

- Command: `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_SUPPRESS_CONSTRUCTION_KEYSETS=1
  nix run .#eval-trace-bench -- generate --num-commits 1 --runs cold
  --with-debug`.
- Auto-selected run `60`, commit `7d7616894ec4`, wall `19.2s`.
- `attrKeys.records` dropped from `4686` to `178`.
- `directoryRecords` dropped from `4530` to `22`.
- `directoryByReason.attrNames` dropped from `4508` to `0`; remaining
  directory keysets were `attrValues=10`, `functionFormals=12`.
- `childDirectCompleteKeysets` dropped from `1502` to `12`.
- `observedDirectorySuppressedKeyset` dropped from `2175` to `2`.
- `outPathPublish.deps` dropped from `37377` to `36632`, a small direct dep
  count reduction; the important effect is enabling observed-directory guards to
  accept unchanged positive routes instead of forcing republishing.

10-commit v9 no-debug result:

- Command: `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_SUPPRESS_CONSTRUCTION_KEYSETS=1
  nix run .#eval-trace-bench -- generate --num-commits 10 --runs cold,hot`.
- Auto-selected run `61`; outputs matched the reference for all cold/hot
  commits.
- Same-10 comparison:
  - `cold/61`: mean `4.408127s`, median `0.399040s`, p90 `13.096571s`,
    max `15.767903s`.
  - Baseline `cold/0` on the same commits: mean `5.863616s`, median
    `5.457449s`, p90 `10.355227s`, max `13.428621s`.
  - `hot/61`: mean `0.384565s`, median `0.376941s`, p90 `0.414971s`,
    max `0.420051s`.
  - Baseline `hot/0` on the same commits: mean `0.891211s`, median
    `0.888664s`, p90 `0.912989s`, max `0.932551s`.

100-commit v9 no-debug result:

- Command: `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_SUPPRESS_CONSTRUCTION_KEYSETS=1
  nix run .#eval-trace-bench -- generate --num-commits 100 --runs cold,hot`.
- Auto-selected run `62`; outputs matched the reference for all cold/hot
  commits.
- Same-100 comparison:
  - `cold/62`: mean `2.006006s`, median `0.491624s`, p90 `12.809324s`,
    max `16.236203s`.
  - Baseline `cold/0`: mean `3.316277s`, median `0.938353s`,
    p90 `10.515076s`, max `13.428621s`.
  - Strict v7 `cold/58`: mean `2.871764s`, median `0.475416s`,
    p90 `12.956262s`, max `15.832111s`.
  - Broad unsound v8 `cold/60`: mean `2.004710s`, median `0.505714s`,
    p90 `12.775082s`, max `15.794430s`.
  - `hot/62`: mean `0.379222s`, median `0.371254s`, p90 `0.432011s`,
    max `0.728728s`.
  - Baseline `hot/0`: mean `0.886366s`, median `0.885450s`,
    p90 `0.905417s`, max `0.932551s`.
  - Strict v7 `hot/58`: mean `0.354958s`, median `0.338118s`,
    p90 `0.400835s`, max `0.677825s`.
  - Broad unsound v8 `hot/60`: mean `0.377289s`, median `0.359470s`,
    p90 `0.422324s`, max `0.735898s`.
- Artifact shape for v9 matches v8: `12` JSON certs, `88` aliases, `12`
  guards, and `12` store-path sidecars in both cold and hot state.
- Slow cold commits (`>2s`) match broad v8 exactly: `12` commits in v9 and
  `12` in v8, versus `19` in strict v7 and `25` in regenerated baseline run
  `0`.

Causal read:

- The construction-keyset sites identified by telemetry are sufficient to
  recover broad v8 cold performance without relaxing all by-name complete
  keysets.
- Hot time remains around v8 and slightly slower than strict v7. That is
  expected: the narrow suppression changes which candidates are allowed to be
  guarded/aliased; it does not reduce the per-hit verifier/guard overhead that
  strict v7 already pays.
- The remaining cold tail is not complete-keyset suppression. The exact same
  `12` slow commits remain in v8 and v9, so the next cold lead is the guard
  invalidation/store-path republication surface for those commits.

Adversarial read:

- v9 is still not a final soundness rule. It is a callsite whitelist tied to
  current Nixpkgs source positions.
- The successful abstraction is not the whitelist; it is provenance for
  complete keysets. The proof system needs a way to distinguish a complete
  keyset used only to construct an intermediate package index from a complete
  keyset that can flow to the demanded result.
- A final implementation should make that provenance explicit in dep recording
  or publication, then allow positive observed-directory route proofs to drop
  construction-only complete keysets while retaining complete keysets for
  user-visible operations and control flow that can affect the selected result.

### v10 maintainer observed-key and v11 by-name directory-listing ablations

Follow-up question after v9: why does `169bbf0bfcad` still republish when its
JSON output and hidden string context match the earlier `a292268556ef` cert?

Negative v10 result:

- Implemented
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_OBSERVED_MAINTAINER_KEYS=1` as proof
  version `10`, extending observed-key guards to `maintainers/maintainer-list.nix`
  and refusing the guard when the proof also contains a complete Nix `#keys`
  structured projection for that file.
- Single-commit debug showed the new guard was present:
  `nixObservedKeyGuards=5`, including `maintainers/maintainer-list.nix` with
  `observedKeys=4010` and `bindingCount=21`.
- 100-commit no-debug run `63` did not reduce certificate count or slow commits:
  `cold/63` mean `1.999286s`, `hot/63` mean `0.380351s`, artifacts stayed
  `12` JSON certs / `88` aliases / `12` guards / `12` store-path sidecars.
- Conclusion: maintainer-list exact invalidation was not the remaining
  `169bbf0bfcad` miss. It is recorded as a failed lead.

Deeper guard read:

- The `a292268556ef -> 169bbf0bfcad` diff is mostly by-name package renames and
  package-file modifications:
  `ciscoPacketTracer8/9` rename to `cisco-packet-tracer_8/9`, plus `gwc`,
  `ioping`, `kicad`, `mihomo`, and `pkgs/top-level/aliases.nix`.
- The v10 `aliases.nix` observed-key guard for `a292268556ef` had only these
  bindings: `ao`, `gg`, `graalvm-ce`, `ir`, `nixos-rebuild`, `pn`, `python`,
  `src`, `xo`. None of the changed Cisco alias names were guarded, so
  observed-key rejection was not the cause.
- Parsing the `a292268556ef` guard sidecar showed the cause:
  `pkgs/by-name/ci`, `gw`, `io`, `ki`, and `mi` were still complete directory
  listing guards. `pkgs/by-name/ci` had no observed-child set, so the Cisco
  package-directory add/delete changed the immediate child set and rejected the
  certificate before observed-key guards mattered.
- That means v9 removed the complete `#keys` structured dep from construction
  sites, but did not slice the underlying `DirectoryEntries` path dep down to
  the positive children observed by this result.

Implemented v11 ablation:

- Env:
  `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_SUPPRESS_CONSTRUCTION_KEYSETS=1
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_RELAX_BY_NAME_DIRECTORY_LISTINGS=1`.
- Proof JSON action version becomes `11` under that flag combination.
- For by-name directory listing guards with no retained complete directory
  keyset, encode an observed-child scope even when the observed set is empty.
  Verification treats presence of the map entry as positive-membership
  authority: unobserved immediate-child add/delete/type changes are accepted;
  observed child changes still reject.
- Complete directory keysets are still excluded from this relaxation through
  `completeDirectoryKeysetPaths`, so keysets that survived v9 continue to get
  strict directory-listing behavior.

Validation:

- `nix build -L --builders '' .#nix-cli` passed.
- Focused proof fixtures passed when run through the functional harness with
  the current `result` binary:
  - `eval-trace-json-action-observed-directory-proof.sh`
  - `eval-trace-json-action-observed-key-proof.sh`

Benchmark results:

- 10-commit run `64` was a smoke gate. Outputs matched reference.
  - `cold/64` mean `4.345s` versus same-window v9 `cold/62` `4.454s` and
    baseline `cold/0` `5.864s`.
  - `hot/64` mean `0.369s` versus same-window v9 `hot/62` `0.358s` and
    baseline `hot/0` `0.891s`.
- 100-commit run `65` is the real read. Outputs matched reference.
  - `cold/65`: mean `1.879258s`, median `0.490658s`, p90 `12.849914s`,
    max `15.838282s`.
  - `hot/65`: mean `0.377882s`, median `0.359400s`, p90 `0.425083s`,
    max `0.688843s`.
  - Against regenerated baseline run `0`: cold is `0.57x` baseline mean
    (`1.879s` vs `3.316s`), hot is `0.43x` baseline mean (`0.378s` vs
    `0.886s`).
  - Against v9 run `62`: cold improves `2.006006s -> 1.879258s`; hot is
    effectively unchanged (`0.379222s -> 0.377882s`).
  - Artifact shape improves from v9/v10 `12` JSON certs / `88` aliases /
    `12` guards / `12` store-path sidecars to v11 `11` JSON certs /
    `89` aliases / `11` guards / `11` store-path sidecars.
  - Slow cold commits (`>2s`) drop from `12` in v9/v10 to `11` in v11.
    `169bbf0bfcad` drops from about `12.8s` to about `0.4s`.
- The v11 `a292268556ef` guard has `obsDirs=748`, `obsChildren=2210`, and
  `414` empty by-name observed scopes. `pkgs/by-name/ci` is explicitly present
  with an empty child set, which is why the later Cisco rename is accepted.

Causal read:

- The improvement is directly tied to the guard shape: v10 had no observed
  child scope for `pkgs/by-name/ci` and rejected the Cisco add/delete; v11
  records an empty observed scope for that directory and accepts the unobserved
  child rename.
- This confirms the useful abstraction is not "by-name is always safe"; it is
  "a complete directory listing used only to construct a package index can be
  sliced to the positive child names actually demanded by the result."
- The remaining 11 slow commits all publish real new certs. The next search
  should classify whether each is a true output/context change or another
  over-broad proof guard; do not assume the whole tail is removable.

Adversarial read:

- v11 is still an ablation, not the final soundness rule. It relies on by-name
  layout plus the v9 callsite suppression to infer construction-only directory
  listings.
- Empty observed-child scopes are only sound when absence of every unobserved
  child is not result-visible. That is not true for `attrNames`, suggestions,
  missing-attr errors, JSON/XML serialization, equality, or function formal
  checks. The retained complete-keyset gate is the current backstop, but a
  final design should record construction-only provenance explicitly instead
  of deriving it from Nixpkgs paths and callsites.
- A future hardening path is to turn the v9/v11 heuristics into a first-class
  proof fact: "this complete keyset was consumed only to build an intermediate
  route table, and the published result depends only on these positive child
  routes." That would remove the by-name-specific assumption while preserving
  the benchmark win.

Remaining-tail classification after v11:

- The 11 slow v11 commits are exactly the 11 JSON certs. Most have unique
  output+context signatures.
- One remaining duplicate exists: `34513330fcc1` has the same JSON output and
  hidden string context as the earlier `1cfdcfa5620e` certificate, but still
  republishes.
- Manual guard replay for `1cfdcfa5620e -> 34513330fcc1` found the rejection
  is now exact file content, not directory shape:
  - `pkgs/by-name/li/libvpx/package.nix`
  - `pkgs/by-name/ne/newt/package.nix`
- Those source changes are derivation-path no-ops for the packages in both
  benchmark systems:
  - x86_64 `libvpx`: both revs evaluate to
    `/nix/store/52xi4wnnb56zkprh7adlrdmhd1fyzib0-libvpx-1.15.2.drv`
  - x86_64 `newt`: both revs evaluate to
    `/nix/store/piwydcjpzmcdcj2zmzxw3rw5g8ibda7z-newt-0.52.24.drv`
  - aarch64 `libvpx`: both revs evaluate to
    `/nix/store/9vjly3fhvw86rb6bjx6v1gq31g5gcj4j-libvpx-1.15.2.drv`
  - aarch64 `newt`: both revs evaluate to
    `/nix/store/kjrlhvn48d6rlkzw457d0jkwipd7q2zf-newt-0.52.24.drv`
- Evaluating both current x86_64 package drv paths in one import took about
  `3.2s` once the local `fetchGit` source was cached. Evaluating old and current
  drv paths for the two packages in one expression took about `6.1s`. That is
  cheaper than the `~12.9s` full republish, but not cheap enough to bolt into
  the hot path casually.

Next design lead:

- The next removable miss is not another directory keyset. It is exact
  FileBytes backstop precision for observed by-name package files whose
  source changes but whose demanded derivation path is unchanged.
- A plausible final abstraction is a semantic package-file guard: exact source
  changes to a package file may be accepted only if a stored package-level
  semantic projection proves the same drv path/output-relevant value for the
  demanded systems.
- Do not implement this by recursively shelling out to `nix eval` per changed
  package in the verifier. The manual timing shows it can save one cold miss,
  but it would be a fragile and expensive verifier path. The better
  implementation would reuse already-recorded derivation/store-path observations
  from the trace or publish a compact package semantic sidecar when that data is
  already available.
- Adversarial constraint: dropping the FileBytes backstop for package files is
  unsound unless the replacement proof is semantic. A changed package file can
  absolutely change the top-level system drv; old context store-path existence
  only proves the old result still exists, not that the current evaluation would
  produce it.

### 2026-05-25 v11 tail: stdout, context, and exact-file diagnostics

Follow-up classification of run `65` refined the remaining tail:

- The v11 cache has `11` real JSON certificates. Their guard sidecars are about
  `451 KiB` each and their context store-path sidecars are about `9 KiB` each.
  At this point sidecar size is not the cold-tail driver; full evaluation for
  first-time cert publication is.
- Full semantic duplicates, meaning same printed JSON and same hidden string
  context, are rare. The only remaining full duplicate is still
  `1cfdcfa5620e -> 34513330fcc1`.
- There is also a stdout-only duplicate:
  `7fb36e13811a -> a292268556ef` has identical printed JSON but different
  `Built` string context drv paths. That is not the same `JsonInstallableOutput`
  under the current cache model.
- The `1cfdcfa5620e -> 34513330fcc1` full duplicate rejects on exact
  FileBytes for `libvpx/package.nix` and `newt/package.nix`.
- The `7fb36e13811a -> a292268556ef` stdout-only duplicate has an exact
  FileBytes hit on
  `pkgs/tools/package-management/nix-prefetch-scripts/default.nix`, and also
  changes hidden derivation contexts.

Semantic read of the context issue:

- For `nix eval --json`, the observable stdout is the JSON string. After
  writing stdout, the command calls `ensureLazyPathsCopied(context)`, and that
  currently copies only opaque path contexts, not `Built` derivation-output
  contexts.
- That means a stdout-only cache for this exact command might be able to ignore
  changed `Built` contexts when the printed JSON is identical.
- I am not taking that shortcut in the current proof cache. The existing
  `JsonInstallableOutput` includes context, and the missing-store-dependency
  fallback/warning behavior depends on retaining and checking relevant store
  dependencies. Treating context as non-authoritative for all JSON action cache
  entries would be too broad.
- A possible future experiment is a separate stdout-only certificate for
  `nix eval --json` that stores only stdout plus the opaque contexts needed by
  `ensureLazyPathsCopied`. That should be a distinct proof version/scope, not
  a silent weakening of the current action certificate.

Implementation note:

- Added cheap guard-rejection counters, reported under
  `evalTrace.proofJsonAction.load` when `NIX_SHOW_STATS` is enabled:
  `guardRejectGitComponent`, `guardRejectExact`,
  `guardRejectExactOverlap`, `guardRejectDirectory`, and
  `guardRejectRecursive`.
- These counters are disabled in normal benchmarks by the existing
  `Counter::enabled` gate, so they do not affect no-stats timing runs. They
  replace some of the ad hoc offline guard replay work for future diagnosis.

Adversarial read:

- A package-file semantic guard can remove at most the remaining full duplicate
  in this 100-commit run unless it also intentionally changes the cache's
  output semantics. Eliminating one `~12.9s` republish would improve the cold
  mean by roughly `0.12s`; using a recursive `nix eval` verifier that costs
  about `6s` would recover only about half of that.
- The package-file guard is still a valid design lead because it is the next
  true false negative, but the benchmark upside is now bounded. It should be
  implemented only if it can reuse facts already produced while publishing the
  certificate or record a compact semantic sidecar at publication time.
- The stdout-only duplicate is a separate design lead. It could produce another
  cert reduction for `nix eval --json`, but it must be isolated from commands
  where string context is semantically consumed.

### 2026-05-25 harness commit-list and exact v11 duplicate diagnostic

Harness change:

- Added `generate --commit-list <path>` for the `nixpkgs-release` and
  `flake-attr` suites. The file is one commit per line; blank lines and
  `#` comments are ignored. The harness preserves that exact order and records
  `sourceRef = commit-list:<path>` in the run manifest.
- Added `benchmarkEnvironment` to generated manifests for eval-trace-related
  environment switches (`NIX_EVAL_TRACE_*`, `NIX_ALLOW_EVAL`, and
  `NIX_DEBUG_SQLITE_TRACES`). This was added after the `cold/66` vs `cold/67`
  child-fragment ablation because that comparison exposed the reproducibility
  gap: the active experimental flag set was visible only in the design log.
- Added a tracked diagnostic list at
  `benchmarks/eval-trace-bench/commit-lists/v11-tail-1cfd-345.txt`. This fixes
  the earlier reproducibility problem where a `--nixpkgs-base 1cfd...`
  22-commit window did not actually include `34513330fcc1`.
- Validation: `nix build -L --builders '' .#eval-trace-bench` passed. That
  package check ran `36` pytest tests, `ruff check`, `ruff format --check`, and
  `pyright`. I then restored `result/bin/nix` with
  `nix build -L --builders '' .#nix-cli`.

Targeted debug run:

- Command:
  `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_SUPPRESS_CONSTRUCTION_KEYSETS=1
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_RELAX_BY_NAME_DIRECTORY_LISTINGS=1
  nix run .#eval-trace-bench -- generate --commit-list
  benchmarks/eval-trace-bench/commit-lists/v11-tail-1cfd-345.txt --runs cold
  --with-debug`.
- Auto-selected `cold-debug/63`.
- Timings:
  - `1cfdcfa5620e`: `18.751296539s`
  - `34513330fcc1`: `15.374079172s`
- The printed `eval.json` files are byte-identical.
- The cache still stored two v11 JSON certs, two guards, and two store-path
  sidecars. Each JSON cert is `224851` bytes, guard sidecar `451234` bytes,
  store-path sidecar `9301` bytes.

Counter read for `34513330fcc1`:

- `proofJsonAction.load.attempts = 1`
- `candidateChecks = 1`, `candidates = 1`
- `guardAccepted = 0`, `guardRejected = 1`
- `guardRejectExact = 1`
- `guardRejectDirectory = 0`, `guardRejectRecursive = 0`,
  `guardRejectExactOverlap = 0`, `guardRejectGitComponent = 0`
- Debug log names the first rejecting path:
  `pkgs/by-name/li/libvpx/package.nix`.

Causal read:

- This validates the offline classification with the real verifier path. The
  v11 duplicate is not losing to directory-listing precision anymore; it is
  losing to the exact FileBytes guard on an observed package file.
- The run also reinforces the bounded upside: serving this one duplicate would
  save one full publication, but the current verifier correctly refuses because
  source bytes changed and no semantic package-file proof exists.
- A better implementation should not weaken exact FileBytes globally. The next
  credible implementation is either:
  - publish a package-file semantic sidecar when the trace already forced the
    package derivation and can prove the same demanded drv path for the systems
    used by the top-level output, or
  - add a narrower stdout-only certificate for `nix eval --json` that is
    explicit about which string-context classes are observable.

### 2026-05-25 child-fragment ablation

Hypothesis:

- Existing child JSON fragment support might make exact package-file churn
  cheaper. If the aggregate `closures.gnome` cert rejects because one or two
  package files changed, per-child fragments could still reuse every unchanged
  child outPath and only force the changed package derivations.

Targeted `1cfdcfa5620e -> 34513330fcc1` result:

- Command: same v11 flags as `cold-debug/63`, plus
  `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_CHILD_FRAGMENTS=1`.
- Auto-selected `cold-debug/64`.
- Timings:
  - `1cfdcfa5620e`: `20.406587165s`
  - `34513330fcc1`: `16.993543719s`
- This is worse than `cold-debug/63` without child fragments
  (`18.751296539s`, `15.374079172s`).
- `34513330fcc1` stats:
  - `proofJsonAction.load.attempts = 3`
  - `guardRejected = 3`
  - `guardRejectExact = 3`
  - `outPathPublish.children = 2`
  - `outPathPublish.externalChildDeps = 0`
  - `outPathPublish.childCaptureUs = 12.767357s`
- Debug log shows all three proof rejections are still the same first exact
  path, `pkgs/by-name/li/libvpx/package.nix`. The fragment split does not help
  this miss because both published child fragments depend on the changed
  package file.

10-commit no-debug isolation:

- Child fragments enabled, v11 flags: `cold/66`, `hot/66`.
- Same v11 flags without child fragments: `cold/67`, `hot/67`.
- Mean wall times:
  - `cold/66`: `4.713943679s`
  - `hot/66`: `0.352999931s`
  - `cold/67`: `4.150945993s`
  - `hot/67`: `0.350506209s`
- Artifact count:
  - `cold/66`: `9` JSON certs, `9` guards, `9` store-path sidecars,
    proof cache about `7.1M`.
  - `cold/67`: `3` JSON certs, `3` guards, `3` store-path sidecars,
    proof cache about `2.4M`.

Decision:

- Reject child fragments as the next default direction. On this workload they
  add publication/write overhead and more proof artifacts without reducing the
  remaining exact-file cold tail.
- The negative result is useful: it says the remaining misses are not caused by
  aggregate-vs-child granularity. The changed package file participates in both
  published system children, so fragmenting at the child-system level is still
  too coarse.

### 2026-05-25 parallel adversarial review and clean child-fragment control

Parallel review results:

- Bohr challenged v11 soundness: empty observed-child scopes for
  `pkgs/by-name/*` are still a heuristic, not a general directory-listing
  theorem. The rule is benchmark-useful, but final hardening needs coverage for
  complete-keyset consumers beyond the current fixtures.
- Nash identified the next plausible implementation gap: changed
  `pkgs/by-name/*/*/package.nix` files still fall through to exact FileBytes.
  The existing observed-key machinery can only help if those files publish a
  semantic guard and are removed from the exact guard.
- Boole found that the earlier `cold-debug/63` vs `cold-debug/64` targeted
  child-fragment comparison had a dirty-tree mismatch. The 10-commit
  `cold/66` vs `cold/67` comparison was cleaner, but it still lacked
  manifested env provenance because the harness field was added afterwards.

Clean targeted child-fragment control:

- Same Nix checkout head, same dirty diff hash, same Nix binary, same commit
  list. The only manifest-level flag difference is
  `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_CHILD_FRAGMENTS=1`.
- No-child run: `cold-debug/65`
  - `1cfdcfa5620e`: `20.282704910s`
  - `34513330fcc1`: `15.042035039s`
  - proof artifacts: `2` JSON certs, `2` guards, `2` store-path sidecars
- Child-fragment run: `cold-debug/66`
  - `1cfdcfa5620e`: `20.238843624s`
  - `34513330fcc1`: `16.758210170s`
  - proof artifacts: `6` JSON certs, `6` guards, `6` store-path sidecars
  - `34513330fcc1` stats: `guardRejectExact = 3`,
    `externalChildDeps = 0`, `childCaptureUs = 12.555185s`

Causal read:

- The clean control confirms the 10-commit read: child fragments are not the
  right lead for this tail. They multiply proof artifacts and do not move the
  rejection away from exact FileBytes.
- The next prototype should be a distinct v12 experiment for by-name package
  files. It must use file-local observed keys, not the current global observed
  key universe; otherwise a package file could be authorized by proving that
  unrelated top-level keys are absent.

### 2026-05-25 v12 by-name package observed-key prototype

Implementation:

- Added
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_OBSERVED_BY_NAME_PACKAGE_KEYS`.
- The flag selects proof JSON action cache version `12`.
- It extends observed-key guard candidates to
  `pkgs/by-name/<prefix>/<name>/package.nix`, but only for the canonical
  by-name layout where `<prefix>` matches the first two characters of `<name>`.
- For these package files, the guard uses file-local structured keys collected
  from proof deps for that exact Nix file. It does not reuse the global observed
  key universe used by top-level files. This is the important soundness
  adjustment from the adversarial review.
- Complete keyset files still fall back to exact FileBytes; the prototype does
  not authorize negative/complete-keyset behavior for package files.

Targeted run:

- Command: v11 flags plus
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_OBSERVED_BY_NAME_PACKAGE_KEYS=1`.
- Build validation: `nix build -L --builders '' .#nix-cli` passed before the
  run.
- Auto-selected `cold-debug/67`.
- Timings:
  - `1cfdcfa5620e`: `18.5s`
  - `34513330fcc1`: `15.2s`
- `34513330fcc1` load stats:
  - `guardAccepted = 0`
  - `guardRejected = 1`
  - `guardRejectExact = 1`
  - `nixObservedKeyGuards = 0`
  - `nixObservedKeyVerified = 0`
- Store stats:
  - `nixObservedKeyGuards = 766`
  - `nixObservedKeyGuardFailures = 0`
- Certificate inspection:
  - `762` by-name package guards were published.
  - Neither `pkgs/by-name/li/libvpx/package.nix` nor
    `pkgs/by-name/ne/newt/package.nix` received an observed-key guard.
  - Both files remain in the exact guard and the debug log still rejects first
    on `pkgs/by-name/li/libvpx/package.nix`.

Causal read:

- The simple package-key extension is not the lead for the current tail. It can
  publish many file-local package guards, but the problematic files remain exact
  because the current trace facts do not provide a positive, file-local
  observed-key proof that is allowed to replace their FileBytes deps.
- This points back to dependency slicing/provenance, not syntax-level package
  key hashing. The changed package files are evaluated and even instantiate
  package derivations, but the final JSON for `closures.gnome` is unchanged.
  The missing proof is "this evaluated package-file work is not causally needed
  for the printed top-level outPath JSON/context", not "this package file has
  unchanged observed top-level keys".
- Keep v12 as an explicit experiment for now, but do not include it in the next
  benchmark gate unless a later change makes it remove the actual exact
  rejection.

Post-counter rerun:

- Rebuilt `.#nix-cli` after adding file-local candidate/skip counters, then
  reran the same two-commit diagnostic as `cold-debug/68`.
- Timings:
  - `1cfdcfa5620e`: `18.6s`
  - `34513330fcc1`: `15.1s`
- `34513330fcc1` store stats:
  - `nixObservedKeyFileLocalCandidates = 3292`
  - `nixObservedKeyGuards = 766`
  - `nixObservedKeySkippedCompleteKeyset = 0`
  - `nixObservedKeySkippedNoFileLocalKeys = 151`
  - `deps = 36632`
- `34513330fcc1` load stats are unchanged in kind:
  `guardRejected = 1`, `guardRejectExact = 1`, and no observed-key verification
  ran.
- Certificate inspection confirms the earlier read:
  `762` by-name package guards were published, but neither
  `pkgs/by-name/li/libvpx/package.nix` nor
  `pkgs/by-name/ne/newt/package.nix` received one. Both remain exact
  `FileBytes` guard entries.

Decision:

- Reject v12 as the next performance lead in its current form. It proves that
  file-local observed-key package guards are feasible and cheap to emit for many
  files, but the actual slow duplicate is not blocked by missing package-file
  syntax. It is blocked by over-captured exact source dependencies for package
  work that is not causally needed by the final JSON output.
- The next search should move into dependency provenance/slicing for published
  JSON outPath proofs: identify why `libvpx` and `newt` exact `FileBytes` deps
  enter both the aggregate proof and the child fragments even when their
  resulting drv paths and the top-level printed JSON/context are unchanged.

### 2026-05-25 exact package-file ablation

Question:

- Is the `1cfdcfa5620e -> 34513330fcc1` duplicate blocked only by the exact
  `FileBytes` entries for `pkgs/by-name/li/libvpx/package.nix` and
  `pkgs/by-name/ne/newt/package.nix`, or is another proof obligation hiding
  behind them?

Manual scratch-state ablation:

- Started from `cold-debug/68` v12 state.
- Removed only those two exact paths from the first certificate's git guard,
  updated the guard hash/size in the certificate, and deleted the second
  commit's exact certificate.
- Running the second commit through the benchmark harness with
  `NIX_ALLOW_EVAL=0` served in `0.4s`.
- Stats for `hot-debug/69/34513330fcc1`:
  - `proofJsonAction.load.hits = 1`
  - `guardAccepted = 1`
  - `guardRejectExact = 0`
  - `outPathPublish.successes = 0`
  - `proofJsonAction.store.attempts = 0`
  - JSON output matched the real `34513330fcc1` output byte-for-byte.

Reproducible ablation flag:

- Added
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DROP_V11_TAIL_PACKAGE_FILEBYTES`.
  It is intentionally opt-in and is included in the ignored routing-env list
  so it can test existing command proof keys. It skips exact git-guard
  rejection only for:
  - `pkgs/by-name/li/libvpx/package.nix`
  - `pkgs/by-name/ne/newt/package.nix`
- This is not a sound proof rule. It is a measurement oracle for the remaining
  duplicate's ceiling.
- Normal two-commit run `cold-debug/70`, with v11/v12 flags plus the ablation,
  reproduced the same result:
  - `1cfdcfa5620e`: `19.9s`
  - `34513330fcc1`: `0.4s`
  - `proofJsonAction.load.hits = 1`
  - `guardAccepted = 1`
  - `outPathPublish.successes = 0`
  - one real JSON certificate and one alias.

Causal read:

- The exact package-file guard entries are necessary and sufficient for this
  remaining full duplicate miss.
- The next sound implementation cannot simply drop these paths. It needs a
  replacement proof fact for "the demanded package-level semantic projection
  that can affect the top-level outPath JSON/context is unchanged." The likely
  shape is a package semantic sidecar tied to already-produced derivation
  observations, not broader observed-key parsing.

100-commit ceiling:

- Ran current-binary control `cold/72,hot/72` with v11 flags and no ablation,
  and current-binary ablation `cold/71,hot/71` with the same commit order.
- `cold/72` control:
  - mean `1.894906s`
  - median `0.535810s`
  - p90 `12.328297s`
  - max `15.808306s`
  - `11` real JSON certs, `89` aliases
  - `34513330fcc1` took `12.328297s`
- `cold/71` ablation:
  - mean `1.776649s`
  - median `0.523087s`
  - p90 `12.135624s`
  - max `16.056072s`
  - `10` real JSON certs, `90` aliases
  - `34513330fcc1` took `0.773597s`
- `hot/72` control mean was `0.452585s`; `hot/71` ablation mean was
  `0.399884s`. Treat the hot delta as secondary because it is near process
  startup/noise scale, but the artifact count and `345` cold timing are
  decisive.

Decision:

- A sound package-file semantic proof can recover about one cold publication in
  this 100-commit window, roughly `0.12s` mean wall time on the current binary.
- That is useful but bounded. It is not the next high-leverage performance
  direction unless the same proof generalizes to more tail commits or can be
  emitted/verified from already-captured facts with near-zero hot cost.
- Do not spend the next iteration building a verifier that recursively
  evaluates changed packages: the earlier manual timing was multiple seconds
  for two packages, which would consume a large fraction of the recovered cold
  win and would fail the `NIX_ALLOW_EVAL=0` cache-only bar.

### 2026-05-25 parallel adversarial review and isolation fix

Research agents:

- Anscombe independently rechecked the `cold/71,hot/71` ablation and
  `cold/72,hot/72` control artifacts. The comparison is clean on commit order,
  Nix binary hash, checkout head, and dirty diff hash. Output hashes match the
  reference. The only artifact-shape change in the 100-commit window is
  `34513330fcc1`: control stores a real JSON cert, guard, and store-path
  sidecar; ablation stores an alias to `1cfdcfa5620e`. This supports the causal
  read that the two exact package-file guards are necessary and sufficient for
  that miss in this window.
- Averroes found a real experiment-isolation hole. Cache version selection used
  only the highest-priority experiment, while the experiment env vars were
  ignored by the impure routing scope. A certificate written with v12 package
  observed-key mode plus v11 empty by-name directory scopes could later be read
  as plain v12, because the guard sidecar itself carried enough authority and
  the loader only checked the selected integer version and proof key.
- Averroes also challenged the unsafe ablation hook. It was intentionally a
  measurement oracle, but because it was ignored for routing it could reuse
  existing proof keys and write aliases after accepting a verifier rule that is
  not sound.
- Mencius independently ranked explicit route/keyset provenance as the next
  high-leverage direction. The current v11 win is from positive directory
  membership for route construction, not from a general by-name exception. The
  proof should eventually say "this complete directory keyset was route-only"
  instead of inferring that from source positions and `pkgs/by-name/*` paths.

Implementation response:

- Added `proofJsonActionAuthorityMode()` and included it in the aggregate proof
  key and child proof key material. It records the semantic proof-authority
  flags, including context-store proof scope, retained structural proof,
  observed-key proof, complete-keyset relaxation, construction-keyset
  suppression, maintainer-key/package-key experiments, by-name directory
  listing relaxation, and unsafe ablation state.
- Stored the same authority mode in each certificate as
  `proofAuthorityMode`, and verification now rejects certificates whose mode
  does not match the current verifier authority.
- Removed
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DROP_V11_TAIL_PACKAGE_FILEBYTES` from
  the ignored routing-env list. In addition, that measurement oracle now only
  activates when `NIX_EVAL_TRACE_JSON_ACTION_ALLOW_UNSAFE_ABLATIONS=1` is also
  set. The unsafe mode is still present for reproducible local measurements,
  but it is isolated in the proof key and cannot be triggered accidentally by
  the old single env var.
- Fixed the guard sidecar reference kind for observed-directory proofs from the
  misleading `sibling-v2` label to `sibling-v4`, matching the actual
  `git-guard.v4` payload.

Validation:

- `git diff --check -- src/libcmd/installable-attr-path.cc
  eval-trace-cache-rewrite-tasks.md` passed.
- `nix build -L --builders '' .#nix-cli` passed with result
  `/nix/store/dxidhf8njym4gf1v0hxsj28zb2c2wcka-nix-2.35.0pre20260515_dirty`.
- Runtime smoke:
  `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_SUPPRESS_CONSTRUCTION_KEYSETS=1
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_RELAX_BY_NAME_DIRECTORY_LISTINGS=1
  nix run .#eval-trace-bench -- generate --commit-list
  benchmarks/eval-trace-bench/commit-lists/v11-tail-1cfd-345.txt --runs cold
  --with-debug` produced `cold-debug/71`.
- `cold-debug/71` stored v11 certificates with `proofAuthorityMode` matching
  the active v11 flags. The first cert's `gitGuardRef.kind` is now
  `sibling-v4`, and its sidecar starts with
  `nix.eval-trace.json-action-proof.git-guard.v4`.
- As expected without the unsafe package-file ablation, `34513330fcc1` still
  misses on `guardRejectExact = 1`; this validates the isolation/format change
  without changing the prior causal read.

Decision:

- Continue with v11 as an explicitly experimental performance lead only after
  the isolation fix. Do not promote v11 as a final proof rule yet.
- The next sound direction is route/keyset provenance: replace
  `populateObservedByNameDirectoryScopes()` with a route-authorized scope that
  is not inherently by-name-specific and is suppressed whenever the keyset is
  result-visible.
- Keep package semantic sidecars as a secondary shadow-telemetry lead. The
  recursive verifier path is rejected, but a publication-time sidecar derived
  from already-forced derivation facts is not ruled out by the 100-commit data.

Local route/keyset provenance read after the isolation fix:

- `TraceSession::tryPublishOutPathObject()` captures navigation deps, target
  deps, and child direct deps separately for counters, but then merges them
  into one flat `projectionDeps` vector before recording the JSON action
  projection. `storeProofJsonActionCache()` receives only that flat vector.
  At that point it can see "this directory has a complete keyset dep" and "this
  directory has positive `#has:key` deps", but it cannot see why a complete
  keyset was demanded or whether it was only used to construct route thunks.
- `maybeRecordAttrKeysDep()` is the last place that still has observation kind
  and source position. Today it uses that information only for counters and the
  hard-coded construction-keyset suppression sites. The recorded `Dep` does
  not retain `AttrKeysObservationKind`, so a later proof encoder cannot
  distinguish `ProjectionPublish`, `Json`, `Xml`, `FunctionFormals`,
  result-visible `AttrNames`, and route-construction `AttrNames`.
- `recordContainerShapeDeps()` records `ProjectionPublish` keysets during
  cached-result publication. Those keysets are verifier/replay guard material,
  not authority to ignore directory children. This is the same category error
  as treating `ImplicitStructure #keys` as user-visible complete-keyset demand.
- Therefore the next implementation should not be another path whitelist. The
  minimum useful prototype is sideband provenance carried out of dep capture:
  for each structured directory keyset, record the observation kind and enough
  consumer context to classify it as result-visible, route-construction-only,
  or unknown. Unknown must behave like today's strict complete-keyset backstop.
- Start this as shadow telemetry, not acceptance: emit per-certificate counts
  and optional debug sidecar rows for directories that v11 would empty-scope,
  directories suppressed by complete keysets, and directories whose complete
  keyset observation kind is route-only/unknown/result-visible. Only after that
  agrees with the known v11/v7 deltas should `populateObservedByNameDirectoryScopes()`
  become a route-authorized scope builder.

### 2026-05-25 sideband provenance and child-fragment ablation

Implementation since the isolation fix:

- Added shadow keyset-provenance sideband metadata through dep capture and JSON
  projection publication. The sideband records complete directory keyset deps
  by observation kind and phase, then reports store-time counters only. It is
  not used as verifier authority.
- Fixed the sideband aggregation compile failure by avoiding `operator[]` on
  non-default-constructible provenance entries.
- Fixed child-fragment demand identity: `JsonOutputProjection::Fragment` now
  carries the child `pathId`, and `storeChildProofActions()` stores each child
  certificate under that child demand instead of the aggregate parent demand.

Validation and observations:

- `nix build -L --builders '' .#nix-cli` passed after the sideband fix.
- `cold-debug/72` used v11 observed-key proof, construction-keyset
  suppression, and by-name directory-listing relaxation on the two-commit
  `1cfdcfa5620e -> 34513330fcc1` list. It produced `19.0s` then `15.4s`.
  The duplicate still missed with `guardRejectExact = 1` on
  `pkgs/by-name/li/libvpx/package.nix`. Store telemetry showed seven complete
  directory keysets, all with metadata and all result-visible; none were
  navigation-only.
- `cold-debug/73` additionally enabled observed by-name package keys. It
  produced `19.0s` then `15.8s` and still missed with `guardRejectExact = 1`.
  It recorded many file-local observed-key candidates, but no observed-key guard
  for `libvpx/package.nix`; this falsifies the hypothesis that the existing v12
  package-key experiment already covers the current miss.
- `cold-debug/74` enabled child fragments before the child `pathId` fix. It
  produced `21.3s` then `17.8s`. Proof load attempted three certificates and
  all three rejected on the same exact `libvpx/package.nix` guard. Store-side
  proof deps grew to `98606` total because the aggregate plus two child
  certificates all carried broad direct deps.
- `cold-debug/75` reran child fragments after the child `pathId` fix. It built
  cleanly and produced `20.9s` then `17.3s`; the two JSON outputs are byte-for-
  byte identical. The three certificate keys are now distinct child/aggregate
  proof keys, with exact guard counts `3976`, `3977`, and `3981`. All three
  guards still contain `pkgs/by-name/li/libvpx/package.nix`, and the duplicate
  reports `proofJsonAction.load.attempts = 3`, `hits = 0`,
  `guardRejectExact = 3`, `store.attempts = 3`, and `store.deps = 98606`.
  Keyset provenance counters are `completeDirectoryKeysets = 21`,
  `withMetadata = 19`, `resultVisible = 19`, `navigationOnly = 0`, and
  `missingMetadata = 2`.

Causal read:

- The child `pathId` fix was necessary for proof-key correctness, but it is not
  a performance fix. It proves the current child-fragment prototype stores the
  right demand identities while still encoding the same over-broad exact source
  guards.
- Sideband keyset provenance is diagnostically useful but should remain
  telemetry. The current smoke data says the complete directory keysets in this
  workload are result-visible, and adversarial review found provenance can be
  incomplete when aggregate projections include previously accepted child
  projections.
- The current benchmark blocker is not lazy route navigation. It is exact
  `FileBytes` invalidation for changed by-name package files whose semantic
  derivation/output facts can remain unchanged for the requested JSON demand.
- Existing child fragments cannot beat the baseline on this case because they
  are still normal proof certificates over broad child direct deps. They need a
  narrower semantic sidecar or they only multiply guard verification and write
  overhead.

Design decision:

- Park route/keyset authorization as a secondary architecture lead until a
  workload shows positive-navigation cache hits are the dominant miss. It is
  still a useful abstraction for sound lazy projections, but it is not the
  limiting factor in the current benchmark gate.
- Do not promote sideband provenance to authority. Use it to reject bad
  hypotheses and to decide where complete-keyset proof may eventually be
  narrowed.
- Continue on package/outPath semantic sidecars or changed-package semantic
  verification. A useful prototype must store or derive facts that let a
  changed by-name package file be checked against the cached package output
  semantics without accepting arbitrary file-byte changes and without requiring
  a full `closures.gnome` fallback.

### 2026-05-25 adversarial review follow-up and no-stats telemetry gate

Parallel review results:

- Carson agreed the only plausible package-file recovery path is narrow and
  semantic, not a blanket weakening of `FileBytes`. The guard needs to report
  "rejected only by modified by-name `package.nix` exact paths", and recovery
  must then compare cached package output facts with the current package
  semantic result before accepting the aggregate JSON.
- Arendt agreed the child-fragment `pathId` fix is necessary but cannot solve
  the `1cfd -> 345` miss. Current child fragments are normal proof
  certificates over broad child direct deps, so they inherit the exact
  `libvpx/package.nix` guard and simply multiply the failed candidates.
- Hypatia found real no-stats overhead in the new provenance telemetry:
  source-site aggregation, sideband key construction, provenance merge/freeze,
  and proof-dep scans were all running even though `Counter` increments are
  disabled without `NIX_SHOW_STATS`.

Implementation response:

- Gated attr-key source-site counters and source-site maps on
  `Counter::enabled`.
- Gated `recordStructuredKeysetObservationKind()` on `Counter::enabled`, since
  the sideband is telemetry-only and not verifier authority.
- Gated keyset provenance merge/freeze and complete-directory-keyset counting
  on `Counter::enabled`.
- Gated `recordProofJsonActionKeysetProvenanceTelemetry()` before it builds
  metadata maps or scans proof deps.

Validation:

- `git diff --check` passed for the touched files.
- `nix build -L --builders '' .#nix-cli` passed with result
  `/nix/store/gfxcg95iam0sx6763s6kwvc66yqg1mq2-nix-2.35.0pre20260515_dirty`.
- No-debug/no-stats v11 two-commit smoke:
  `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_SUPPRESS_CONSTRUCTION_KEYSETS=1
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_RELAX_BY_NAME_DIRECTORY_LISTINGS=1
  nix run .#eval-trace-bench -- generate --commit-list
  benchmarks/eval-trace-bench/commit-lists/v11-tail-1cfd-345.txt --runs
  cold,hot` produced `cold/73` and `hot/73`.
- `cold/73`: `1cfdcfa5620e = 15.32s`, `34513330fcc1 = 12.23s`, mean
  `13.78s`.
- `hot/73`: `1cfdcfa5620e = 0.37s`, `34513330fcc1 = 0.41s`, mean `0.39s`.
- `eval-trace-bench runs --runs cold/73,hot/73 --reference cold/73` reported
  all outputs match.

Causal read:

- The telemetry gate is a cleanup for no-stats wall-time runs, not a hit-rate
  fix. It removes diagnostic work from the benchmark path while preserving
  debug-stat visibility when `NIX_SHOW_STATS` is enabled.
- The hot path on this pair is already comfortably below the old `0.88s`
  reference, but the cold path is still far above the `6.06s` reference because
  duplicate commits still fall back to full evaluation after exact package-file
  guard rejection.
- The next implementation work should target a cold-miss conversion:
  identify exact guard misses caused only by modified by-name package files,
  prove the relevant package output facts are semantically unchanged for the
  demanded projection, and then accept or stitch from cached JSON without full
  `closures.gnome` evaluation.

### 2026-05-25 by-name output-boundary recovery ablation

Adversarial review:

- Boyle rejected package-only `pkgs.<name>.drvPath` recovery as unsound. It
  proves only one default package derivation identity and misses overrides,
  `passthru`, `meta`, helper attrs, and hidden string context. Boyle also
  pointed out that the existing guard verifier short-circuits, so "rejected
  only by by-name package files" needs an exhaustive rejection classifier.
- Socrates agreed that re-evaluating and comparing the exact command output
  boundary is materially sounder than package-only recovery because it compares
  the cached JSON bytes and `NixStringContext`, but warned that it is close to
  tautological and unlikely to fix hot time.
- Heisenberg found the cheapest safe implementation seam: before
  `getOrCreateTraceCache()` in `InstallableAttrPath::tryServeJsonOutput()`,
  directly evaluate the file installable with the existing `EvalState`, resolve
  the same attr path, and render with `renderJsonInstallableOutput()`.

Implementation:

- Added the experimental flag
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_BY_NAME_OUTPUT_BOUNDARY_RECOVERY=1`.
  It is included in `proofJsonActionAuthorityMode()` and ignored in the routing
  env so the flag isolates proof keys without making every copied bench state
  miss its own env.
- Added an exhaustive text-guard classifier for the narrow candidate:
  every rejecting change must be ordinary `M` status for a canonical
  `pkgs/by-name/<prefix>/<name>/package.nix` exact guard path, with no
  remaining exact-overlap, directory, recursive, malformed, or `.git`-component
  rejection.
- Added a distinct recovery path in `tryLoadProofJsonActionCache()`. It does
  not pass rejected guard bytes as accepted guard bytes, so it does not mark
  changed file bytes as verified. Instead it requires the cert header and guard
  sidecar reference to match, directly re-renders the current output boundary,
  and returns only if current JSON and hidden context exactly equal the cached
  JSON and context.
- Added counters for output-boundary recovery attempts, hits, rejections, and
  time.

Validation:

- `git diff --check -- src/libcmd/installable-attr-path.cc
  src/libexpr/include/nix/expr/eval-trace/counters.hh
  src/libexpr/eval-trace/counters.cc src/libexpr/eval.cc` passed.
- `nix build -L --builders '' .#nix-cli` passed with result
  `/nix/store/4nj7n8yf9vaf22vgkv5zfhc6h6d42dd3-nix-2.35.0pre20260515_dirty`.
- Direct no-trace controls on `34513330fcc1`:
  full `closures.gnome` JSON took `9.099s`; child `outPath` checks took
  `4.109s` for `x86_64-linux` and `3.970s` for `aarch64-linux`; child
  `drvPath` checks took `4.037s` and `3.967s`.
- No-debug/no-stats two-commit recovery smoke produced `cold/74` and `hot/74`.
  `cold/74`: `1cfdcfa5620e = 15.54s`, `34513330fcc1 = 6.90s`, mean `11.22s`.
  `hot/74`: `1cfdcfa5620e = 0.37s`, `34513330fcc1 = 6.97s`, mean `3.67s`.
  `eval-trace-bench runs --runs cold/73,hot/73,cold/74,hot/74 --reference
  cold/74` reported all outputs match.
- Debug `cold-debug/76` confirmed the duplicate used the recovery path:
  `proofJsonAction.load.guardRejectExact = 1`,
  `outputBoundaryRecoveryAttempts = 1`,
  `outputBoundaryRecoveryHits = 1`,
  `outputBoundaryRecoveryRejected = 0`, and
  `outputBoundaryRecoveryUs = 7601196`.

Causal read:

- The recovery path converts the known duplicate cold miss from a traced
  fallback (`12.23s` in `cold/73`) into direct command-boundary evaluation
  (`6.90s` in `cold/74`). That is useful evidence: the exact by-name
  `package.nix` guard is a semantic false negative for this pair.
- It is not a baseline-beating design. Hot regresses from `0.41s` to `6.97s`
  for the duplicate because the accepted alias still points at the older cert
  whose guard rejects, so every hot lookup re-renders the full output boundary.
- Therefore this should remain a gated measurement/control path. The next
  performance-relevant design must store enough semantic output facts at
  publication time to make this check cheap on hot runs: for this workload,
  likely per-child system output facts (`closures.gnome.<system>.outPath`,
  hidden context, and required store-path availability) bound to the aggregate
  JSON/context, not package-only `drvPath` facts.

Follow-up lower-bound oracle:

- Ran the hardcoded unsafe exact-file drop oracle with
  `NIX_EVAL_TRACE_JSON_ACTION_ALLOW_UNSAFE_ABLATIONS=1` and
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DROP_V11_TAIL_PACKAGE_FILEBYTES=1`.
  This is not a sound design; it only answers "what if the `libvpx`/`newt`
  exact package-file rejects were discharged for free?"
- No-debug/no-stats `cold/75`: `1cfdcfa5620e = 15.86s`,
  `34513330fcc1 = 0.42s`, mean `8.14s`.
- No-debug/no-stats `hot/75`: `1cfdcfa5620e = 0.43s`,
  `34513330fcc1 = 0.42s`, mean `0.43s`.
- The oracle proves cheap discharge of the two package-file exact rejects is
  enough to restore excellent hot time and make the duplicate cold case cheap.
  It is still not enough to beat the old `6.06s` cold mean on this two-commit
  slice because first-publication cost remains about `15.5-15.9s`.

Additional source/proof observations:

- The actual changed package-file diffs are small:
  `libvpx` changes `env.NIX_LDFLAGS = toString [...]` to
  `NIX_LDFLAGS = [...]`; `newt` rewrites `env.NIX_LDFLAGS = toString (...)`
  to a string-valued `NIX_LDFLAGS`. Both leave the final output JSON and
  context unchanged for this workload.
- The stored v11 cert has only four retained deps and four
  `nixObservedKeyGuards`, all for top-level files:
  `pkgs/top-level/aliases.nix`, `all-packages.nix`,
  `python-packages.nix`, and `python-aliases.nix`. There is no observed-key
  guard for `pkgs/by-name/li/libvpx/package.nix` or
  `pkgs/by-name/ne/newt/package.nix`; those files are represented only by the
  omitted Git guard exact path set. This explains why the current v12
  by-name-package-key experiment did not affect the miss.
- Direct no-trace full-output controls with the current binary were
  `1cfdcfa5620e = 7.844s` and `34513330fcc1 = 9.099s`. That puts a hard floor
  under any first-publication path that fully evaluates `closures.gnome` in this
  worktree; beating the old `6.06s` cold reference requires either avoiding
  much of first evaluation or reducing benchmark/environment differences, not
  just optimizing proof writeback.

### 2026-05-25 exact-head memo after output-boundary recovery

Adversarial input:

- Noether argued that package-level semantic facts are still too broad without
  causality/slicing, but that a current-head output memo is bounded: it does not
  claim changed package file bytes are verified, it only records "this exact
  clean Git head and command boundary was freshly rendered once."
- This directly targets the `hot/74` failure mode. In `hot/74`, the alias still
  routed to the old cert, the by-name package-file exact guard still rejected,
  and output-boundary recovery re-rendered the whole command on every hot hit.

Implementation:

- Added `jsonActionCacheExactHeadFastMemoEnabled()`, currently gated by
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_BY_NAME_OUTPUT_BOUNDARY_RECOVERY=1`.
- `InstallableAttrPath::tryServeJsonOutput()` now checks
  `tryLoadJsonInstallableFastActionCache(gitActionKeys->fastKey)` before
  opening a trace session when the experiment is enabled.
- On any verified proof hit, output-boundary recovery hit, projection output,
  or final fallback render, it stores the exact-head result with
  `storeJsonInstallableFastActionCache(gitActionKeys->fastKey, output)`.
- This is an exact-head memo only. It is not a cross-commit proof and does not
  change the rejected guard into accepted path bytes.

Validation:

- `git diff --check -- src/libcmd/installable-attr-path.cc` passed.
- `nix build -L --builders '' .#nix-cli` passed with result
  `/nix/store/8z1dgjbl58a9l4qa6crcw59868nv5mws-nix-2.35.0pre20260515_dirty`.
- No-debug/no-stats two-commit run with observed-key proof, suppressed
  construction keysets, relaxed by-name directory listings, and output-boundary
  recovery produced `cold/76` and `hot/76`.
- `cold/76`: `1cfdcfa5620e = 15.33s`, `34513330fcc1 = 6.86s`, mean `11.10s`.
- `hot/76`: `1cfdcfa5620e = 0.35s`, `34513330fcc1 = 0.33s`, mean `0.34s`.
- `eval-trace-bench runs --runs cold/76,hot/76 --reference cold/76` reported
  all outputs match.

Causal read:

- The exact-head memo fixes the output-boundary recovery hot regression:
  `34513330fcc1` hot moved from `6.97s` in `hot/74` to `0.33s` in `hot/76`.
- The duplicate cold case remains the direct re-render cost (`6.86s`), which is
  roughly the same as `cold/74` and confirms this memo is only a hot-path fix.
- First-publication cost remains the blocking problem. `1cfdcfa5620e` is still
  `15.33s`, so the current design still cannot beat the old `6.06s` cold mean
  while the first publication forces the child outPaths under the traced
  projection publisher.

Adversarial follow-up and fixes:

- Schrodinger found that the first fast-memo prototype could serve when
  `NIX_ALLOW_EVAL=0`, because `NIX_ALLOW_EVAL` is excluded from the proof
  routing env and the fast loader did not check it. Fixed by refusing fast-memo
  loads when `NIX_ALLOW_EVAL=0`.
- Schrodinger also found that the fast memo could bypass the proof cache's
  missing-store fallback behavior: JSON would be written to stdout and only
  then `ensureLazyPathsCopied()` could fail. Fixed by checking the output
  context's referenced store roots before returning a fast memo and warning /
  falling back through the existing proof/fresh-eval path on missing deps.
- To avoid overstating proof performance, exact-head fast memos are no longer
  stored after ordinary traced projection publication or final fallback render.
  They are stored only after a proof/recovery hit, or by the explicit
  direct-output memo ablation below.
- Rebuilt after the fixes:
  `/nix/store/n8cnspv292x0mix5594d1nm30ppzc9bg-nix-2.35.0pre20260515_dirty`.

Post-fix validation:

- Recovery without direct-output memo produced `cold/78` and `hot/78`.
  `cold/78`: `1cfdcfa5620e = 15.93s`, `34513330fcc1 = 7.28s`, mean `11.60s`.
  `hot/78`: `1cfdcfa5620e = 0.40s`, `34513330fcc1 = 0.38s`, mean `0.39s`.
- The hot result remains sub-second after the fixes. It should now be read as
  mixed authority: first-head hot is proof-cache replay, duplicate-head hot is
  the exact-head memo written by successful output-boundary recovery.

### 2026-05-25 direct-output memo lower-bound ablation

Hypothesis:

- If the current cold miss is mostly traced projection publication, then a
  pre-session direct render of the command output plus exact-head memo should
  be materially faster than `cold/78`.
- This is not a cross-commit proof. Direct eval without a trace session does
  not record imported file bytes; `evalFile` intentionally records those only
  inside an active trace session. Therefore this ablation measures the cost
  floor of "compute the output directly" rather than a proof-authorized cache.

Implementation:

- Added explicit flag
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_DIRECT_OUTPUT_MEMO=1`, gated under the
  output-boundary recovery experiment.
- When enabled, `InstallableAttrPath::tryServeJsonOutput()` attempts
  `renderFileInstallableJsonOutputWithoutTraceSession()` before opening a trace
  session, stores the exact-head fast memo, and returns the freshly rendered
  output.

Validation:

- No-debug/no-stats run after fast-memo hardening produced `cold/79` and
  `hot/79`.
- `cold/79`: `1cfdcfa5620e = 10.79s`, `34513330fcc1 = 7.17s`, mean `8.98s`.
- `hot/79`: `1cfdcfa5620e = 0.39s`, `34513330fcc1 = 0.35s`, mean `0.37s`.
- `eval-trace-bench runs --runs cold/78,hot/78,cold/79,hot/79 --reference
  cold/78` reported all outputs match.

Causal read:

- The ablation improves first-publication cold from `15.93s` to `10.79s` on
  `1cfdcfa5620e`, so traced projection publication is a real multi-second tax.
- It is still well above the old `6.06s` cold target. Direct full evaluation of
  this command in the current harness is not enough to beat the baseline.
- Therefore a baseline-beating cold design needs either a way to avoid much of
  the first evaluation, or a proof publisher that captures only much cheaper
  command-boundary facts while retaining enough path-dependency authority.

### 2026-05-25 outPath publisher trace-overhead ablations

Question:

- The direct-output memo ablation avoids the traced projection publisher, but it
  also stops producing proof certs. To identify the expensive part of the traced
  publisher, try unsafe publication ablations that keep the normal command path
  but remove specific publisher work.

Implementation:

- Added unsafe flags inside `TraceSession::tryServeOutPathJsonObject()`:
  - `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_OUTPATH_SKIP_TRACE_ACTIVATION=1`
    skips the `TraceActivationScope` during outPath JSON publication.
  - `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_OUTPATH_SKIP_SHAPE_DEPS=1` skips
    `recordContainerShapeDeps(state, value)` in the outPath projection path.
- If either unsafe ablation is active, the publisher returns a projection with
  null dependency/keyset provenance so the action-cache proof writer does not
  publish a misleading certificate.

Validation:

- `cold/80`, skip trace activation only:
  `1cfdcfa5620e = 12.78s`, `34513330fcc1 = 7.97s`, mean `10.38s`.
- `cold/81`, skip shape deps only:
  `1cfdcfa5620e = 14.5s`, `34513330fcc1 = 11.0s`, mean `12.74s`.
- `cold/82`, skip both trace activation and shape deps:
  `1cfdcfa5620e = 11.4s`, `34513330fcc1 = 7.7s`, mean `9.54s`.
- Rebuilt the follow-up direct-output trace-context ablation as:
  `/nix/store/hnb1g6z71hg80x3sq1m1kvfls0y61q13-nix-2.35.0pre20260515_dirty`.

Causal read:

- Explicit shape-dependency recording is not the dominant cost. Skipping only
  shape deps was noisy and worse than the normal traced publisher.
- Trace-active evaluator/replay bookkeeping is a real cost, but removing the
  activation scope from the publisher still leaves cold mean above `9.5s` when
  combined with shape-dep skipping.
- The traced outPath publisher, as currently structured, is unlikely to beat
  the old `6.06s` cold baseline by local pruning alone.

### 2026-05-25 direct-output render with trace context disabled

Question:

- The direct-output memo still reuses the normal `EvalState`, whose `traceCtx`
  is populated even though no trace session has been opened yet. Measure whether
  temporarily disabling `state.traceCtx` during the direct render removes a
  meaningful amount of evaluator bookkeeping.

Implementation:

- Added
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_DIRECT_OUTPUT_DISABLE_TRACE_CTX=1`,
  gated under the direct-output memo experiment.
- When enabled, `renderFileInstallableJsonOutputWithoutTraceSession()` moves
  `state.traceCtx` aside before parsing/evaluating/rendering the command output
  and restores it with `Finally`.
- This is an ablation only: if the direct render fails after mutating evaluator
  caches, the subsequent fallback could observe state populated while tracing
  was disabled.

Validation:

- No-debug/no-stats run produced `cold/83` and `hot/83`.
- `cold/83`: `1cfdcfa5620e = 10.19s`, `34513330fcc1 = 6.59s`, mean `8.39s`.
- `hot/83`: `1cfdcfa5620e = 0.37s`, `34513330fcc1 = 0.33s`, mean `0.35s`.
- `eval-trace-bench runs --runs cold/79,hot/79,cold/83,hot/83 --reference
  cold/79` reported all outputs match.

Causal read:

- Disabling the trace context during direct rendering improves the direct-output
  cold mean from `8.98s` to `8.39s`, and improves the duplicate commit from
  `7.17s` to `6.59s`.
- That is useful signal, but it is too small to explain the gap to the old
  `6.06s` cold baseline. The remaining cost is mostly ordinary evaluation and
  output rendering, not just trace-context bookkeeping.
- Next viable search directions should focus on avoiding duplicate evaluation
  after the first commit, or capturing a much cheaper command-output proof
  during first publication. Further local trimming inside the current traced
  publisher is unlikely to be sufficient.

### 2026-05-25 adversarial review round: direct memo and proof authority

Subagent scope:

- Spawned three read-only review agents against the current checkout, not
  master:
  - proof/output-boundary/fast-memo soundness review
  - design-space review for baseline-beating candidates
  - benchmark harness/methodology review

Findings to carry forward:

- The exact-head fast memo is an exact-source serving tier, not a proof replay.
  It currently sits before certificate verification. Store-dependency checking
  and `NIX_ALLOW_EVAL=0` gating fixed the previously observed stale-store and
  no-eval issues, but a fast memo written from an untraced direct render is
  still an unsafe lower-bound artifact unless it has its own proof sidecar or
  is written only after an accepted proof/recovery cert.
- Output-boundary recovery hits should be interpreted as
  "fresh re-render equals cached output", not as cache replay. They are useful
  because they prove the changed package files are semantic false negatives for
  this workload, but they still pay evaluation cost.
- Context-store proof coverage needs tightening: publication used the output
  context closure to cover `StorePathAvailability` deps but only wrote context
  roots to the sidecar. Verification then checks only the sidecar. Either the
  sidecar must contain every covered closure member, or coverage must narrow to
  the roots actually written.
- The cert/guard base revision binding is too implicit. The guard says which
  base revision it protects; the cert also has `headRev`. Verification should
  require the accepted guard's base rev to match `cert.headRev` before using
  `cert.headRev` for observed-key changed-path filtering.
- Harness review confirmed normal `cold` and `reference` runs delete the whole
  per-run `_state` directory, while `hot` copies the full cold `_state`,
  including file-based fast/proof caches. The `clear_caches()` helper does not
  clear these directories, but it is used by warm priming, not normal cold/hot
  setup. After deleting results, run `0` only means baseline if the baseline is
  regenerated first.

Design-space consolidation:

- More projection routing or child-fragment machinery is not the current lead;
  earlier measurements show it mostly multiplies guards/candidates unless a
  compact proof authority already exists.
- The next useful experiment is direct command-output proof capture: render the
  command output directly, but under a narrow dep-capture and trace-activation
  scope, then store an aggregate proof cert. This should measure whether we can
  keep cross-commit proof replay while avoiding the full traced outPath
  projection publisher.
- If direct proof capture still records the same package-file exact deps, the
  remaining lead is a semantic replacement for those package-file `FileBytes`
  deps. If it drops or narrows them, it may become a real path to beating cold.

### 2026-05-25 direct-output proof-capture experiment

Hypothesis:

- The previous direct-output memo showed that skipping the traced outPath
  projection publisher reduces first-publication cost from about `15.9s` to
  about `10.2s`, but it does not publish a cross-commit proof.
- A narrower experiment should render the command output directly under
  `DepCaptureScope` + `TraceActivationScope`, store an aggregate proof cert,
  and avoid full traced projection publication.

Implementation:

- Added `TraceSession::withActiveSession()` so direct render can bind the
  current trace session without entering the projection publisher.
- Added
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_DIRECT_OUTPUT_PROOF_CAPTURE=1`,
  included in `proofAuthorityMode()` so its certs are isolated from the normal
  publisher.
- `tryServeJsonOutput()` now checks the proof cache before any direct-output
  ablation, then, on miss, can run direct proof capture and store an aggregate
  proof cert from captured deps.
- Tightened proof verification while in this area:
  - cert filename must match `cert.headRev`
  - accepted guard base rev must match `cert.headRev`
  - output-boundary recovery also requires the rejected guard's base rev to
    match `cert.headRev`
- Revisited the context-store proof sidecar after adversarial review. I kept
  root-only sidecar checks and documented the reason in code: valid Nix stores
  have the closure property, so checking the output context roots is the
  intended missing-store fallback boundary. A temporary closure-sidecar variant
  wrote `12,176` paths / `716 KB`; reverting to root sidecar restored the
  expected `160` lines / `9.3 KB`.

Validation:

- Built successfully:
  `/nix/store/5330f3zdnn7qskfbf7jkvx5b7pbgvd2d-nix-2.35.0pre20260515_dirty`.
- `cold/84`, direct proof capture before root-sidecar correction:
  `1cfdcfa5620e = 15.5s`, `34513330fcc1 = 7.1s`, mean `11.29s`;
  `hot/84` about `0.4s`.
- Rebuilt after root-sidecar correction and outPath renderer:
  `/nix/store/3wvjky2i81dlxckjh9kjbk0wcjvr5gpv-nix-2.35.0pre20260515_dirty`.
- `cold/86`, direct proof capture after correction:
  `1cfdcfa5620e = 15.5s`, `34513330fcc1 = 7.0s`, mean `11.24s`;
  `hot/86` about `0.4s`.
- The `cold/86` cert still has a `451,204` byte guard and exact guard entries
  for both:
  - `pkgs/by-name/li/libvpx/package.nix`
  - `pkgs/by-name/ne/newt/package.nix`
- It retains only the same four non-path deps and four observed-key guards as
  the traced publisher; direct proof capture did not produce a narrower proof.

Causal read:

- Activating trace/dependency capture during direct rendering reintroduces the
  expensive part of the path. It avoids explicit projection publication, but it
  still captures the same coarse imported-file facts and does not solve the
  package-file false negative.
- Direct proof capture is therefore not the baseline-beating path in its current
  form. The next proof has to be more selective, not merely captured from a
  different call site.

### 2026-05-25 direct outPath JSON renderer experiment

Hypothesis:

- The command output for this workload is an object mapping systems to
  derivation `outPath` strings:
  `{"aarch64-linux": "...nixos-system...", "x86_64-linux": "...nixos-system..."}`.
- Maybe output-boundary recovery/direct memo is slow because generic
  `printValueAsJSON()` probes attrset string coercion and recursively enters
  derivation attrsets.

Implementation:

- Added a narrow renderer used by the direct render helper:
  - only handles an attrset with no top-level `outPath`/`__toString`
  - each child must be an attrset with `outPath` and no `__toString`
  - preserves lexicographic attr order and string context
  - falls back to generic JSON for any unsupported shape

Validation:

- `cold/85`, direct-output memo + traceCtx-disabled lower bound with the narrow
  renderer: `1cfdcfa5620e = 10.4s`, `34513330fcc1 = 6.9s`, mean `8.65s`;
  `hot/85` about `0.36s`.
- This is not better than `cold/83` (`10.19s`, `6.59s`, mean `8.39s`).

Causal read:

- Generic JSON rendering is not the missing lever. The remaining direct render
  cost is ordinary nixpkgs evaluation needed to reach the two outPath strings,
  not recursive JSON formatting.
- Combined with the direct proof-capture result, this reinforces that the next
  viable lead is semantic discharge/slicing for the by-name package-file exact
  deps, not another renderer or publication-path variant.

### 2026-05-25 generic by-name package FileBytes drop oracle

Hypothesis:

- The hardcoded unsafe exact-drop oracle for
  `pkgs/by-name/li/libvpx/package.nix` and
  `pkgs/by-name/ne/newt/package.nix` showed the `34513330fcc1` duplicate can
  serve in about `0.4s`.
- That could still have been an overly specific coincidence. Generalizing the
  ablation to every canonical `pkgs/by-name/<prefix>/<name>/package.nix`
  exact `FileBytes` guard should produce the same result if the real missing
  proof is semantic by-name package discharge rather than those two literals.

Implementation:

- Added the unsafe experiment flag
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DROP_BY_NAME_PACKAGE_FILEBYTES=1`,
  gated by `NIX_EVAL_TRACE_JSON_ACTION_ALLOW_UNSAFE_ABLATIONS=1`.
- The helper now drops exact `FileBytes` guard paths for any canonical
  by-name `package.nix`, not just the two v11-tail files.
- This remains an oracle only. It is not sound because it assumes package file
  changes are irrelevant without proving the package/output facts consumed by
  the JSON result stayed unchanged.

Validation:

- Built successfully:
  `/nix/store/8l8lfchr3b3gswn8cjdv1akhwcx76d2w-nix-2.35.0pre20260515_dirty`.
- No-debug/no-`NIX_SHOW_STATS` two-commit tail run:
  `cold/87` and `hot/87`, with observed-key proof, construction keyset
  suppression, relaxed by-name directory listings, output-boundary recovery,
  and the generic unsafe by-name exact-drop ablation enabled.
- Timings:
  - `cold/87`: `1cfdcfa5620e = 15.48s`,
    `34513330fcc1 = 0.41s`, mean `7.94s`
  - `hot/87`: `1cfdcfa5620e = 0.35s`,
    `34513330fcc1 = 0.36s`, mean `0.35s`
- `eval-trace-bench runs` reported that all outputs match the reference.

Causal read:

- This reproduces the hardcoded unsafe oracle (`cold/75` mean `8.14s`,
  duplicate `0.42s`) without naming the two files.
- It also contrasts sharply with sound output-boundary recovery without exact
  memo (`cold/78`: `1cfd = 15.93s`, `345 = 7.28s`, mean `11.60s`).
- The duplicate miss is therefore not primarily JSON formatting, whole-output
  rendering, trace materialization, or proof-cache lookup overhead. The
  dominant false negative for this tail case is exact `FileBytes` rejection on
  canonical by-name package files whose consumed output facts appear unchanged.

Ten-commit upper-bound gate:

- Ran the same unsafe generic by-name exact-drop oracle over the normal
  10-commit window as `cold/88` and `hot/88`.
- Timings:
  - `cold/88` mean: `4.29s`
  - `hot/88` mean: `0.37s`
  - outputs match the reference.
- Cold outliers were the publication commits:
  - `7d7616894ec4 = 15.31s`
  - `fd3fbe0cfd62 = 12.12s`
  - `7fb36e13811a = 12.86s`
- The other seven cold commits served in `0.35s` to `0.40s`.

Interpretation against the old baseline:

- This unsafe upper bound is materially below the old pre-schema SQLite
  eval-cache baseline (`cold/938 ~= 6.06s`, `hot/938 ~= 0.88s`) on the same
  10-commit scale.
- That does not make the unsafe drop acceptable, but it means the design is not
  fundamentally incapable of beating the baseline. The baseline-beating path is
  specifically a proof-backed replacement for by-name package `FileBytes`
  rejection, not more whole-output recovery.
- A sound implementation still has to account for why package file changes are
  irrelevant to the JSON result. The oracle only says the performance budget is
  there if the proof can discharge that class cheaply.

Adversarial review consolidation:

- Direct proof capture was not perfectly isolated in the two-commit run because
  `tryServeJsonOutput()` checks existing proof/recovery before the direct
  capture path. That does not undermine the inspected first-publication cert,
  which still contains the same `libvpx`/`newt` exact guards, but future
  measurement should add counters or run a cleaner single-head publication.
- The narrow outPath renderer needs hit/fallback counters if it stays in the
  branch, but the negative timing result is consistent with the code: it can
  replace final JSON formatting only after ordinary nixpkgs evaluation has
  already reached the derivation outPath values.
- Existing provenance is still too coarse. The package files are recorded as
  ordinary import/file-content backstops, then moved into the Git guard exact
  path set. Keyset/observed-binding telemetry does not prove they were consumed
  only through derivation/output identity.
- The strongest next lead is a shadow `by-name-package-semantics-v1` proof:
  record package file path/name, system, old `drvPath`, output map/default
  `outPath`, output string context, and a facet mask showing the JSON result
  consumed only derivation/output identity. On load, discharge modified
  canonical by-name package exact guards only if current package facts match,
  the cert/head/env cache keys match, store context roots are available, and
  every unsupported facet fails closed.

### 2026-05-25 counter instrumentation and by-name semantic-proof review

Added cheap localized counters before trying another proof rule. The new stats
cover:

- narrow outPath renderer attempts/hits/fallback reasons,
- direct-output proof-capture attempts/successes/fallbacks/unstable deps,
- proof store guard composition: guard bytes, exact paths, by-name package
  exact paths, directory paths, recursive paths, retained deps,
- exact guard drops by changed path, overlap, and by-name package-file oracle,
- output-boundary recovery candidate path/package counts and cert rejections.

Build status:

- The modified `nix` binary compiled and was installed at
  `/nix/store/fznxxd229vcd8kmlpx7wa01gafixadfb-nix-2.35.0pre20260515_dirty`.
- Full `nix build .` / `nix build .#nix` continued past the binary build but
  failed in the functional-test derivation with existing broader failures.
  Therefore the current verification state is "binary built and benchmarked",
  not "full flake build passed".
- The `result` symlink was repointed to that binary output so the harness uses
  the counter-instrumented executable for diagnostics.

Counter diagnostic:

- Ran `cold-debug/77` on the two-commit tail with observed-key proof,
  construction keyset suppression, relaxed by-name directory listings,
  output-boundary recovery, and the unsafe generic by-name package FileBytes
  drop oracle enabled.
- First commit `1cfdcfa5620e` published a cert:
  - proof store deps: `36633`,
  - guard bytes: `451204`,
  - guard exact paths: `3981`,
  - by-name package exact paths: `913`,
  - directory paths: `1042`,
  - recursive paths: `3625`,
  - observed-key guards: `4`,
  - complete directory keysets with provenance: `7`.
- Duplicate commit `34513330fcc1` served from the cert:
  - proof load attempts/hits: `1/1`,
  - guard accepted: `1`,
  - exact drops: `2`,
  - by-name package FileBytes drops: `2`,
  - exact/directory/recursive hard rejects: `0`,
  - observed-key guards: `4`,
  - observed-key accepted: `1`, skipped unchanged: `3`,
  - observed-directory accepted-unobserved: `2`,
  - store-path checks: `157`,
  - proof load time: `92480us`, guard time: `72594us`, verify time `17300us`.

Causal read:

- The unsafe generic drop is taking exactly the path intended: two modified
  by-name package exact FileBytes guards are suppressed and no other guard
  class rejects. The duplicate is then a normal certificate hit.
- The publication cert still contains `913` by-name package exact paths, so the
  real proof problem is broad. The `libvpx`/`newt` tail case is one visible
  instance, not a special-case-only problem.
- Narrow outPath renderer counters stayed at zero in this diagnostic. It is not
  participating in the traced projection publisher path that produces the cert.
- The guard encode time on first publication (`828454us`) is visible but not the
  dominant multi-second cold miss. The hard miss is still first-time evaluation
  and derivation/output forcing; the proof work matters when deciding whether a
  repeated output can avoid that miss.

Adversarial semantic review:

- A naive "same `pkgs.<name>.drvPath`" or "same default outPath" proof is
  unsound. A changed by-name package file can leave the default derivation
  unchanged while changing `meta`, `passthru`, `override`, `overrideAttrs`,
  helper attrs, formals, output metadata, non-default outputs, or `__toString`
  behavior.
- Reconstructing `pkgs.${name}` later may inspect the wrong package instance.
  The target evaluation can use overlays, cross package sets, package scopes,
  splicing, or overridden callPackage environments. Any future package fact must
  be captured from the actual evaluated value/scope or be keyed by an equally
  strong route/scope identity.
- Store dependency availability remains a final gate. Semantic package equality
  cannot serve a cached JSON/context if the referenced store roots are missing;
  the existing root-context store check must stay in the load path.
- `NIX_ALLOW_EVAL=0` must fail closed for semantic discharge unless exact
  current-head semantic facts are already present under the full proof key.
- The sidecar, if promoted beyond diagnostics, must be hash/size-bound to the
  cert and guard and must bind proof key, repo root, head/base revs, authority
  mode, changed path set, schema/provider epochs, store dir, system/eval scope,
  and fact schema.

Refined next step:

- Do not implement drvPath equality as authority.
- Add a shadow diagnostic sidecar first. It should record, per JSON child
  fragment, which canonical by-name package `package.nix` FileBytes deps were
  in that child direct-dep slice and what JSON/context fragment they helped
  produce. This is not serving authority; it is evidence for whether the trace
  can localize the exact package deps to derivation-output projection routes.
- If that sidecar shows the changed exact FileBytes deps are localized to
  specific child outPath projections, the next proof design can require a
  first-class facet/projection record before discharging those exact paths. If
  the deps remain only global aggregate backstops, stop and choose a different
  lead rather than broadening the unsafe oracle.

### 2026-05-25 shadow by-name package semantics diagnostic

Implementation:

- Added diagnostic-only sidecars behind
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_BY_NAME_PACKAGE_SEMANTICS_DIAGNOSTIC=1`.
- The sidecar is written only after successful aggregate certificate
  publication. It is a sibling file named
  `*.json.by-name-package-semantics.json`.
- It explicitly records `"servingAuthority": false` and is not referenced by
  the certificate. It must not be used to authorize replay.
- Follow-up tightening added proof authority mode, cache/schema/provider/hash
  epochs, store dir, current system, and explicit per-fragment projection
  metadata (`derivation-output-json-string`, output `out`) so diagnostic files
  are less ambiguous when compared across runs.
- Contents:
  - proof key, repo root, head rev, cert filename/size/hash,
  - aggregate JSON/context hashes and aggregate by-name package FileBytes deps,
  - per child fragment: child name, path id, JSON/context hash, direct dep
    count, and by-name package FileBytes deps in that child direct-dep slice.

Build/validation:

- `git diff --check` passed for the touched implementation and log files.
- `nix build -L --builders '' .#nix-cli` passed and installed the current
  binary at
  `/nix/store/070szpwy87rjij1l2398insfrqxrpxby-nix-2.35.0pre20260515_dirty`.
- After the metadata tightening, `nix build -L --builders '' .#nix-cli` passed
  again with result
  `/nix/store/yx5v5vnlld0d3an0rbipndry560igkrc-nix-2.35.0pre20260515_dirty`.

Two-commit diagnostic:

- Command shape: two-commit `v11-tail-1cfd-345` cold run with observed-key
  proof, construction-keyset suppression, relaxed by-name directory listings,
  output-boundary recovery, unsafe generic by-name FileBytes drop, and the new
  diagnostic sidecar flag enabled.
- Auto-selected `cold/89`.
- Timings: `1cfdcfa5620e = 15.6s`, `34513330fcc1 = 0.4s`.
- The run published one diagnostic sidecar for the first certificate. Size:
  `492452` bytes.
- Sidecar counts:
  - aggregate deps: `36633`,
  - aggregate unique by-name `package.nix` FileBytes deps: `913`,
  - fragments: `aarch64-linux` and `x86_64-linux`,
  - `aarch64-linux` direct deps: `30984`, by-name package deps: `910`,
  - `x86_64-linux` direct deps: `30989`, by-name package deps: `912`,
  - aggregate unique by-name deps equals the union of fragment by-name deps.
- The two changed package files from the duplicate miss are present in both
  child fragment direct-dep slices:
  - `aarch64-linux`: `libvpx`, `newt`,
  - `x86_64-linux`: `libvpx`, `newt`.

Causal read:

- The package FileBytes facts are not merely aggregate/global guard noise; they
  are attached to direct child outPath publication slices for both systems.
  That supports the idea that a future proof could be scoped to child
  derivation-output projections rather than navigation.
- It does not prove a safe discharge rule yet. Each child direct-dep slice still
  carries roughly `31k` deps and `~911` by-name package exact file deps. The
  trace can localize "this package file was read while deriving this child's
  outPath JSON", but it still cannot prove "only derivation/output identity from
  this package was consumed".
- This points to a narrower next proof shape: cert-bound child projection facts
  with an explicit facet mask, not a broad package-name or path-shape rule. If
  that facet mask cannot be produced from the trace, package-file semantic
  discharge remains an unsafe oracle even though it has enough performance
  headroom.

Fresh parallel review consolidation:

- One reviewer confirmed the same boundary: the generic by-name FileBytes drop
  is intentionally unsound and must remain behind the unsafe gate. The
  narrowest defensible authority is "all guard failures are modified canonical
  by-name package files" plus a cert-bound semantic sidecar proving exactly the
  result-visible JSON/context facets are unchanged.
- A second reviewer ranked the sidecar as the correct next step and recommended
  adding a stats-only harness mode next, because current counter runs still
  require `--debug`.
- A third reviewer gave a clean baseline protocol: archive or delete the current
  `eval-trace-bench-results` before a clean comparison, build the baseline with
  `nix build -L --builders '' ".?rev=92a3df1ab706cf514357ae57b367050b373ff146"`,
  run baseline as numbered `cold/0`/`hot/0`, then restore the current `result`
  symlink and run the prototype as `cold/1`/`hot/1`. Do not compare debug
  wall times to baseline.

### 2026-05-25 stats-only harness mode

Implementation:

- Added `--with-stats` to `eval-trace-bench generate`.
- `--with-stats` sets `NIX_SHOW_STATS=1` and `NIX_SHOW_STATS_PATH` but does
  not pass `--debug` to Nix.
- Stats-only runs use distinct run names such as `cold-stats/0`, so they do
  not collide with wall-only `cold/0` or debug `cold-debug/0` runs.
- `--with-debug` continues to imply stats and still uses `*-debug` run names.
- Completion checks now require `stats.json` when either stats-only or debug
  mode is active. Wall-only runs still do not require stats.
- The run manifest records both `withDebug` and `withStats`; stats-only
  manifests use `"measurementMode": "stats"`.

Validation:

- `nix run .#eval-trace-bench -- generate --help` rebuilt the Python package,
  ran its install checks (`39 passed`), passed `ruff check`, and after
  formatting also passed `ruff format --check`.
- The help output exposes both `--with-debug` and `--with-stats`.
- Ran a two-commit stats-only cold diagnostic as `cold-stats/0` with the
  generic unsafe by-name FileBytes oracle:
  - first commit wall `18.64s`, second commit wall `0.43s`,
  - manifest: `withDebug=false`, `withStats=true`,
    `measurementMode="stats"`,
  - each commit directory has `stats.json`, `stderr.log`, and `timing.json`,
    with no `debug.log`,
  - timing files record `withDebug=false`, `withStats=true`,
  - second commit counters show the intended oracle path:
    `guardDropByNamePackageFileBytes=2`,
    `guardDroppedExactChanged=2`, `guardAccepted=1`,
    `storePathChecks=157`.

Use:

- Use stats-only runs for counter attribution when wall-time distortion from
  `--debug` would obscure the signal.
- Still keep final baseline/prototype comparisons wall-only and no
  `NIX_SHOW_STATS`.

Stats-only output-boundary recovery control:

- Ran `cold-stats/1` on the same two-commit tail with observed-key proof,
  construction-keyset suppression, relaxed by-name directory listings, and
  output-boundary recovery, but without the unsafe by-name FileBytes drop.
- Timings:
  - `1cfdcfa5620e`: `18.74s`,
  - `34513330fcc1`: `7.50s`.
- Counter read for `34513330fcc1`:
  - one candidate check,
  - exact guard reject count `1`,
  - output-boundary recovery candidate packages/paths: `2/2`,
  - output-boundary recovery attempts/hits: `1/1`,
  - recovery time: `7050941us`,
  - total proof-action load time: `7111159us`,
  - narrow outPath renderer attempts/hits: `1/1`, children `2`.

Causal read:

- The narrow outPath renderer does work in the recovery path and succeeds, but
  the recovery still costs about `7.05s` because it must re-evaluate far enough
  to produce the two child `outPath` strings. This confirms again that JSON
  rendering is not the missing performance lever.
- The unsafe oracle's `0.4s` duplicate hit comes from avoiding the recovery eval
  entirely, not from a faster recovery renderer. Any sound replacement must be
  an accepted proof/load path, not another full-current-output recomputation.

Harness hardening after adversarial review:

- Fixed stats run discovery so `cold-stats/N` and `hot-stats/N` classify like
  their wall-only counterparts instead of disappearing from comparison logic.
- Fixed phase12 compare hints to identify the reference run through normalized
  run mode, so future `reference-stats`/`reference-debug` names do not confuse
  the hint text.
- Added a manifest/resume compatibility guard that rejects an existing run
  directory when the benchmark subject, source commit list, eval args,
  measurement mode, or environment provenance differs. This prevents stale
  per-commit outputs from being silently reused across different subjects.
- Tightened that guard after review: it now builds the planned manifest in
  memory, compares it with any existing manifest, and aborts before writing.
  The earlier version detected the mismatch but could overwrite the previous
  manifest before exiting, which protected execution but damaged provenance.
- Added a regression test for that no-overwrite behavior.
- Validation: `nix build -L --out-link /tmp/eval-trace-bench-harness-check
  .#eval-trace-bench` passed with `42 passed`, `ruff check`, `ruff format
  --check`, and `pyright`.

Fresh adversarial review findings and fixes:

- Found a real unsafe-authority leak: exact-head fast memo keys were based on
  exact source identity but did not include `proofJsonActionAuthorityMode()`.
  An unsafe by-name FileBytes oracle run could therefore seed a fast memo that
  a later safe run at the same exact head would read before proof verification.
- Fixed by bumping the fast memo material header to v3 and adding
  `proofJsonActionAuthorityMode()` to the fast key. Unsafe oracle memo entries
  are now isolated from safe proof runs in the same way proof certs already
  were.
- Validation: `nix build -L --builders '' .#nix-cli` passed after the fast-key
  authority change. Result path:
  `/nix/store/fdv97ll8229d62z05kxdshz3cfnxqmdf-nix-2.35.0pre20260515_dirty`.
- Harness review found two benchmark-validity hazards:
  - `hot` copied `_state` from `--hot-from` without checking that the source
    manifest matched the planned hot run except for the run name.
  - stateful reruns could reset `_state` and then skip completed per-commit
    outputs, producing a run where timings/JSON and cache state no longer
    corresponded.
- Fixed by requiring hot source manifests to match benchmark subject, source
  window, eval args, environment provenance, and measurement mode; requiring
  the source run to have complete outputs for the planned commit list; rejecting
  unmanifested output directories; and rejecting output reuse for stateful
  cold/hot/warm runs. This intentionally makes interrupted stateful reruns
  fail closed until we add a real progress/state sentinel.
- Validation after these harness guards: `nix build -L --out-link
  /tmp/eval-trace-bench-harness-check .#eval-trace-bench` passed with
  `46 passed`, `ruff check`, `ruff format --check`, and `pyright`.

Next diagnostic refinement:

- Added a load-path rejection diagnostic sidecar, gated by the existing
  by-name package semantics diagnostic env var. When a cert's git guard rejects
  only modified canonical `pkgs/by-name/*/*/package.nix` files and
  output-boundary recovery is considered, it writes
  `*.by-name-package-rejection.<current-head>.json`.
- The sidecar records `servingAuthority=false`, proof key, current/base/cert
  revisions, cert and guard hashes, rejected package files, cached output
  hash/context, and current recovery output hash/context when available.
- This is intentionally not an authority path. It exists to answer the next
  causal question: do the rejected by-name file changes leave the actual
  result-visible JSON/context unchanged, and can we line those rejections up
  with child outPath facets later without re-running output-boundary recovery?
- Validation: `nix build -L --builders '' .#nix-cli` passed after the
  rejection-diagnostic sidecar change. Result path:
  `/nix/store/nn6xzkbi73723zrfzzl40cgpvqbi3h15-nix-2.35.0pre20260515_dirty`.

Diagnostic run:

- Ran `cold-stats/2` on the two-commit `v11-tail-1cfd-345` list with
  observed-key proof, construction-keyset suppression, relaxed by-name
  directory listings, output-boundary recovery, and by-name diagnostics. No
  unsafe drop flags were enabled.
- Timings: `1cfdcfa5620e` `18.6s`; `34513330fcc1` `7.3s`.
- Outputs match byte-for-byte between the two commits.
- Manifest subject:
  `/nix/store/nn6xzkbi73723zrfzzl40cgpvqbi3h15-nix-2.35.0pre20260515_dirty`;
  measurement mode `stats`.
- Rejection diagnostic was written:
  `*.by-name-package-rejection.34513330fcc124d7f204d4621cb71f3e42d59020.json`.
  It records `outcome=accepted-by-output-boundary-recovery`, rejected files
  `pkgs/by-name/li/libvpx/package.nix` and
  `pkgs/by-name/ne/newt/package.nix`, and identical cached/current
  JSON/context hashes (`146be86a...`, size `208`).
- Counters for `34513330fcc1`: candidate checks `2`, recovery candidate
  paths/packages `2/2`, recovery attempts/hits `1/1`, recovery time
  `6848679us`, total proof-action load `6909905us`, unsafe drop counters `0`.
- Interpretation: the new sidecar confirms the local semantic fact we need for
  this pair: the only exact guard failures were two modified canonical by-name
  package files and the result-visible JSON/context did not change. It still
  does not prove that the package-file deps were consumed only by child
  `outPath` facets; the semantics diagnostic continues to show broad child
  direct-dep slices (`~31k` direct deps and `910+` by-name deps per system).
- Refined the rejection sidecar to summarize the cached semantics diagnostic
  when present. For each rejected package file, it records whether the aggregate
  diagnostic contained the path and which cached child fragments contained it,
  including fragment direct-dep and by-name-dep counts. This is still telemetry,
  but it makes the missing proof obligation visible in one file.
- Validation: `nix build -L --builders '' .#nix-cli` passed after the coverage
  summary addition. Result path:
  `/nix/store/af99pbnmg3vw3zgv0xj8m0zxjmn70pmd-nix-2.35.0pre20260515_dirty`.
- Reran the two-commit diagnostic as `cold-stats/3`. Timings were again
  `18.5s` then `7.3s`; outputs matched byte-for-byte. Counters for the second
  commit: recovery attempts/hits `1/1`, recovery time `6894314us`, total
  proof-action load `6962591us`, unsafe drop `0`.
- The new coverage summary shows both rejected package files in the aggregate
  and in both child fragments:
  - `aarch64-linux`: `30984` direct deps, `910` by-name package file deps,
  - `x86_64-linux`: `30989` direct deps, `912` by-name package file deps.
- Interpretation strengthened: the current proof deps are much too coarse for
  sound by-name discharge. A useful next prototype must either capture a
  narrower child `outPath` dependency slice or prove a stronger package/output
  semantic fact. Simply knowing "the rejected file appears in the child
  fragment" is not selective enough.

### 2026-05-25 second adversarial review round and hardening

Fresh reviewer findings:

- Harness: analysis commands could still compare incompatible run manifests
  even after `generate` became stricter. This could make `runs`/`pairwise`
  compare different measurement modes, source windows, or stateful processing
  orders if the directories happened to overlap.
- Harness: `runs --reference` could silently fall back to the first discovered
  run if the requested reference was missing.
- Harness: stale stats could still be loaded when a run lacked a readable
  manifest but had a generated `timing.json` that said stats were disabled.
- C++: exact-head fast memo was still too broad. Even after adding
  `proofJsonActionAuthorityMode()` to the key, proof-backed hits and
  output-boundary recovery hits could populate the fast memo and later bypass
  the full proof store-path availability sidecar.
- C++: observed-key guards substituted for exact file deps but lacked the same
  symlink-component rejection gate as normal Git guards.
- C++: certificate filename scanning was SHA-1-shaped only. That filtered
  diagnostics correctly but would skip Git SHA-256 repositories.
- Design: the projection/outPath machinery is still not causal by itself. It
  publishes replay facts after forcing values; it does not prove why changed
  source bytes are irrelevant. The promising path remains cert-bound semantic
  facets or first-class construction/result-visible provenance, not a faster
  JSON renderer.

Fixes applied:

- Added `eval_trace_bench.compat` and staged it so Nix source snapshots include
  the new module.
- `runs` and `pairwise` now reject incompatible analysis inputs:
  - comparable shape keys must match (`suite`, `workloads`, source repo/ref,
    debug/stats mode, measurement mode, extra eval args),
  - `benchmarkSubject` and `benchmarkEnvironment` are intentionally not
    compared because baseline-vs-prototype is the intended A/B dimension,
  - stateful `cold`/`hot`/`warm` runs require identical manifest
    `commitOrder` unless `--allow-intersection` is passed.
- `runs` now errors when the requested `--reference` is absent instead of
  silently using the first run.
- Warm runs now remove the whole `_state` tree at run start, covering stale
  cache, local store, eval store, state, and log directories.
- Stats loading now treats generated timing metadata as authoritative when the
  manifest is missing: `withStats=false` ignores stray `stats.json`, and
  `withStats=true` requires it. Timing/manifest `withStats` disagreement is a
  load error.
- Fast memo hardening:
  - bumped the fast key header again to v4, invalidating earlier v3 entries,
  - stopped storing proof-cache hits and output-boundary recovery hits into the
    exact-head fast memo,
  - direct-output memo experiment remains the only writer for the fast memo.
- Output-boundary recovery now checks the cert's store-path availability facts
  before running the expensive recovery eval. If expected-valid store deps are
  missing, it fails closed and the existing missing-store warning path fires.
  This recovery path still returns freshly evaluated output only; it is not
  promoted to cache authority.
- Observed-key proof now rejects files with symlink components both when
  writing guards and when loading existing guards, before deciding an unchanged
  Git path can skip verification.
- Certificate candidate filtering now accepts 40- and 64-hex object ids with
  `.json`, keeping diagnostic sidecars out of scans while allowing Git SHA-256
  repositories.

Validation:

- `nix build -L --out-link /tmp/eval-trace-bench-harness-check
  .#eval-trace-bench` passed with `59 passed`, `ruff check`, `ruff format
  --check`, and `pyright`.
- `nix build -L --builders '' --out-link /tmp/nix-cli-proof-check .#nix-cli`
  passed after the C++ hardening. Result:
  `/nix/store/d5cpxc2v3w12asfcglb6dl9la3z7afm7-nix-2.35.0pre20260515_dirty`.

Current design conclusion:

- The unsafe by-name `FileBytes` oracle remains the performance upper bound,
  not an implementation shape. It proves there is enough headroom if the two
  changed package files can be discharged without current-output recovery.
- Output-boundary recovery is now safer diagnostics but remains too slow: it
  re-evaluates to compute current child outPaths.
- The next implementation lead should be diagnostic-only dep/facet slicing at
  the derivation-output publication boundary. The falsifier is straightforward:
  if the recorded facet deps still include the same `~31k` child deps and
  `~911` by-name exact file deps, then child `outPath` publication is not the
  path to a sound oracle replacement.

Post-hardening smoke diagnostic:

- Updated `result` to the hardened CLI build:
  `/nix/store/d5cpxc2v3w12asfcglb6dl9la3z7afm7-nix-2.35.0pre20260515_dirty`.
- Ran the same two-commit stats diagnostic as `cold-stats/4` with observed-key
  proof, construction-keyset suppression, relaxed by-name directory listings,
  output-boundary recovery, and by-name diagnostics. No unsafe drop flags were
  enabled.
- Timings: `1cfdcfa5620e` `18.5s`; `34513330fcc1` `7.3s`.
- Outputs matched byte-for-byte.
- Relevant second-commit counters:
  - proof JSON action load attempts/hits/misses: `1/1/0`,
  - candidate checks/candidates: `1/1`,
  - guard exact rejects: `1`,
  - observed directory checks accepted as unobserved: `2/2`,
  - output-boundary recovery paths/packages: `2/2`,
  - output-boundary recovery attempts/hits/rejections: `1/1/0`,
  - output-boundary recovery time: `6894675us`,
  - total proof-action load time: `6959452us`,
  - store path checks/query: `157` / `642us`,
  - unsafe by-name drop: `0`.
- Rejection diagnostic outcome remained
  `accepted-by-output-boundary-recovery`; cached/current JSON hash stayed
  `146be86a564d9d7e9360f6b7b1b869f9a3398e9f29f82ec685348b4a3aeba7a4`.
- Coverage remained broad: both rejected files were present in the aggregate
  and both child fragments, with `30984`/`30989` direct deps and `910`/`912`
  by-name package file deps. This preserves the previous conclusion: the
  current child fragment deps are diagnostic evidence, not a discharge rule.

### 2026-05-25 outPath facet split falsifier

Implementation notes:

- Added env-gated diagnostic splitting for child `outPath` publication under
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_OUTPATH_DEP_FACETS`.
- The normal path remains the old single `DepCaptureScope`; phase splitting is
  only active when the experiment flag is set.
- Split phases:
  - child construction: `forceValue(child)` plus the derivation decision,
  - child `outPath` force,
  - container shape publication.
- Added observational and marginal counters for each phase. Marginal counters
  subtract exact deps already seen in earlier phases so replayed deps do not
  look like new phase-local causes.
- Fixed the first adversarial review issue by moving `isDerivation()` into the
  construction capture scope.
- Fixed the second adversarial review issue by including
  `outpath-dep-facets` in `proofJsonActionAuthorityMode()` and excluding
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_OUTPATH_DEP_FACETS` from the generic
  impure env scope. Diagnostic certs now live in a distinct proof-key
  namespace.

Adversarial review findings incorporated:

- Phase splitting is not a pure causal proof because separate capture scopes
  can replay memoized deps that a single scope would suppress. The marginal
  counters reduce duplicate exact deps but remain diagnostic, not authority.
- The child fragment sidecar still stores merged child deps, not separate facet
  deps. It can show that a rejected package file is in the child slice, but not
  that it belongs to the `outPath` facet.
- Counters are attempt counters. In the current successful benchmark this is
  harmless, but the names should not be read as "published cert only" counters
  without checking `successes`.

Validation:

- `nix build -L --builders '' --out-link /tmp/nix-cli-dep-slice-check .#nix-cli`
  passed after marginal counters and authority-key isolation. Result:
  `/nix/store/18j7mjqh29s4fqqfmjdmb01ka5zl41y2-nix-2.35.0pre20260515_dirty`.
- Updated `result` to that build before measuring.
- Ran:
  - `cold-stats/5`: obsolete/contaminated diagnostic before authority-key
    isolation; kept only as historical evidence.
  - `cold-stats/6`: clean diagnostic with the authority key isolated.

Clean diagnostic command shape:

```console
env -u NIX_SHOW_STATS -u NIX_SHOW_STATS_PATH \
  NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1 \
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_SUPPRESS_CONSTRUCTION_KEYSETS=1 \
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_RELAX_BY_NAME_DIRECTORY_LISTINGS=1 \
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_BY_NAME_OUTPUT_BOUNDARY_RECOVERY=1 \
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_BY_NAME_PACKAGE_SEMANTICS_DIAGNOSTIC=1 \
  NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_OUTPATH_DEP_FACETS=1 \
  nix run .#eval-trace-bench -- generate \
    --runs cold \
    --with-stats \
    --run-number 6 \
    --commit-list benchmarks/eval-trace-bench/commit-lists/v11-tail-1cfd-345.txt
```

Run `cold-stats/6` results:

- Wall times:
  - `1cfdcfa5620e`: `20.346670622s`,
  - `34513330fcc1`: `7.320107597s`.
- Outputs matched byte-for-byte.
- First commit outPath publication counters:
  - `children`: `2`,
  - `childDirectDeps`: `61973`,
  - `childDirectCompleteKeysets`: `14`,
  - `childConstructionDeps`: `61973`,
  - `childConstructionByNamePackageFileBytes`: `1822`,
  - `childConstructionCompleteKeysets`: `14`,
  - `childConstructionMarginalDeps`: `61973`,
  - `childConstructionMarginalByNamePackageFileBytes`: `1822`,
  - `childConstructionMarginalCompleteKeysets`: `14`,
  - `childOutPathForceDeps`: `2995`,
  - `childOutPathForceByNamePackageFileBytes`: `40`,
  - `childOutPathForceMarginalDeps`: `0`,
  - `childOutPathForceMarginalByNamePackageFileBytes`: `0`,
  - `childOutPathForceMarginalCompleteKeysets`: `0`,
  - `childShapeDeps`: `0`,
  - `childShapeMarginalDeps`: `0`.
- Second commit proof load counters:
  - output-boundary recovery attempts/hits/rejections: `1/1/0`,
  - output-boundary recovery time: `6889518us`,
  - total proof-action load time: `6948924us`,
  - store path checks/query: `157` / `643us`,
  - unsafe by-name drop: `0`.
- The diagnostic sidecar proof authority mode includes
  `outpath-dep-facets=1`, confirming the namespace split took effect.

Interpretation:

- This falsifies child `outPath` boundary slicing as the primary
  baseline-beating mechanism. All marginal by-name `package.nix` `FileBytes`
  came from child construction; the `outPath` phase only replayed already-seen
  deps and added zero marginal deps.
- The fast unsafe oracle was fast because it ignored broad construction deps,
  not because the `outPath` facet was naturally narrow.
- Continuing to optimize output-boundary recovery would mostly optimize a slow
  recomputation fallback. It does not create the accepted proof path needed to
  beat the baseline.
- Next search direction: first-class construction-time provenance/deferred
  dependency activation. We need to prove which construction deps are route or
  index construction only, and only activate by-name package `FileBytes` when
  the corresponding package/value is actually forced into a result-visible
  facet. Negative membership, `attrNames`, suggestions, and serialization must
  continue to require complete keyset authority.

### 2026-05-25 by-name package semantic oracle experiment

Hypothesis tested:

- If the only guard rejection is modified canonical by-name package files, we
  might validate those packages' derivation outputs directly instead of
  re-rendering the full `closures.gnome` JSON.
- The validation fact tested was intentionally narrow: for each rejected package
  name and output system, compare old/current `drvPath`, output names, and all
  output paths.

Implementation notes:

- Added a targeted semantic evaluator for rejected by-name packages.
- The first version evaluated the cached revision using `builtins.fetchGit`.
- The second version materialized the cached revision as a local detached Git
  worktree under the proof-cache state directory and imported that path
  directly.
- After adversarial review, the shortcut was moved behind unsafe diagnostic
  gates only:
  - `NIX_EVAL_TRACE_JSON_ACTION_ALLOW_UNSAFE_ABLATIONS=1`,
  - `NIX_EVAL_TRACE_JSON_ACTION_UNSAFE_BY_NAME_SEMANTIC_RECOVERY=1`.
- The normal safe path still renders current command JSON/context and accepts
  only exact equality. Unsafe semantic hits are now labeled as
  `accepted-by-unsafe-by-name-semantic-recovery-oracle`, not ordinary
  output-boundary recovery.
- Cleared the diagnostic by-name derivation facet vector at the start of each
  JSON output attempt to avoid stale cross-run sidecar pollution in long-lived
  processes.

Runs and observations:

- `cold-stats/11`: unsafe semantic shortcut using `builtins.fetchGit`.
  - `1cfd`: `20.6s`.
  - `345`: `31.3s`.
  - `byNameSemanticRecoveryAttempts/Hits`: `1/1`.
  - `byNameSemanticRecoveryUs`: `30852894us`.
  - Interpretation: source realization through `builtins.fetchGit` in the
    benchmark store is much worse than full output-boundary recovery.
- Manual probe with a detached worktree for `1cfd`:
  - `git worktree add --detach`: about `2.0s` one-time cost.
  - package facet eval from worktree: about `0.42s`.
- `cold-stats/12`: safe default after gating the oracle.
  - `1cfd`: `20.5s`.
  - `345`: `7.4s`.
  - `byNameSemanticRecoveryAttempts/Hits`: `0/0`.
  - `outputBoundaryRecoveryUs`: `6914812us`.
  - Interpretation: safe behavior is restored; no semantic shortcut is used by
    default.
- `cold-stats/13`: explicit unsafe worktree oracle.
  - `1cfd`: `20.7s`.
  - `345`: `2.9s`.
  - `byNameSemanticRecoveryAttempts/Hits`: `1/1`.
  - `byNameSemanticRecoveryPackageChecks`: `8`.
  - `byNameSemanticRecoveryUs`: `2465522us`.
  - Interpretation: avoiding `fetchGit` cuts the oracle from `~30.9s` to
    `~2.47s`, but it is still far above the old `hot/938 ~0.88s` target and
    remains unsafe as serving authority.

Adversarial review outcome:

- Equal `drvPath` and output paths for reconstructed `pkgs.<name>` do not prove
  the cached command JSON/context is unchanged. A package file can change
  `meta`, `passthru`, helper attrs, formals, or overlay-sensitive behavior while
  leaving derivation outputs unchanged.
- Reconstructing `pkgs = import src { inherit system; }` is not necessarily the
  same package instance or scope used by the cached result.
- Inferring systems from output JSON shape is workload-specific. It works for
  the current `closures.gnome` object, but is not proof provenance.
- Some by-name package files are imported as broad construction/source coverage
  without evidence that their derivations were originally constructed in the
  result-visible slice. Constructing `pkgs.<name>` after the fact can prove a
  different evaluation.
- Therefore the semantic shortcut is an oracle only. It is useful evidence that
  package/output facts can replace full output-boundary recovery if they are
  captured and bound to the certificate, but it cannot safely serve cached JSON
  by itself.

Next design direction:

- Promote package/output facts from diagnostic sidecars into authority-grade,
  cert-bound per-fragment facts only if the original result capture records the
  actual package route/scope, system, fragment path id/name, JSON/context digest,
  `drvPath`, output names, output paths, and required store roots.
- Do not re-evaluate the cached revision on every guard rejection. Store the
  cached facet facts with the certificate and compare only current facts when
  needed.
- The stronger architecture is still deferred construction activation: broad
  construction deps should not be active guard deps unless their activated
  result-visible facet depends on them. Unknown or mixed provenance remains
  active and falls back.

Follow-up run after relabeling the unsafe oracle:

- `cold-stats/14`: explicit unsafe worktree oracle with separated outcome label.
  - `1cfd`: `20.6s`.
  - `345`: `3.0s`.
  - `byNameSemanticRecoveryAttempts/Hits`: `1/1`.
  - `byNameSemanticRecoveryPackageChecks`: `8`.
  - `byNameSemanticRecoveryUs`: `2511917us`.
  - `outputBoundaryRecoveryHits`: `0`.
  - `outputBoundaryRecoveryUs`: `2511922us`.
  - Rejection diagnostic outcome:
    `accepted-by-unsafe-by-name-semantic-recovery-oracle`.
  - Interpretation: the relabeling worked. The shortcut no longer appears as
    an ordinary output-boundary recovery hit. The causal picture did not change:
    reconstructing package facets from source is faster than full command
    recovery but still too slow and still not authority.

### 2026-05-25 deferred construction by-name FileBytes ablation

Hypothesis:

- The by-name `package.nix` exact guards that cause the `1cfd -> 345` miss are
  construction-phase backstops for child derivations, not result-visible outPath
  facts.
- If those construction-only by-name `FileBytes` deps are inactive for this JSON
  outPath projection, publication should produce a cert whose guard no longer
  rejects on the changed `libvpx`/`newt` files. The duplicate should then be an
  ordinary proof hit, with no output-boundary recovery and no semantic oracle.

Implementation:

- Added unsafe experiment flag
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_OUTPATH_DEFER_CONSTRUCTION_BY_NAME_FILEBYTES`,
  gated by `NIX_EVAL_TRACE_JSON_ACTION_ALLOW_UNSAFE_ABLATIONS`.
- The flag is included in `proofJsonActionAuthorityMode()` and excluded from the
  generic impure env routing scope so these certs are cache-key isolated.
- In the outPath dep-facet path, construction deps are held pending until
  outPath-force and shape deps are captured. The ablation then filters only
  construction-phase canonical by-name package `FileBytes` whose dep key was not
  reactivated by the later result-visible phases.
- Added counters:
  - `childConstructionDeferredByNamePackageFileBytesCandidates`,
  - `childConstructionDeferredByNamePackageFileBytes`,
  - `childConstructionActivatedByNamePackageFileBytes`.

Validation:

- `nix build -L --builders '' --out-link /tmp/nix-cli-defer-construction-by-name
  .#nix-cli` passed. Result:
  `/nix/store/g72kvjmflp7iiwfpj5a6pdkbc2kqwlai-nix-2.35.0pre20260515_dirty`.
- Initial stats run `cold-stats/15`:
  - `1cfd`: `20.6s`,
  - `345`: `0.4s`,
  - outputs byte-identical,
  - second commit load: `guardAccepted=1`, `outputBoundaryRecoveryAttempts=0`,
    `byNameSemanticRecoveryAttempts=0`, `guardDropByNamePackageFileBytes=0`,
    `totalUs=87094us`.
- Refined the counters to distinguish filtered vs reactivated construction deps.
- `nix build -L --builders '' --out-link
  /tmp/nix-cli-defer-construction-activated .#nix-cli` passed. Result:
  `/nix/store/99gcaq90jbvn6dzkyz7vccdqf34rb88k-nix-2.35.0pre20260515_dirty`.
- Refined stats run `cold-stats/16`:
  - `1cfd`: `20.483059751975816s`,
  - `345`: `0.42018594697583467s`,
  - outputs byte-identical.
- First commit outPath/proof counters in `cold-stats/16`:
  - construction by-name candidates: `1822`,
  - filtered construction-only by-name deps: `1782`,
  - reactivated by-name deps: `40`,
  - outPath-force by-name deps: `40`,
  - outPath-force marginal by-name deps: `0`,
  - published proof deps: `35740`,
  - guard exact paths: `3835`,
  - guard by-name package exact paths: `767`.
- The published guard contains no `pkgs/by-name/li/libvpx/package.nix` or
  `pkgs/by-name/ne/newt/package.nix` exact entries.
- Second commit load counters in `cold-stats/16`:
  - proof load hit: `1`,
  - `guardAccepted=1`,
  - `guardRejectExact=0`,
  - `guardDropByNamePackageFileBytes=0`,
  - `outputBoundaryRecoveryAttempts=0`,
  - `byNameSemanticRecoveryAttempts=0`,
  - `totalUs=87572us`.

Causal read:

- This is the strongest evidence so far for the deferred-construction direction.
  It reproduces the `~0.4s` duplicate behavior of the unsafe load-time guard drop
  while avoiding any load-time source-dep suppression, output-boundary recovery,
  or package-facet semantic oracle.
- The changed `libvpx`/`newt` files are among the `1782` construction-only
  by-name deps filtered at publication. Once they are not active guard deps, the
  duplicate commit is an ordinary proof hit.
- This is still not authority. The ablation suppresses construction source deps
  without a persisted proof that they are irrelevant to every result-visible
  semantic facet. It is useful because it identifies the smallest production
  target: make construction deps inactive by default only when a cert-bound
  activation proof explicitly shows the result-visible projection did not consume
  those source facts.

Next direction:

- Promote the phase split into an authority design, not as a blanket filter.
  Persist active-vs-deferred dependency lanes for outPath JSON projections.
- Keep unknown, mixed, attr-name/keyset, formals, negative membership, and
  suggestion paths active. Only canonical by-name package `FileBytes` proven to
  be construction-only for a derivation-output projection can move to the
  deferred lane.
- Add a cert-bound activation record tying fragment path/name, JSON/context
  digest, output context roots, active dep digest, deferred dep digest, and
  authority mode. A load hit may ignore changed deferred deps only when all
  active deps verify and no unsupported deferred class is present.

10-commit benchmark under the refined unsafe ablation:

- Command shape:
  `nix run .#eval-trace-bench -- generate --num-commits 10 --runs cold,hot`
  with observed-key proof, construction-keyset suppression, relaxed by-name
  directory listings, output-boundary recovery, by-name package semantic
  diagnostics, outPath dep facets, and the unsafe deferred-construction
  by-name FileBytes ablation enabled. The run was wall-only: no debug and no
  `NIX_SHOW_STATS`.
- `cold/90`:
  - `7d7616894ec4`: `16.857848084008s`,
  - `3f7b5d89ca3e`: `0.374781738967s`,
  - `53443ee9cdff`: `0.364192045992s`,
  - `1575c9f64e00`: `0.484339907998s`,
  - `db18c3780dc2`: `0.413550726022s`,
  - `fd3fbe0cfd62`: `13.820006460010s`,
  - `fa143ab863d8`: `0.364901194989s`,
  - `1e7e40c82cf9`: `0.344030244974s`,
  - `7c395b2da6d6`: `0.353550478001s`,
  - `7fb36e13811a`: `14.013907015033s`,
  - mean: `4.739110789599s`.
- `hot/92` using `--hot-from cold/90`:
  - `7d7616894ec4`: `0.339273313002s`,
  - `3f7b5d89ca3e`: `0.396043777000s`,
  - `53443ee9cdff`: `0.384547739988s`,
  - `1575c9f64e00`: `0.518738668994s`,
  - `db18c3780dc2`: `0.420123620017s`,
  - `fd3fbe0cfd62`: `0.339109253022s`,
  - `fa143ab863d8`: `0.348429669975s`,
  - `1e7e40c82cf9`: `0.342409787991s`,
  - `7c395b2da6d6`: `0.357981148001s`,
  - `7fb36e13811a`: `0.352081640973s`,
  - mean: `0.379873861896s`.
- Each `hot/92` `eval.json` is byte-identical to the corresponding `cold/90`
  `eval.json`.
- Compared with the established pre-schema SQLite eval-cache baseline
  (`cold/938 ~6.06s`, `hot/938 ~0.88s`), the unsafe ablation beats both wall
  means on this 10-commit window. This does not make the ablation sound; it
  makes the deferred construction/activation lane the first prototype that has
  demonstrated enough headroom to justify hardening.

Harness observation:

- `hot/91 --hot-from cold/90` failed before evaluation because the hot manifest
  was built from the current `/home/connorbaker/nixpkgs` checkout after the cold
  run had left it at the final benchmark commit. `cold/90` correctly recorded
  `sourceRepoProvenance.head = 7d761689...`, but the failed `hot/91` manifest
  recorded `7fb36e138...`.
- The hot source state was valid; the rejection came from comparing the source
  manifest against a planned hot manifest whose source provenance described the
  incidental current checkout rather than the cold state being reused.
- I changed the harness so hot runs inherit `sourceRepoProvenance` from their
  `--hot-from` manifest before source compatibility checks and before writing
  the hot manifest. Focused tests pass:
  `uv run --with pytest pytest -q tests/test_semantics.py -k 'hot_manifest_inherits_source_provenance or hot_source_manifest'`.
- I also hardened `generate` to restore the nixpkgs checkout in a `finally`
  block for the `nixpkgs-release` suite, so failed hot/cold runs are less likely
  to leave `/home/connorbaker/nixpkgs` at the final benchmark commit.
- Package-level runner verification after the harness changes passes:
  `nix build -L --builders '' .#eval-trace-bench` (`pytest`, `ruff`, format
  check, and `pyright` all clean).

Adversarial review and refinement:

- Parallel review agreed the 10-commit timing evidence is real but warned not
  to overclaim it:
  - `cold/90`/`hot/92` are wall-only and prove headroom,
  - `cold-stats/16`/`17` are the causal counter evidence for the two-commit
    duplicate-output case,
  - neither is a soundness proof.
- A more serious issue was found: the unsafe deferral had been publishing the
  filtered dep set into the normal trace backend. Because the trace backend is
  not keyed by the unsafe proof-action authority mode, that could let a row
  produced under the unsafe ablation be served later by ordinary trace lookup.
- I changed the prototype so unsafe deferred-construction filtering still
  returns deps to the explicitly unsafe, authority-mode-keyed proof JSON action
  cache, but does not record the filtered aggregate JSON projection into the
  normal trace backend.
- I also started persisting lane evidence in the proof certificate under
  `unsafeDeferredDependencyLanes`. This is explicitly marked
  `authority = diagnostic-only`; it records per-fragment candidates/deferred/
  activated counts plus path and expected dep hash for each deferred or
  activated by-name package `FileBytes` fact. This does not make the lane rule
  sound, but it gives us the durable audit artifact the previous counter-only
  prototype lacked.

Follow-up stats run after the safety refinement:

- Build:
  `nix build -L --builders '' --out-link /tmp/nix-cli-defer-lanes .#nix-cli`
  passed. Result:
  `/nix/store/b1dnpp8ra5a2q4yk9yi47av5zpkiaam3-nix-2.35.0pre20260515_dirty`.
- `result` was repointed to that build for the benchmark harness.
- `cold-stats/17` on `v11-tail-1cfd-345.txt`:
  - `1cfd`: `21.44057066401001s`,
  - `345`: `0.4428031489951536s`,
  - outputs byte-identical (`sha256 a236395...`).
- First commit proof/store counters:
  - proof store `successes=1`, `deps=35740`,
  - guard exact paths `3835`,
  - by-name package exact guard paths `767`,
  - outPath construction by-name candidates `1822`,
  - deferred `1782`,
  - activated `40`,
  - `recordUs=0` for the unsafe filtered aggregate trace publication path.
- Certificate lane diagnostics:
  - `unsafeDeferredDependencyLanes.kind =
    outpath-construction-by-name-filebytes-v1`,
  - `authority = diagnostic-only`,
  - `aarch64-linux`: candidates `910`, deferred `890`, activated `20`,
  - `x86_64-linux`: candidates `912`, deferred `892`, activated `20`,
  - the changed `libvpx` and `newt` package files appear as deferred facts in
    both fragments, with expected digest values, and do not appear in the guard.
- Second commit load counters:
  - proof load `hits=1`, `misses=0`, `guardAccepted=1`,
  - `guardRejectExact=0`,
  - `guardDropByNamePackageFileBytes=0`,
  - `outputBoundaryRecoveryAttempts=0`,
  - `byNameSemanticRecoveryAttempts=0`,
  - total proof load time `103638us`.

Current safety status:

- The prototype remains intentionally unsafe for serving because the verifier
  still trusts the active lane by policy instead of proving the deferred lane is
  irrelevant.
- The accidental flagless trace-serving risk for newly produced rows is fixed
  by not recording unsafe-filtered aggregate rows into the normal trace backend.
- The proof-action cache remains mode-isolated by
  `proofJsonActionAuthorityMode()`, including the unsafe deferral flag.
- Next hardening step is to replace `diagnostic-only` lane metadata with an
  accepted verifier rule: active deps verify normally; deferred deps must be a
  supported class, be bound to fragment/output JSON/context, have their original
  expected facts persisted, and be ignored only under an explicit activation
  predicate that the verifier checks.

### 2026-05-25 run 19 after lane-shape and harness hardening

Adversarial review results:

- Harness review found two concrete benchmark risks:
  - `execute_run()` created the run directory before validation, while automatic
    run-number discovery counted every numeric directory. Empty failed runs could
    therefore advance the next run number and make `run 0` stop being the clean
    reference after result deletion.
  - The harness recorded provenance for `result/bin/nix` but executed the mutable
    symlink path. If `result` moved during a run, later commits could silently use
    a different binary from the manifest.
- Authority review agreed the normal trace-backend leak for newly produced rows
  is fixed by skipping aggregate trace publication under unsafe deferred deps,
  but emphasized that this is a control-flow invariant, not a persisted row
  invariant. Old unsafe rows or future alternate writers would still need cache
  isolation/cleanup.
- Benchmark-evidence review agreed the causal signal is strong for the
  `1cfd -> 345` duplicate case, but the cert is still an unsafe oracle because
  `unsafeDeferredDependencyLanes` was diagnostic-only and ignored by
  verification. The missing authoritative facts are current selected output
  identity and active derived-store/output facts, not merely old output store-path
  availability.

Harness changes retained:

- `_next_unused_run_number()` now ignores numeric run directories that have no
  manifest and no commit-output directories. This keeps an empty failed run from
  consuming the next run number.
- The benchmark runner resolves `result/bin/nix` once with `resolve(strict=True)`
  and uses that store path for both provenance and execution.
- Added focused tests for both cases. The package-level build of
  `.#eval-trace-bench` passed with `62` tests, ruff, format check, and pyright.

Lane/cert hardening retained:

- Added `relativePath` to each deferred/activated by-name package fact in
  `unsafeDeferredDependencyLanes`.
- Added a verifier-side fail-closed shape check for
  `unsafeDeferredDependencyLanes`: when the field exists it must be under the
  unsafe deferred-construction authority mode, have the expected kind/diagnostic
  authority, have internally consistent counts, decode every expected hash, and
  every fact must be a canonical repo-relative by-name `package.nix` path with no
  `.git` component.
- Bumped the unsafe authority string with
  `unsafe-outpath-defer-construction-lane-diagnostics=1`, so stale unsafe certs
  produced before the lane sidecar shape change do not share the proof/fast key.
- This is not a sound serving rule. It only makes the diagnostic cert shape
  explicit and fail-closed. The verifier still does not use the lane to prove
  that deferred deps are irrelevant.

Validation:

- `uv run --frozen --with pytest pytest -q -p no:cacheprovider tests/test_semantics.py -k 'next_unused_run_number or resolve_benchmark_nix_bin or hot_manifest_inherits_source_provenance or hot_source_manifest'`
  passed: `6 passed`.
- `uv run --frozen ruff format ... && uv run --frozen ruff check ...` passed.
- `nix build -L --builders '' --out-link /tmp/nix-cli-defer-lanes .#nix-cli`
  passed. Result was repointed to
  `/nix/store/khfxr50didxzcm1yqmfsawdzvxzv6dlm-nix-2.35.0pre20260515_dirty`.
- The following stats run also rebuilt `.#eval-trace-bench` and passed its full
  install checks: `62` tests, ruff, format check, and pyright.

`cold-stats/19` on `v11-tail-1cfd-345.txt` with the same unsafe deferred-lane
flags:

- `1cfd`: `21.160978137981147s`.
- `345`: `0.4396420630509965s`.
- Outputs remained byte-identical:
  `sha256 a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`.
- First commit outPath/proof counters:
  - deferred by-name construction candidates: `1822`,
  - deferred: `1782`,
  - activated: `40`,
  - proof deps: `35740`,
  - retained cert `.deps`: `4` env deps,
  - guard exact paths: `3835`,
  - guard by-name package exact paths: `767`,
  - `recordSkippedUnsafeDeferred=1`.
- Second commit load counters:
  - proof hit: `1`,
  - `guardAccepted=1`,
  - `guardRejectExact=0`,
  - `outputBoundaryRecoveryAttempts=0`,
  - `byNameSemanticRecoveryAttempts=0`,
  - `loadTrace.count=0` and verifier trace work stayed avoided,
  - proof load total `108403us`.
- Cert inspection:
  - proof authority includes
    `unsafe-outpath-defer-construction-lane-diagnostics=1`,
  - lane entries: `aarch64-linux` candidates/deferred/activated
    `910/890/20`, `x86_64-linux` `912/892/20`,
  - sample fact now includes both absolute `path` and repo `relativePath`,
  - `pkgs/by-name/li/libvpx/package.nix` and
    `pkgs/by-name/ne/newt/package.nix` appear as deferred facts twice each,
    and neither appears in the guard.

Current decision:

- The deferred-construction lane remains the best performance lead: it is the
  only path so far that beats the old pre-schema baseline wall times on the
  10-commit window and it has a clear two-commit causal counter story.
- The safe design is not "trust current persisted facts." Current persisted
  facts are mostly env deps plus a git guard; the lane sidecar is diagnostic. A
  sound version needs verifier-consumed active output identity facts. At minimum,
  the cert must persist current-checkable child fragment output identity and
  active derived-store/output facts, then allow by-name package FileBytes changes
  only when every rejected changed path is in the deferred lane, no changed path
  is in the activated lane, all non-deferred guard coverage still accepts, store
  deps are present, and active output identity recomputes to the cached
  JSON/context.
- Exact-head fast memo after accepted proof hits is tempting because the
  duplicate proof load still costs about `108ms`, but it cannot be treated as
  generally safe without a cert purity/volatility condition. It would otherwise
  bypass volatile deps after one verified hit. Keep it as a possible explicitly
  gated benchmark ablation, not as the next retained safe implementation.

### 2026-05-25 run 20 active DerivedStorePath diagnostic sidecar

Question:

- To make deferred construction lanes safe, do we need to persist and recompute a
  huge active output witness, or is the active `DerivedStorePath` set small enough
  to be plausible?

Implementation:

- Extended the existing non-authoritative by-name package semantics sidecar with
  `derivedStorePathDeps` for the aggregate and for each child fragment.
- This is deliberately outside the serving certificate. It is not read by the
  verifier and does not affect hot proof load. It is only design-space evidence
  for the next authority format.

Validation:

- Rebuilt `.#nix-cli` successfully and repointed `result` to
  `/nix/store/6zklc6arrkh8f0ryac1ijyq4w2ajax2p-nix-2.35.0pre20260515_dirty`.
- `cold-stats/20` on the same two-commit list:
  - `1cfd`: `21.0550855009933s`,
  - `345`: `0.43969363800715655s`,
  - proof hit behavior unchanged on the duplicate: `hits=1`,
    `guardAccepted=1`, `outputBoundaryRecoveryAttempts=0`,
    `byNameSemanticRecoveryAttempts=0`, proof load total `107204us`.
- Sidecar size was about `556KiB`; it is diagnostic-only.

Observed active witness size:

- Aggregate proof deps: `35740`.
- Aggregate active by-name package FileBytes: `20`.
- Aggregate active `DerivedStorePath` diagnostics: `846`.
- Per-fragment active `DerivedStorePath` diagnostics:
  - `aarch64-linux`: `845`,
  - `x86_64-linux`: `845`.
- `338` of the aggregate derived-store-path facts are under `pkgs/by-name/*`, but
  none are `package.nix`; the sample is source/support files such as
  `doc/anchor-use.js` and by-name patch/setup-hook files.

Causal read:

- This supports a narrower active-output-witness design. The verifier would not
  need to recompute tens of thousands of deps to make progress on the deferred
  lane; the active derived-store source-path witness is under one thousand facts
  for this projection.
- It is still not enough by itself. These facts describe source paths copied to
  the store, not the selected derivation output identity for the top-level JSON.
  The next authority prototype needs both:
  - active path/derived facts that stay under normal guard or explicit
    recomputation, and
  - a current-checkable output identity witness for each child fragment's cached
    `out` string/context.

### 2026-05-25 runs 21-22 lane hardening and producer-side identity diagnostic

Adversarial review results:

- Performance review reaffirmed that the deferred construction by-name
  `FileBytes` lane is the only lead so far with a plausible path to beating both
  old targets, but only as a lead. The current serving path is still an unsafe
  ablation, not a proof.
- Producer-side review rejected the consumer-phase activation argument as
  insufficient: a package file can determine the derivation output during
  `derivationStrict`, then not be observed again when the already-built
  `outPath` thunk is forced. The safe witness must be producer-side output
  identity, not merely "not re-read later."
- Proof review found additional unsafe/soundness hazards to track:
  - `sourceIdentityGuards` currently validate shape but do not recheck the
    expected `GitRevisionIdentity` hash. This may be acceptable only if every
    result-visible source path is independently covered by the path guard; it is
    not acceptable as standalone authority.
  - `unsafe-by-name semantic recovery` is a serving oracle when enabled despite
    being documented as diagnostic-only.
  - `DerivedStorePath` remains treated as git-guard-covered in JSON action
    proofs, whereas a sound proof should retain/recompute it or replace it with a
    purpose-built derivation-output identity dep.
  - Output-context store-path sidecars should be tied directly to the decoded
    output context roots before serving, not only by a mode label and sidecar
    checksum.

Implementation changes retained:

- Made `unsafeDeferredDependencyLanes` mandatory under the unsafe deferred
  authority mode and added explicit `depsDropped`.
- Made lane construction fail closed if any deferred/activated fact cannot be
  encoded as canonical repo-relative `pkgs/by-name/*/*/package.nix`.
- Tightened lane shape checks for duplicate facts, fragment-name uniqueness,
  deferred/activated overlap within a fragment, count consistency, and
  `depsDropped` consistency.
- Replaced the broad producer drop predicate with a canonical by-name
  `package.nix` layout check.
- Added a diagnostic-only producer-side by-name construction fact sidecar on
  `ByNameDerivationOutputFacet`. It records the by-name package `FileBytes`
  facts observed during `derivationStrict` for that derivation facet. This does
  not authorize serving.

Validation:

- `nix build -L --builders '' --out-link /tmp/nix-cli-defer-lanes .#nix-cli`
  passed and was used for `cold-stats/21`.
- `cold-stats/21` on `v11-tail-1cfd-345.txt`:
  - `1cfd`: `21.307810445025098s`,
  - `345`: `0.4476642420049757s`,
  - duplicate proof hit: `hits=1`, `guardAccepted=1`,
    `outputBoundaryRecoveryAttempts=0`,
    `byNameSemanticRecoveryAttempts=0`, proof load total `112618us`.
- `nix build -L --builders '' --out-link /tmp/nix-cli-identity-diag .#nix-cli`
  passed and was used for `cold-stats/22`.
- `cold-stats/22` on the same two commits:
  - `1cfd`: `21.24198673898354s`,
  - `345`: `0.4324899190105498s`.
- `git diff --check` passed after the producer-side diagnostic patch.

Observed in `cold-stats/22`:

- Lane shape stayed stable:
  - `aarch64-linux`: candidates/deferred/activated `910/890/20`,
  - `x86_64-linux`: candidates/deferred/activated `912/892/20`.
- The diagnostic sidecar had `30` by-name derivation output facets and `24`
  producer-side construction file facts.
- Producer facts were narrow:
  - `11` unique producer by-name package files,
  - every producer fact appeared in the same-fragment activated lane,
  - no producer fact appeared in the same-fragment deferred lane.
- The changed package files in the `1cfd -> 345` tail case were still deferred:
  - `pkgs/by-name/li/libvpx/package.nix`: deferred yes, activated no,
    producer-side construction fact no,
  - `pkgs/by-name/ne/newt/package.nix`: deferred yes, activated no,
    producer-side construction fact no.

Causal read:

- This is stronger evidence than the prior consumer-phase lane alone. It says
  the packages that were actually observed while producing by-name derivation
  output facets are exactly the activated class for the same system fragments,
  while the changed `libvpx`/`newt` files are outside that producer-side witness.
- It still does not prove safe serving. The current diagnostic records by-name
  package file facts next to derivation output facets; it does not add a
  canonical dependency kind for `(drvPath, outputName) -> outputPath`, and the
  verifier does not recompute that identity.
- Next safe-direction implementation should add a real
  `DerivationOutputIdentity` dependency/witness rather than overloading
  `DerivedStorePath`. The verifier should retain/recompute that witness and
  only then allow changed by-name package files to be ignored when they are
  deferred, unactivated, and not producer-output-identity inputs.

### 2026-05-25 runs 23-25 conservative lane checks and proof-gap hardening

Implementation changes retained:

- Stopped treating `DerivedStorePath` as git-guard-covered in JSON action
  proofs. It is now retained in the certificate and recomputed during
  verification. This closes the immediate hole but is intentionally
  conservative and too broad for the final performance shape.
- Renamed the unsafe deferred lane authority from a diagnostic label to
  `unsafe-serving-ablation-v1`, and made the unsafe by-name semantic recovery
  oracle diagnostic-only. It no longer directly serves cached output.
- Added a lane self-consistency check for the unsafe deferred construction
  lane. If deps were dropped, verification now requires an accepted git guard,
  rejects any changed path that appears in an activated lane, rejects non-`M`
  changes to deferred paths, and requires modified deferred paths to remain
  regular files at both the cert base and current head.
- Tightened lane facts to reject symlinked by-name package paths, mirroring the
  normal git guard path checks.
- Added minimal fail-closed hardening for two proof gaps found by adversarial
  review:
  - `sourceIdentityGuards` are now bound to the command root file through the
    accepted git guard. The root file is added as an exact guard path when the
    source identity guard is stored, and verification requires exact,
    structural-exact, or recursive git-guard coverage for that file.
  - `output-context-v1` store proofs now carry
    `outputContextStorePathRoots`. Verification decodes the cached output,
    recomputes the roots from its string context, requires exact set equality
    with the manifest, and requires every root to be present in the checked
    valid store-path deps. The proof namespace was bumped so old certs fail
    closed.

Validation:

- `nix build -L --builders '' --out-link /tmp/nix-cli-derived-recheck .#nix-cli`
  passed after the `DerivedStorePath` recheck change.
- `cold-stats/23` on `v11-tail-1cfd-345.txt`:
  - `1cfd`: `20.646853852958884s`,
  - `345`: `0.6722252320032567s`,
  - duplicate proof hit: `hits=1`, `guardAccepted=1`,
    `parseUs=123121`, `guardUs=94002`, `verifyUs=101438`,
    proof load total `320288us`.
- `nix build -L --builders '' --out-link /tmp/nix-cli-lane-diff-check .#nix-cli`
  passed after the first lane self-consistency check.
- `cold-stats/24` on the same two commits:
  - `1cfd`: `20.358744413999375s`,
  - `345`: `0.5268808379769325s`,
  - duplicate proof hit: `hits=1`, `guardAccepted=1`,
    `parseUs=5365`, `guardUs=71093`, `verifyUs=114865`,
    proof load total `192955us`.
- `nix build -L --builders '' --out-link /tmp/nix-cli-context-root-guard .#nix-cli`
  passed after the source-root and output-context manifest hardening.
- `cold-stats/25` on the same two commits:
  - `1cfd`: `20.09565511898836s`,
  - `345`: `0.5942918549990281s`,
  - duplicate proof hit: `hits=1`, `guardAccepted=1`,
    `parseUs=5266`, `guardUs=70717`, `verifyUs=184310`,
    proof load total `261991us`.
  - Outputs matched byte-for-byte:
    `sha256 a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`.

Observed cert shape after run `25`:

- Proof cache namespace is `eval-trace-json-action-proof-v12`.
- Main cert size: `884376` bytes; guard sidecar: `401352` bytes; store-path
  sidecar: `9301` bytes; diagnostic by-name sidecar: `561391` bytes.
- Retained deps: `850`, of which `846` are `derivedStorePath`.
- Store proof scope: `output-context-v1`.
- `outputContextStorePathRoots` contains exactly the two returned context
  `.drv` roots.
- Unsafe deferred lane shape stayed stable:
  - `aarch64-linux`: candidates/deferred/activated `910/890/20`,
  - `x86_64-linux`: candidates/deferred/activated `912/892/20`.
- Diagnostic sidecar for the aggregate still reports `846`
  `DerivedStorePath` facts and `20` construction by-name package file facts.

Causal read:

- The conservative `DerivedStorePath` recheck improves soundness but is not the
  winning implementation. It raises duplicate proof load from the run `21-22`
  band (`~107-113ms`) into the run `23-25` band (`~193-320ms`) and keeps `846`
  broad path-derived facts in the hot cert.
- The lane self-consistency and output-context/root hardening are the right
  fail-closed direction, but they do not solve the central semantic hole. A
  deferred by-name `package.nix` can still affect derivation construction and
  not be re-observed during `outPath` forcing. The current lane remains an
  unsafe serving ablation.
- Parallel adversarial review converged on the same next lead: replace the
  broad retained `DerivedStorePath` set with a compact producer-side
  derivation-output identity witness, ideally a canonical dependency for
  `(drvPath, outputName) -> outputPath/context`. The expected evidence is that
  retained derived-store deps fall from `846` to a small output-identity set,
  cert size drops materially, duplicate proof load returns toward `<150ms`, and
  activated producer-package changes still reject.

Follow-up lower-bound ablation:

- Added
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DERIVATION_OUTPUT_FACET_PROOF=1`, gated
  behind `NIX_EVAL_TRACE_JSON_ACTION_ALLOW_UNSAFE_ABLATIONS=1` and the unsafe
  deferred-construction lane. This is not a proof-hardening change. It uses the
  existing producer-side `byNameDerivationOutputFacets` as an output-boundary
  witness and omits retained `DerivedStorePath` deps to measure the ceiling for
  a compact canonical output-identity proof.
- The ablation verifies that each facet's `.drv` is currently valid, reads the
  derivation, and checks each recorded output name still maps to the recorded
  static output path. It still does not prove that the current source expression
  would evaluate to the same `.drv`, so the lane remains unsafe for serving
  claims.
- `nix build -L --builders '' --out-link /tmp/nix-cli-output-facet-ablation .#nix-cli`
  passed.
- `cold-stats/26` on `v11-tail-1cfd-345.txt`:
  - `1cfd`: `21.005026080994867s`,
  - `345`: `0.4716721829609014s`,
  - duplicate proof hit: `hits=1`, `guardAccepted=1`,
    `parseUs=10136`, `guardUs=76391`, `verifyUs=41453`,
    proof load total `138061us`.
  - Outputs matched byte-for-byte:
    `sha256 a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`.
- Cert shape changed as intended:
  - retained deps: `4`,
  - retained `derivedStorePath` deps: `0`,
  - main cert `byNameDerivationOutputFacets`: `30`,
  - main cert size: `662239` bytes,
  - same guard and store-path sidecar sizes as run `25`.

Causal read:

- This confirms the performance hypothesis. Dropping the `846`
  `DerivedStorePath` recomputations while keeping the other v12 hardening moved
  duplicate proof load from run `25`'s `261991us` to `138061us`, and wall time
  from `0.594s` to `0.472s`.
- The lower-bound result is still slower than run `22` because the current
  cert still carries the large diagnostic/sideband material and the git guard
  remains around `70-76ms`. But it recovers most of the regression introduced by
  retaining broad `DerivedStorePath`.
- Adversarial review's semantic conclusion is now recorded as a constraint:
  a derivation-output identity witness proves only "given this authorized
  `.drv`, this output still maps to this output path/context." It does not prove
  "this changed package file is irrelevant" or "current eval still constructs
  this `.drv`." A production design needs a source-to-drv binding proof in
  addition to the compact output-boundary witness.

### 2026-05-25 run 93 10-commit output-facet lower bound

Ran the unsafe derivation-output facet lower-bound ablation on the first 10
benchmark commits with no debug and no `NIX_SHOW_STATS`:

- Env included the v12 proof flags plus:
  - `NIX_EVAL_TRACE_JSON_ACTION_ALLOW_UNSAFE_ABLATIONS=1`,
  - `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_OUTPATH_DEFER_CONSTRUCTION_BY_NAME_FILEBYTES=1`,
  - `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DERIVATION_OUTPUT_FACET_PROOF=1`.
- Command used `--runs cold,hot --run-number 93` because the harness refused to
  reuse the existing global `reference` manifest. That reference was produced by
  an incompatible baseline binary and environment, so it is not a valid
  correctness comparator for this run.

Observed wall times:

- `cold/93`: mean `3.4289671804988755s`, median `0.544849599013105s`, min
  `0.4389544560108334s`, max `16.765952894988004s`.
- `hot/93`: mean `0.5201892986078747s`, median `0.5513081320095807s`, min
  `0.4192673960351385s`, max `0.6157671540277079s`.

Per-commit cold/hot pairs:

- `1575c9f64e00`: cold `0.5151128739817068s`, hot `0.5861850929795764s`.
- `1e7e40c82cf9`: cold `0.5472614929894917s`, hot `0.5742035009898245s`.
- `3f7b5d89ca3e`: cold `0.4389544560108334s`, hot `0.5513081320095807s`.
- `53443ee9cdff`: cold `0.5365398210124113s`, hot `0.4248049460002221s`.
- `7c395b2da6d6`: cold `0.544849599013105s`, hot `0.5492429180303589s`.
- `7d7616894ec4`: cold `16.765952894988004s`, hot `0.42244933400070295s`.
- `7fb36e13811a`: cold `13.385944661975373s`, hot `0.4192673960351385s`.
- `db18c3780dc2`: cold `0.5289542880491354s`, hot `0.6157671540277079s`.
- `fa143ab863d8`: cold `0.5480152519885451s`, hot `0.5687062570359558s`.
- `fd3fbe0cfd62`: cold `0.4780864649801515s`, hot `0.4899582549696788s`.

Correctness/readout notes:

- `cold/93` and `hot/93` outputs matched each other for all 10 commits.
- Comparing to the stale global `reference` reported four mismatches, but that
  reference points at a different baseline binary and no matching measurement
  environment. The harness correctly rejected overwriting it. Do not count those
  stale-reference mismatches as a valid correctness failure or success.
- The hot mean `0.520s` is materially below the old run-938 hot target
  (`0.880s`) and supports the performance hypothesis that replacing broad
  retained `DerivedStorePath` rechecks can beat the baseline.
- The cold mean is dominated by two expensive first-time commits; without a
  regenerated compatible reference/cold/hot set, it should not be overclaimed
  against either the original run-938 first-10 baseline or the later regenerated
  run-0 baseline.

Next production-oriented lead:

- Keep the lower-bound ablation as evidence only. Its current authority is still
  unsafe: validating an old `.drv` output identity does not prove current eval
  still constructs that `.drv`.
- Implement a real `DerivationOutputIdentity` dependency kind so producer-side
  derivation outputs are first-class proof facts rather than diagnostic JSON.
  The witness should prove only `(drvPath, outputName) -> outputPath` for static
  outputs, and must be combined with source/lane proof that the current path can
  still denote that `drvPath`.
- Benchmarkable expectation: retained `derivedStorePath` deps should stay near
  zero, DOI witness count should be closer to tens than hundreds, `verifyUs`
  should stay near the run-26 band, and the 10-commit hot mean should remain
  below baseline after replacing diagnostic authority with real dep plumbing.

Harness note from parallel review:

- `eval-trace-bench generate` has no result-root/output-dir option. It writes
  to `<--nix>/eval-trace-bench-results`.
- `reference` is a flat unnumbered directory. It must be moved or deleted before
  regenerating a compatible reference; `--run-number` only applies to stateful
  runs like `cold/<n>` and `hot/<n>`.
- For the current partially repopulated results tree, run `94` was reported as
  unused after run `93`. A clean compatible two-commit reference/cold/hot set
  would therefore be:
  - move `eval-trace-bench-results/reference` aside if it exists,
  - run `nix run .#eval-trace-bench -- generate --nix /home/connorbaker/nix --nixpkgs /home/connorbaker/nixpkgs --commit-list /home/connorbaker/nix/benchmarks/eval-trace-bench/commit-lists/v11-tail-1cfd-345.txt --run-number 94 --runs reference,cold,hot`,
  - analyze with `nix run .#eval-trace-bench -- runs --nix /home/connorbaker/nix --nixpkgs /home/connorbaker/nixpkgs --runs reference,cold/94,hot/94 --reference reference`.
- `--allow-intersection` does not solve manifest shape mismatches. It is not a
  substitute for a compatible regenerated reference.

### 2026-05-25 DOI dep plumbing and retention ablations

Implemented a first-class `DerivationOutputIdentity` dependency:

- Key: static `.drv` store path plus output name.
- Observation: printed static output path.
- Resolver: require the `.drv` is still valid, `readDerivation`, recompute the
  derivation store path from the loaded derivation, find the output name, reject
  dynamic outputs, and compare the computed output path.
- Proof JSON: encode as `kind=derivationOutputIdentity` with `drvPath`,
  `outputName`, and `expected`.
- Schema/proof namespace bumped to `kSchemaEpoch=53` and observed-key proof
  namespace `v13` for the relaxed-by-name-directory-listing mode.

Adversarial review constraints:

- DOI proves only `given this .drv, outputName maps to outputPath`. It still
  does not prove that the current source expression constructs that `.drv`.
- The unsafe facet ablation remains a lower-bound experiment until there is a
  source-to-drv binding proof.
- A stale process-global facet bug was found: facets were only cleared when the
  by-name package semantics diagnostic flag was enabled. The clear now also
  runs when the unsafe derivation-output facet proof ablation is enabled.
- Benchmark reports intentionally ignore some subject/environment fields, so
  comparisons must manually inspect manifests and use a regenerated compatible
  reference. Do not treat the stale global `reference` as a valid comparator.

Broad DOI recording lower bound:

- First producer-side implementation recorded DOI facts for every static
  by-name derivation instantiated during evaluation.
- Build passed:
  `nix build -L --builders '' --out-link /tmp/nix-cli-doi-prototype .#nix-cli`.
- `cold-stats/27` on `v11-tail-1cfd-345.txt`:
  - `1cfd`: `25.2s`,
  - `345`: `1.4587653820053674s`,
  - proof hit: `hits=1`, `guardAccepted=1`.
- Cert shape:
  - retained deps: `15942`,
  - `derivationOutputIdentity`: `15938`,
  - `environmentLookup`: `4`,
  - `byNameDerivationOutputFacets`: `30`.
- Load counters on duplicate:
  - `parseUs=180820`,
  - `guardUs=69871`,
  - `verifyUs=819202`,
  - proof load total `1082741us`.

Causal read:

- Broad DOI is worse than retained `DerivedStorePath`. It creates a compact
  semantic kind but retains far too many producer facts. Parse and verifier
  cost dominate the duplicate path.
- This falsified "record all producer DOI facts" as a winning implementation.
  The relevant proof set must be the output-boundary facts actually used by the
  returned JSON, not every derivation touched during evaluation.

Producer-side by-name narrowing:

- Adjusted producer recording so DOI facts are only recorded when a by-name
  `package.nix` witness is found for the derivation. This reduced noise but
  still retained all DOI facts that survived into the aggregate proof deps.
- Build passed:
  `nix build -L --builders '' --out-link /tmp/nix-cli-doi-byname .#nix-cli`.
- `cold-stats/28` on the same two commits:
  - `1cfd`: `23.2s`,
  - `345`: `0.7483618819969706s`,
  - proof hit: `hits=1`, `guardAccepted=1`.
- Cert shape:
  - retained deps: `3017`,
  - `derivationOutputIdentity`: `3013`,
  - `environmentLookup`: `4`,
  - `byNameDerivationOutputFacets`: `30`.
- Load counters on duplicate:
  - `parseUs=20460`,
  - `guardUs=72333`,
  - `verifyUs=276912`,
  - proof load total `378114us`.

Causal read:

- Producer-side narrowing alone is still too broad. It improves broad DOI by
  removing about 13k retained deps, but the verifier is still spending hundreds
  of milliseconds recomputing unused output identities.
- The remaining gap is retention-side selection, not DOI resolver mechanics.

Facet-keyed DOI retention filter:

- Added a retention filter under the unsafe derivation-output facet proof
  ablation: retain only DOI deps whose `(drvPath, outputName)` appears in the
  selected `byNameDerivationOutputFacets` for the cached JSON result.
- This keeps producer DOI recording available for diagnostics/future proof work
  but makes cert retention scale with the actually used output-boundary facets.
- Build passed:
  `nix build -L --builders '' --out-link /tmp/nix-cli-doi-filtered .#nix-cli`.
- `cold-stats/29` on the same two commits:
  - `1cfd`: `22.1s`,
  - `345`: `0.4695788819808513s`,
  - proof hit: `hits=1`, `guardAccepted=1`.
- Cert shape:
  - retained deps: `36`,
  - `derivationOutputIdentity`: `32`,
  - `environmentLookup`: `4`,
  - `byNameDerivationOutputFacets`: `30`,
  - facet output count: `32`,
  - output context roots: `2`.
- Load counters on duplicate:
  - `parseUs=3973`,
  - `guardUs=69090`,
  - `verifyUs=39124`,
  - proof load total `114640us`.

Causal read:

- This is the result the DOI prototype was supposed to test. Matching retained
  DOI deps to selected facets collapsed DOI retention from `3013` to `32` and
  restored duplicate timing to the unsafe run-26 band.
- The timing improvement is not just noise: duplicate wall moved from run 28
  `0.748s` to run 29 `0.470s`, while verifier time moved from `277ms` to
  `39ms` and parse time from `20ms` to `4ms`.
- This supports the design direction: compact output-boundary facts can beat
  broad `DerivedStorePath` rechecks, but the production proof still needs a
  source-to-drv binding before the unsafe lane can be treated as sound.

Next checks:

- Run the 10-commit no-stats cold/hot benchmark with the filtered DOI build and
  compare to run `93` and the old run-938 target.
- Regenerate a compatible reference before making any correctness claims
  against reference output. Manual manifest/env inspection is required.
- Continue adversarial testing with changed by-name `package.nix` cases:
  changed source must reject unless the source-to-drv binding proves the same
  `.drv` boundary.

### 2026-05-25 run 94 10-commit filtered DOI

Ran the filtered DOI/facet-retention build on the same first 10 benchmark
commits, no debug and no `NIX_SHOW_STATS`:

- Build: `/tmp/nix-cli-doi-filtered`.
- Command: `generate --runs cold,hot --run-number 94 --num-commits 10`.
- Cache subject changed from run `93` only by the current dirty patch hash and
  resulting Nix binary; env flags and commit order matched run `93`.
- `cold/94`: mean `3.242653153609717s`, median `0.4921509455307387s`, min
  `0.3805366840097122s`, max `15.953462111006957s`.
- `hot/94`: mean `0.45490929399384183s`, median `0.4580081549938768s`, min
  `0.37428116996306926s`, max `0.5301306589972228s`.
- `cold/94` and `hot/94` outputs matched byte-for-byte for all 10 commits.

Per-commit hot times:

- `1575c9f64e00`: `0.5092235910124145s`.
- `1e7e40c82cf9`: `0.4087806119932793s`.
- `3f7b5d89ca3e`: `0.37428116996306926s`.
- `53443ee9cdff`: `0.4711304309894331s`.
- `7c395b2da6d6`: `0.48326056299265474s`.
- `7d7616894ec4`: `0.39143844100181013s`.
- `7fb36e13811a`: `0.44488587899832055s`.
- `db18c3780dc2`: `0.41884970100363716s`.
- `fa143ab863d8`: `0.5301306589972228s`.
- `fd3fbe0cfd62`: `0.517111892986577s`.

Causal read:

- Filtering retained DOI deps to the selected output facets improved the
  10-commit hot mean from run `93` `0.520s` to run `94` `0.455s`.
- This is materially below the original run-938 hot target `~0.88s`, but it is
  still a lower-bound unsafe-lane result until source-to-drv authority is
  complete and a compatible reference is regenerated.
- Cold still has the same two first-touch spikes as run `93`; the filtered DOI
  change did not introduce a cold regression in the observed 10-commit window.

Adversarial review follow-up:

- A reviewer noted that DOI-bearing persistent exact descriptors are still
  rejected by `proofCoveredByExactIdentity`, while `exactSessionProofCovers`
  has DOI current-value revalidation. This is intentional for now: descriptor
  serving currently validates descriptor identity but does not call
  `exactSessionProofCovers`, so allowing DOI there without a current-value
  recheck would be unsound. Future direction is either keep DOI out of durable
  exact replay, or add a descriptor-serving path that explicitly revalidates
  DOI/current-world facts before accepting.
- Another reviewer flagged exact-head fast memo as potentially stale on dirty
  worktrees. Local code read: `buildGitJsonActionCacheKeys` calls
  `cleanGitHeadRevision`, which runs `git status --porcelain=v1
  --untracked-files=all --ignored=matching` and refuses to build keys on any
  dirty output. That particular dirty-worktree stale-serving concern appears
  falsified by the current code. It remains worth testing explicitly, but it is
  not the next blocker.
- The unsafe deferred-lane objection is valid: modified deferred by-name package
  files are accepted as regular `M` changes rather than revalidated. The current
  source-to-drv hardening therefore must focus on selected producer packages,
  and the lane must remain labelled unsafe until modified deferred facts are
  either rejected or proven irrelevant by a stronger output-boundary rule.

### 2026-05-25 selected package-file source witnesses

Added a source-to-drv hardening step for the DOI/facet prototype:

- Main cert facets now include a direct `packageFileBytes` hash for every
  selected `byNameDerivationOutputFacet`.
- Main cert facets also include the already-collected
  `constructionByNamePackageFileBytes` facts when available.
- `proofJsonActionDerivationOutputFacetsAccept` now rechecks the selected
  `package.nix` file bytes, validates by-name path shape and symlink safety,
  rechecks any construction by-name file-byte facts present, and only then
  verifies `.drv -> outputName -> outputPath`.
- Proof cache namespace moved from `v13` to `v15` for the current
  relaxed-by-name-directory-listing mode so older certs without package-file
  witnesses are not accepted.

Two-commit measurement:

- Build: `/tmp/nix-cli-doi-source-facet`.
- `cold-stats/30` on `v11-tail-1cfd-345.txt`:
  - `1cfd`: `22.1s`,
  - `345`: `0.5239958220045082s`,
  - proof hit: `hits=1`, `guardAccepted=1`.
- Cert shape:
  - proof namespace `eval-trace-json-action-proof-v15`,
  - retained deps: `36`,
  - `derivationOutputIdentity`: `32`,
  - `environmentLookup`: `4`,
  - `byNameDerivationOutputFacets`: `30`,
  - all `30` facets have `packageFileBytes`,
  - construction file-byte fact counts: min `0`, max `2`, total `24`,
  - facet output count: `32`.
- Load counters on duplicate:
  - `parseUs=4034`,
  - `guardUs=67224`,
  - `verifyUs=42258`,
  - proof load total `116150us`.

Causal read:

- Adding selected `package.nix` byte witnesses barely changes proof-load cost
  relative to run `29` (`114640us` -> `116150us`) and keeps retained DOI bounded
  at `32`.
- Duplicate wall time moved from run `29` `0.470s` to run `30` `0.524s`, which
  is still in the same practical band and below the original hot target. The
  verifier delta itself was only about `3ms`, so wall variation is likely not
  dominated by the added source-byte checks.
- This is a useful step but not a complete proof: direct selected
  `package.nix` bytes plus present construction by-name facts do not cover every
  possible helper file imported by a package. The claim is narrower:
  selected-package source edits now reject instead of relying only on stale
  `.drv` identity; full source-to-drv authority still needs broader dependency
  coverage or an explicit current package-to-drv recomputation.

### 2026-05-25 v15 follow-up, deferred lane, and facet counters

No-stats 10-commit measurement for the selected-package source witness build:

- Build: `/tmp/nix-cli-doi-source-facet`.
- Command: `generate --runs cold,hot --run-number 95 --num-commits 10`.
- `cold/95`: mean `3.2120253661996685s`, median `0.4942138934857212s`, min
  `0.37269709300016984s`, max `15.92506316001527s`.
- `hot/95`: mean `0.460761976102367s`, median `0.47585133102256805s`, min
  `0.3586816469905898s`, max `0.5219183909939602s`.
- `cold/95` and `hot/95` outputs matched byte-for-byte for all 10 commits.
- Causal read: selected `package.nix` source witnesses did not materially move
  the first-10 hot mean from filtered DOI run `94` (`0.455s` -> `0.461s`).

Deferred-lane probe:

- `cold-stats/31` on `7d7616 -> 345133` missed, but the miss was before the
  deferred-lane rule mattered: `guardAccepted=0`, `guardRejectExact=1`.
  The changed deferred by-name files included `libvpx` and `newt`.
- A stricter rule that rejected every modified deferred by-name package file
  was too conservative. `cold-stats/32` on `1cfd -> 345` missed with
  `guardAccepted=1`, `hits=0`, and the second commit took `18.7s`.
- I replaced that with a protected-facet rule: modified deferred package files
  are allowed only when they are regular-file `M` changes and are not among the
  selected/protected facet package witnesses.

Countered diagnosis:

- `cold-stats/33` still missed after the protected-facet rule:
  `guardAccepted=1`, `hits=0`, second commit `18.6s`.
- I added facet-proof rejection counters and required every accepted facet
  output to have the matching retained `derivationOutputIdentity` dep.
- `cold-stats/34` showed the immediate miss was my new checker, not the lane:
  `facetProofRejectMissingDoiDep=1` even though the cert had `32` DOI deps for
  `32` facet outputs. The cause was text-shape mismatch: cert DOI deps store
  `drvPath` as the raw store basename, while facets carry `/nix/store/...`.
- After comparing against `StorePath::to_string()`, `cold-stats/35` recovered:
  - `1cfd`: `22.7s`,
  - `345`: `0.7s`,
  - `hits=1`, `guardAccepted=1`, `facetProofAccepted=1`,
  - retained deps: `36`, DOI deps: `32`, facets: `30`, facet outputs: `32`,
  - deferred facts: `1782`, activated facts: `40`.
- `cold-stats/35` duplicate counters:
  - `parseUs=8625`,
  - `guardUs=72171`,
  - `verifyUs=189271`,
  - `nixObservedKeyVerifyUs=154680`,
  - proof load total `278002us`,
  - `storePathChecks=157`, `storePathQueryUs=3719`.

Causal read:

- The protected-facet deferred lane can recover the `1cfd -> 345` hit without
  accepting selected-package source drift. The remaining cost in run `35` is
  dominated by Nix observed-key verification rather than DOI or store-path
  checks.
- The DOI/facet path is still the strongest measured performance lead, but it
  is not a complete soundness story. It proves selected package file bytes and
  selected `.drv/output` identity independently; it still does not prove the
  current package source evaluates to that `.drv`.
- The new DOI-dep integrity check closes an easy malformed-cert hole where a
  facet could be present without a retained DOI dep. It does not by itself bind
  source to `.drv`.

Adversarial review notes to carry forward:

- Splicing risk remains: a cert can try to combine current package bytes with a
  different still-valid `.drv` unless another guard blocks it. Need a targeted
  falsification test.
- Helper/imported source risk remains: selected `package.nix` witnesses do not
  cover arbitrary helper files imported by that package. Normal git-guard deps
  may cover them today, but the facet proof itself does not.
- Modified deferred by-name packages are still unsafe if they can feed a
  selected output facet through an input derivation. The current lane rule is a
  benchmark hypothesis, not production authority.
- Process-global by-name facet capture is cleared at JSON serve start, but
  concurrent or multi-installable contamination deserves a falsification test.

### 2026-05-25 v17 selected-input-closure deferred protection

Implemented a narrower replacement for "reject all modified deferred by-name
package files":

- When writing a DOI/facet proof cert, compute the transitive input-derivation
  closure of the selected output facet `.drv` paths.
- From the process-global by-name facet snapshot, map any deferred by-name
  package files whose package `.drv` is in that selected input closure into a
  new cert field: `inputClosureByNamePackageFiles`.
- On proof load, treat those files as protected in
  `proofJsonActionDeferredDependencyLaneAcceptsCurrentDiff`; if a protected
  deferred package file changes, reject and fall back.
- Bumped the proof namespace for facet-proof mode to `v17` so old v15 certs
  without selected-input-closure protection are not reused.

Two-commit stats:

- Build: `/tmp/nix-cli-doi-input-closure`.
- `cold-stats/36` on `v11-tail-1cfd-345.txt`:
  - `1cfd`: `22.6s`,
  - `345`: `0.5s`,
  - `hits=1`, `guardAccepted=1`, `facetProofAccepted=1`.
- Cert shape:
  - proof namespace `eval-trace-json-action-proof-v17`,
  - retained deps: `36`,
  - DOI deps: `32`,
  - selected facets: `30`,
  - facet outputs: `32`,
  - selected-input-closure protected deferred package files: `232`.
- The changed deferred files in the v11 tail, `libvpx` and `newt`, were not in
  `inputClosureByNamePackageFiles`. This supports the causal read that the
  recovered hit is not accepting changes to a package known to feed the selected
  output facets.
- Duplicate load counters:
  - `parseUs=4443`,
  - `guardUs=69072`,
  - `verifyUs=42786`,
  - proof load total `119971us`,
  - `storePathChecks=157`, `storePathQueryUs=679`.

No-stats 10-commit gate:

- `cold/98`: mean `3.143600123404758s`, median `0.43037205046857707s`, min
  `0.3915824429714121s`, max `15.524222672043834s`.
- `hot/98`: mean `0.4249130867945496s`, median `0.4291714369901456s`, min
  `0.3551417489652522s`, max `0.47367272700648755s`.
- `cold/98` and `hot/98` outputs matched byte-for-byte for all 10 commits.
- Relative to v15 run `96`, v17 did not regress the first-10 hot path
  (`0.4635s` -> `0.4249s` in this run).

Later by-name regression slice:

- Slice commits:
  `0e04b37b`, `89bad58d`, `d3ce2e9a`, `abed8724`, `752d1aaa`, `fb26f56a`.
- `cold/99`: mean `2.937294009840116s`, median `0.39145840000128374s`, min
  `0.36169168597552925s`, max `15.664840912038926s`.
- `hot/99`: mean `0.3825909251754638s`, median `0.3812908789841458s`, min
  `0.3663312760181725s`, max `0.40476802201010287s`.
- `cold/99` and `hot/99` outputs matched byte-for-byte for all 6 commits.

Causal read:

- The v17 protection makes the deferred lane less obviously unsafe: a modified
  deferred by-name package is rejected if its recorded package derivation lies
  in the selected output `.drv` input closure.
- This does not fully bind source to `.drv`. It narrows one concrete
  adversarial case: changed deferred dependency package feeding a selected
  output derivation.
- Remaining gaps:
  - helper files imported by selected packages are not facet witnesses unless
    they also appear in ordinary retained/git-guarded deps,
  - source-byte witnesses and `.drv/output` witnesses are still independently
    checked and need a splicing falsification test,
  - input-closure protection depends on the by-name facet snapshot being
    complete for evaluated dependency packages.

### 2026-05-25 v17 adversarial falsification follow-up

I isolated the active proof-key namespace before mutating certs. An earlier
manual harness accidentally compared against the wrong proof key because the
copied `cold-stats/36` state contained both:

- `24/247c.../1cfd...json` plus a `345...alias` from the benchmark hit, and
- `0b/0b9e.../345...json` from a later manual fallback run.

For the current rebuilt binary, the active manual proof key was `0b...`; for
the original benchmark harness it was `24...`. The malformed-cert tests must
delete unrelated proof-key directories or run through the benchmark harness
itself, otherwise `candidateChecks=0` or an unrelated exact cert can hide the
behavior under test.

Active-cert malformed proof checks:

- Removing every `derivationOutputIdentity` dep from the active `0b` cert
  rejected as intended:
  - `candidateChecks=1`,
  - `facetProofAttempts=1`,
  - `facetProofRejectMissingDoiDep=1`,
  - `hits=0`.
- Replacing the first facet output path with
  `/nix/store/00000000000000000000000000000000-bad` rejected as intended:
  - `candidateChecks=1`,
  - `facetProofRejectOutputMismatch=1`,
  - `hits=0`.

Causal read:

- The DOI-retention integrity check is functioning on the current source. The
  earlier apparent acceptance was a harness artifact: the load was hitting an
  untouched exact `345` cert in a different proof-key directory.
- The verifier now really enforces “every selected facet output must have a
  retained DOI dep” and “the retained `.drv` must still compute the expected
  output path.”

Source-to-DRV splicing falsifier:

- Mutating the active cert so facet `0` used facet `1`'s `packageFile`,
  `packageName`, and `packageFileBytes`, while keeping facet `0`'s `drvPath`
  and `outputPaths`, still hit:
  - `candidateChecks=1`,
  - `guardAccepted=1`,
  - `facetProofAccepted=1`,
  - `hits=1`.
- This confirms the adversarial review: the facet proof checks source bytes and
  `.drv/output` identity independently. It does not prove that the current
  source evaluates to the retained `.drv`.

Selected-input-closure trust issue:

- `inputClosureByNamePackageFiles` is not recomputable from the current cert.
  The cert contains `30` selected facets, but the protected input-closure set
  contains `232` by-name package files. That set was created at publication by
  scanning the process-global by-name facet snapshot for dependency packages,
  not by serializing a verifiable package-file-to-DRV map.
- Therefore the field is a useful diagnostic/prototype filter, not proof
  authority. A tampered cert can shrink it unless the loader has another
  source-to-DRV binding. Making the field mandatory only prevents old or
  missing-field certs; it does not prevent omission of individual protected
  paths.

Rejected lead: retain existing `DerivedStorePath` deps.

- I removed the v17 store-side filter that dropped `DerivedStorePath` deps and
  rebuilt as `/tmp/nix-cli-derived-retained`.
- Fresh two-commit benchmark through `/tmp/bench-nix-derived-retained`:
  - `cold-stats/0` `1cfd`: `22.206s`,
  - `cold-stats/0` `345`: `0.561s`,
  - duplicate hit still succeeded.
- Cert shape changed from the v17 baseline:
  - deps grew from `36` to `882`,
  - `846` were `derivedStorePath`,
  - `32` remained DOI,
  - `4` remained env.
- Duplicate load cost regressed:
  - `parseUs=8534`,
  - `guardUs=68928`,
  - `verifyUs=117291`,
  - proof load total `198582us`.
- I then ran the same source/DRV splicing falsifier through the benchmark
  harness with the derived-dep cert. It still hit:
  - `candidateChecks=1`,
  - `facetProofAccepted=1`,
  - `hits=1`,
  - proof load total `285320us`.

Causal read:

- `DerivedStorePath` proves source-copy store paths used by evaluation. It does
  not bind a by-name `package.nix` witness to the selected `.drv/output`
  witness. Keeping it adds many independent checks and does not close the
  splicing hole.
- I reverted the one-line derived-dep retention experiment. It is not a useful
  direction unless paired with a separate source-to-current-DRV proof.

Selected-facet recomputation oracle:

- I evaluated the same package facet expression used by the existing unsafe
  semantic-recovery diagnostic for the 15 selected by-name packages across
  `aarch64-linux` and `x86_64-linux`.
- Command shape: import `/home/connorbaker/nixpkgs`, read each `pkgs.<name>`,
  collect `drv.drvPath`, `drv.outputs`, and output paths.
- Wall time was about `1.891s`.

Causal read:

- Recomputing selected package facets is sounder as a comparator, and it would
  catch the source/DRV splice because it recomputes the current package
  derivation from source.
- It is far above the hot-path target. It cannot be the normal serving
  authority if the goal is still to beat the `~0.88s` hot baseline; it is useful
  as a diagnostic/adversarial oracle.

### 2026-05-25 strict deferred-lane witness and recovery checkpoint

Construction-fact bridge attempt:

- First bridge: map `byNameDerivationOutputFacets[*].packageFile` to
  `facet.drvPath`, then annotate deferred by-name package-file facts with that
  `.drv`.
- Result: partial attribution only. `newt` picked up the selected
  `networkmanager` `.drv` paths, but `libvpx` still had no `.drv` because it is
  a lazy construction input of `ffmpeg-headless`, not the package file of the
  selected output facet itself.
- Causal read: selected output facets are not enough to explain construction
  sources. The proof needs a producer-side witness from each traced
  `derivationStrict` invocation.

Producer-side construction witness:

- Added `ByNameDerivationConstructionFileBytes` and recorded it from
  `derivationStrictInternal` while the derivation's by-name package-file reads
  are still in scope.
- Deferred facts now persist `drvPaths` rather than a single optional `drvPath`.
  The loader accepts old `drvPath` for compatibility but validates that new
  `drvPaths` are nonempty strings with no duplicates.
- v18f strict two-commit result:
  - first commit: about `21.7s`;
  - second commit: about `19.0s`;
  - `hits=0`;
  - `deferredLaneRejectedMissingDrv=0`;
  - `deferredLaneRejectedProtected=1`.
- Changed deferred facts now have construction-owner attribution:
  - `pkgs/by-name/li/libvpx/package.nix` maps to selected
    `ffmpeg-headless-8.0.1.drv` paths;
  - `pkgs/by-name/ne/newt/package.nix` maps to selected
    `networkmanager-1.54.3.drv` paths.

Causal read:

- The unsafe fast hit was unsafe for a real reason: changed source files are in
  the construction input lane of selected output derivations.
- The strict miss is also conservative for this concrete pair. Fallback
  evaluation of commit `34513330fcc124d7f204d4621cb71f3e42d59020` produced the
  same JSON output and the same selected output-facet JSON as commit
  `1cfdcfa5620e13451b5c869742dd7cc00fb02e81`.
- Therefore the design is not fundamentally incapable of a hit here; the
  missing proof is a cheap current-source-to-same-owner-derivation equality
  proof, not another unchecked store-path availability shortcut.

Strict output-boundary recovery:

- Added protected-deferred-reject recovery that builds a by-name package exact
  guard candidate from the changed deferred facts, checks store/facet
  dependency availability, re-renders the command through the existing
  output-boundary recovery path, and serves only when recovered JSON and string
  context exactly match the cached output.
- v18g strict two-commit result:
  - first commit: about `21.6s`;
  - second commit: about `7.5s`;
  - `hits=1`;
  - `deferredLaneRejectedProtected=1`;
  - `facetProofAccepted=1`;
  - `outputBoundaryRecoveryAttempts=1`;
  - `outputBoundaryRecoveryHits=1`;
  - `outputBoundaryRecoveryCandidatePackages=2`;
  - `outputBoundaryRecoveryCandidatePaths=2`;
  - `outputBoundaryRecoveryUs=6899545`;
  - proof load total about `7.11s`.

Causal read:

- Full output-boundary recovery proves the cached answer is still correct for
  this commit pair, but the proof is far slower than the `0.886s` regenerated
  hot baseline. It should stay as a diagnostic and adversarial oracle unless a
  later implementation can avoid the full command path.
- A manual current-checkout probe of guessed owner derivations
  (`ffmpeg-headless`, `networkmanager`) across `aarch64-linux` and
  `x86_64-linux` took about `2.335s`. This is cheaper than full recovery but
  still too slow for the hot target, and it is currently only a heuristic
  because the persisted witness records owner `.drv` paths but not the owner
  attr name used to evaluate them.

Next proof leads:

- Persist a construction-owner name witness if one can be made sound. A raw
  derivation name is not enough authority, but it may be useful diagnostics for
  evaluating only affected owners.
- Prefer a derivation-boundary equality proof that checks changed construction
  owners produce the same `.drv`/output identity. It must recompute from current
  source or otherwise bind source bytes to the retained `.drv`; independent
  source-byte and DOI checks are insufficient because a cert can splice one
  package's source witness onto another package's `.drv`.
- Treat input-closure and deferred-lane sets as diagnostics unless each
  protected path is backed by a verifiable source-to-owner-derivation binding.
- Keep full output-boundary recovery as the correctness oracle for evaluating
  narrower designs.

Harness/provenance checkpoint:

- `eval-trace-bench` now records source repo provenance separately from
  benchmark subject/environment provenance. `--allow-provenance-mismatch`
  intentionally ignores only the benchmark subject and environment; it still
  rejects source repo mismatches.
- Pairwise comparison now checks the actual paired commits rather than the full
  dataset commit list. This allows a short candidate/reference window to be
  compared against a longer run without muting commit-order errors inside the
  window.
- The harness test build passed after formatting:
  `71` pytest tests, `ruff check`, `ruff format --check`, and `pyright`.
- Open harness hardening: hash dirty symlink/mode/type metadata, and record a
  deriver/provenance hint for `result`-wrapper Nix subjects when available.

Adversarial review consolidation:

- Facet roots were under-bound. The verifier checked each
  `byNameDerivationOutputFacets` entry internally, but the protected selected
  closure was then derived from the facet set without first proving that the
  facet set covered every drv root in the cached JSON output context.
- Fixed by requiring facet proofs to include all `DrvDeep`/`Built` drv roots
  from the cached output context. A missing root now rejects with
  `facetProofRejectOutputRootMissing` before the facet set can drive selected
  input-closure checks.
- Construction attribution was over-broad. The first producer-side witness
  mapped `relative package path -> drvPath`, ignoring the expected file hash.
  A deferred fact can now inherit construction-owner `.drv` paths only from a
  witness with the same package path and the same expected `FileBytes` hash.
- Deferred-lane well-formedness now treats a repeated relative package path
  inside the same lane as malformed, even if the repeated fact has a different
  expected hash. A sane publication should have one current expected value per
  path; accepting multiple values only creates room for splice-style certs.

Harness fixes from review:

- Pairwise no longer performs the stateful-order compatibility check before it
  has constructed actual same-commit pairs. This keeps a 100-commit baseline
  comparable to a focused 2/10-commit candidate window while preserving the
  later `comparison_commits` check.
- Source repo provenance is captured once for a `generate` invocation and
  reused for all requested modes. This avoids `reference`, `cold`, and `hot`
  manifests disagreeing merely because earlier modes left nixpkgs checked out
  at the last workload commit.
- Rev-built `result/bin/nix` subjects are classified as `result-wrapper` before
  considering whether the surrounding checkout is a dirty Git repository.
  Subject provenance now records the store path and, when available, the
  deriver for that store path.
- Dirty-tree hashing now includes untracked symlink targets, mode/type bits,
  and recursive untracked directory contents, and records error markers instead
  of collapsing unsupported untracked entries to `None`.

Design-space conclusion from parallel review:

- The current design is still not dead. The unsafe lower bound and DOI path put
  proof-load time in the right band; the strict v18f reject is identifying a
  real dependency edge.
- The best remaining lead is a dependency-output boundary proof: if a changed
  dependency package was consumed only through derivation/output identity, then
  current-only re-evaluation of that changed package's persisted output facet
  may discharge the protected change without owner/full-command evaluation.
- This is only sound with an interface classification. A raw "same
  `pkgs.<dependency>.drvPath`" proof is insufficient when the owner can consume
  arbitrary attrs, passthru, metadata, or helper values.

### 2026-05-26 v18f current-source strict protected reject

I rebuilt the current worktree after changing deferred by-name drv attachment to
use the process-global `ByNameDerivationConstructionFileBytes` snapshot instead
of only `ByNameDerivationOutputFacet::byNamePackageFileBytes`.

Build and command:

- built `.#nix-cli` to `/tmp/nix-cli-v18f`,
- immutable subject store path:
  `/nix/store/zvx1csx4plwc6wb77pymy6ip5vx9iw16-nix-2.35.0pre20260515_dirty`,
- benchmark root: `/tmp/bench-nix-v18f-strict`,
- strict env:
  - `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1`,
  - `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_SUPPRESS_CONSTRUCTION_KEYSETS=1`,
  - `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_RELAX_BY_NAME_DIRECTORY_LISTINGS=1`,
  - `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_BY_NAME_OUTPUT_BOUNDARY_RECOVERY=1`,
  - `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_OUTPATH_DEP_FACETS=1`,
  - `NIX_EVAL_TRACE_JSON_ACTION_ALLOW_UNSAFE_ABLATIONS=1`,
  - `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_OUTPATH_DEFER_CONSTRUCTION_BY_NAME_FILEBYTES=1`,
  - `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DERIVATION_OUTPUT_FACET_PROOF=1`,
  - no `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DEFERRED_MISSING_DRV_INPUT_CLOSURE`.

Two-commit stats run:

- `cold-stats/1` `1cfdcfa5620e`: `22.7s`,
- `cold-stats/1` `34513330fcc1`: `18.3s`.

The second commit still missed, but the failure mode changed:

- `candidateChecks=1`,
- `guardAccepted=1`,
- `deferredLaneChecks=1`,
- `deferredLaneRejectedMissingDrv=0`,
- `deferredLaneRejectedMalformed=0`,
- `deferredLaneRejectedProtected=1`,
- `hits=0`,
- proof load total before fallback: `202883us`.

Cert shape changed in the important direction:

- deferred by-name facts: `1782`,
- missing-drv facts: `6`,
- missing-drv unique paths: `3`,
- prior v18e/facet-only publication had `314` missing-drv facts and `157`
  missing-drv unique paths.

Changed deferred files now carry drv witnesses:

- `pkgs/by-name/li/libvpx/package.nix` maps to
  `/nix/store/sxvi91ksrcjb2dn8whz2jv7qxr394xvp-ffmpeg-headless-8.0.1.drv`
  and, in the second lane, also
  `/nix/store/1fpcpr6qamd2w6ziv88350948gn36pvf-ffmpeg-headless-8.0.1.drv`.
- `pkgs/by-name/ne/newt/package.nix` maps to
  `/nix/store/9m34g9iqnhdhhsxi1v275xyjf863b5la-networkmanager-1.54.3.drv`
  and, in the second lane, also
  `/nix/store/dcdx4qvkkjx5wda6j3knm87lvqyam1a5-networkmanager-1.54.3.drv`.
- Querying the selected facet derivation closure confirmed that the
  `ffmpeg-headless` drv paths are in the selected closure. The protected reject
  is therefore not a false missing-witness bug.

The evaluated command JSON for `1cfd` and `345` was byte-identical:

- both `eval.json` files hashed to
  `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`.

Causal read:

- The construction-snapshot witness fixed the immediate libvpx missing-drv
  hole. Strict v18f now knows that the changed `libvpx` file was observed while
  constructing selected-closure `ffmpeg-headless` derivations.
- The unsafe input-closure ablation's earlier hit was therefore not a sound
  proof. It accepted because `inputClosureByNamePackageFiles` did not contain
  `libvpx`, but the current construction witness shows `libvpx` is relevant to
  selected derivations.
- The remaining opportunity is semantic no-op recovery: the source file changed,
  but the produced relevant derivations/output JSON did not.

### 2026-05-26 changed-package oracle measurements

I measured the existing by-name package facet oracle shape outside the loader
using detached worktrees for the two commits.

Changed package oracle:

- expression: import the revision, evaluate `pkgs.libvpx` and `pkgs.newt` for
  `x86_64-linux` and `aarch64-linux`, collect `drvPath`, `outputs`, and output
  paths;
- base `1cfd`: `0.72s`,
- current `345`: `0.59s`,
- JSON facets were identical across the two revisions.

Protected parent package oracle:

- expression: same as above, but for `libvpx`, `newt`, `ffmpeg-headless`, and
  `networkmanager`;
- base `1cfd`: `3.10s`,
- current `345`: `2.24s`,
- facets were identical.

Current-only protected parent oracle:

- expression: `ffmpeg-headless` and `networkmanager` only for the current
  revision and both systems;
- current `345`: `3.02s`.

Adversarial read:

- An own-package by-name oracle explains the semantic no-op cheaply: `libvpx`
  and `newt` themselves still produce identical drv/output facets.
- That is not sufficient serving authority by itself. A parent can depend on a
  package's non-output attrs or passthru values while the package's own drv path
  stays unchanged. In that counterexample, accepting because only the own
  package drv is unchanged would serve stale parent output.
- A parent-package oracle is closer to sound because it recomputes the affected
  parent derivations, but it is already `2.2-3.1s` in this small case and also
  needs a reliable parent attr route. Parsing attr names from drv basenames is
  not a proof.
- Full command output-boundary recovery is sound but degenerates to the normal
  `18s` evaluation for this workload.

Next direction ranking:

1. Record/decompose derivation-construction boundaries so nested dependency
   package file reads do not look like direct parent source reads. This should
   tell whether `libvpx -> ffmpeg-headless` is a dependency-output boundary or a
   genuine parent source read. Without that decomposition, a changed dependency
   package file must conservatively poison the parent.
2. Keep the construction-snapshot drv witness. It is a useful strict reject
   authority and eliminates most missing-drv ambiguity.
3. Use own-package facet recomputation only as a diagnostic/ceiling until it is
   paired with a boundary proof that the parent only consumed the dependency's
   drv/output identity.
4. Do not revive `inputClosureByNamePackageFiles` as authority. The v18f result
   demonstrates it was missing a protected `libvpx` dependency.
5. Do not abandon deferred serving yet. The design is not fundamentally dead;
   it has moved from "missing proof bridge" to "need a sound derivation-boundary
   proof for semantic no-op changed dependencies."

### 2026-05-26 output-context closure correction

Run `cold-stats/90` with the stricter facet/root binding rejected before
output-boundary recovery:

- second commit wall time: `18.48798286600504s`,
- `proofJsonAction.load.hits=0`,
- `deferredLaneRejectedProtected=1`,
- `deferredLaneRejectedMissingDrv=0`,
- `facetProofAttempts=1`,
- `facetProofAccepted=0`,
- `facetProofRejectOutputRootMissing=1`,
- `outputBoundaryRecoveryCandidateCertRejected=1`,
- `outputBoundaryRecoveryAttempts=0`.

Inspecting the cached cert showed the flaw in my hardening:

- the decoded output string context roots are the top-level
  `nixos-system-nixos-26.05pre708350.gfedcba.drv` derivations;
- `byNameDerivationOutputFacets` contains dependency package facets, not those
  top-level system derivations.

So the prior root-coverage check was aimed at the wrong relation. Package
facets can still be valid package/DOI/output facts without being the selected
JSON output roots. The selected/protected root set must instead come from the
decoded JSON output context.

Implementation correction:

- `proofJsonActionDerivationOutputFacetsAccept` once again validates package
  facets internally: package file bytes, construction file bytes, DOI deps,
  drv validity, and output path equality. It no longer requires facet drv paths
  to include every output-context drv root.
- The deferred-lane protected dependency check now computes the derivation input
  closure from `JsonInstallableOutput.context` drv roots and tests changed
  deferred construction-owner drv paths against that closure.
- If the JSON output context has no derivation roots, the strict deferred lane
  falls back instead of treating an empty closure as authority.
- The old missing-drv `inputClosureByNamePackageFiles` acceptance path has been
  disabled. That field was computed from selected package facets, not from the
  decoded output context roots, so it is only diagnostic until rewritten.

Causal expectation:

- This should restore the sound v18g output-boundary recovery diagnostic path:
  the changed `libvpx`/`newt` facts remain protected, but the recovery candidate
  should no longer be rejected merely because dependency package facets are not
  top-level output-context roots.
- It should not create a new proof hit. A hit before the dependency-output
  boundary proof would be suspicious; the expected result is a protected reject
  followed by recovery, with wall time around the prior recovery band rather than
  the full `18s` fallback.

### 2026-05-26 strict correction and dependency-output lower bound

After the output-context closure correction and after disabling the old
missing-drv input-closure authority, I rebuilt `.#nix-cli` and reran the focused
two-commit workload.

Strict path, no unsafe dependency-output lower bound (`cold-stats/91`):

- first commit wall time: `21.488716371997725s`;
- second commit wall time: `7.4712155949673615s`;
- output hashes matched:
  `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`;
- second-commit counters:
  - `proofJsonAction.load.hits=1`,
  - `deferredLaneRejectedProtected=1`,
  - `deferredLaneRejectedMissingDrv=0`,
  - `facetProofAttempts=1`,
  - `facetProofAccepted=1`,
  - `facetProofRejectOutputRootMissing=0`,
  - `outputBoundaryRecoveryAttempts=1`,
  - `outputBoundaryRecoveryHits=1`,
  - `outputBoundaryRecoveryUs=6678900`,
  - `proofJsonAction.load.totalUs=7106232`,
  - `proofJsonAction.load.verifyUs=7024141`.

Causal read:

- The output-context closure correction restored the expected recovery path. The
  previous output-root/facet overbinding was wrong because by-name package
  facets are dependency facts, while the selected JSON roots are the
  top-level output context derivations.
- The old missing-drv input-closure path is no longer authority. New certs stop
  publishing `inputClosureByNamePackageFiles`; missing deferred owner drv
  witnesses now fail closed.
- This path is sounder than the previous ablation, but still too slow: it proves
  equality by re-running the output-boundary recovery path, costing about
  `6.68s` inside recovery on the second commit.

Unsafe dependency-output/current-package lower bound, first implementation
(`cold-stats/92`):

- first commit wall time: `21.344578635995276s`;
- second commit wall time: `1.11034535698127s`;
- output hashes matched;
- second-commit counters:
  - `proofJsonAction.load.hits=1`,
  - `deferredLaneAccepted=1`,
  - `deferredLaneAcceptedChanged=1`,
  - `deferredLaneRejectedProtected=0`,
  - `dependencyOutputBoundaryAttempts=1`,
  - `dependencyOutputBoundaryHits=1`,
  - `dependencyOutputBoundaryOwnerDrvs=4`,
  - `dependencyOutputBoundaryPackageChecks=4`,
  - `dependencyOutputBoundaryCurrentFacetEvalUs=306685`,
  - `outputBoundaryRecoveryAttempts=0`,
  - `proofJsonAction.load.totalUs=762474`,
  - `proofJsonAction.load.verifyUs=688194`.

No-stats focused run `cold/92`:

- first commit wall time: `15.102422723022755s`;
- second commit wall time: `1.1009396350127645s`.

Target-membership closure optimization (`cold-stats/93`):

- first commit wall time: `21.28215038398048s`;
- second commit wall time: `0.8314102239673957s`;
- output hashes matched;
- second-commit counters:
  - `proofJsonAction.load.hits=1`,
  - `deferredLaneAccepted=1`,
  - `deferredLaneAcceptedChanged=1`,
  - `dependencyOutputBoundaryAttempts=1`,
  - `dependencyOutputBoundaryHits=1`,
  - `dependencyOutputBoundaryOwnerDrvs=4`,
  - `dependencyOutputBoundaryPackageChecks=4`,
  - `dependencyOutputBoundaryCurrentFacetEvalUs=298118`,
  - `outputBoundaryRecoveryAttempts=0`,
  - `proofJsonAction.load.totalUs=480040`,
  - `proofJsonAction.load.verifyUs=398759`,
  - `proofJsonAction.load.guardUs=74410`,
  - `proofJsonAction.load.storePathChecks=157`.

No-stats focused run `cold/100`:

- first commit wall time: `15.14162348699756s`;
- second commit wall time: `0.8060931849759072s`;
- output hashes matched.

Current interpretation:

- The lead is performance-relevant. The optimized lower bound moves the focused
  hot case below the regenerated baseline target band around `0.88s` by avoiding
  output-boundary recovery and replacing it with four current-package facet
  checks plus output-context target-membership validation.
- The result is not a sound final design. The unsafe proof shows that each
  protected old owner drv has the current changed package drv as an input, and
  that the changed package's current drv/output facet is equal to the cached
  facet. It does not prove that the parent only consumed the dependency through
  drv/output identity. A parent can still inspect dependency `meta`, `passthru`,
  arbitrary attrs, formals, error behavior, or `attrNames` while the dependency
  drv/output facet remains unchanged.
- The causal bottleneck shifted: strict recovery spends seconds re-evaluating
  the whole output boundary; the lower bound spends roughly `0.30s` in current
  package facet evaluation and roughly `0.18s` in the remaining proof path after
  target-closure optimization. That is the band a sound boundary proof must fit
  inside.
- The next implementation direction is therefore not "make the unsafe flag
  default." It is to record or derive an interface/boundary classification that
  proves selected owners consumed changed dependencies only through accepted
  drv/output identities, then reuse the current-package facet check as the
  semantic no-op discharge.

### 2026-05-26 adversarial review and lower-bound tightening

Parallel adversarial review on the current branch produced the same conclusion
from three angles:

- Soundness review: the unsafe
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DEPENDENCY_OUTPUT_BOUNDARY_CURRENT_PACKAGE`
  path proves only that the current changed package drv is an input of each old
  protected owner drv. It does not prove semantic insensitivity of the owner to
  the changed package file.
- Implementation review: the likely sound path is a producer-side
  derivation-boundary classifier. Useful hooks are `derivationStrictInternal`,
  `coerceToStringWithProvenance`, attr selection, `builtins.getAttr`,
  `builtins.hasAttr`, and `builtins.attrNames`. Allowed consumption must be
  narrow: drv/output identity only; arbitrary attrs, `meta`, `passthru`, shape,
  missing attr behavior, and stringification side effects must fail closed.
- Harness review: run `0` is not a durable baseline after result deletion unless
  regenerated with the fixed harness. Result-wrapper provenance is now better
  for new manifests but still conflates wrapper checkout provenance with subject
  source provenance. Pairwise also still needed an explicit zero-pair guard.

I tightened the unsafe lower-bound verifier without changing its status:

- `proofJsonActionUnsafeDependencyOutputBoundaryCurrentPackageAccepts` now
  parses current package `outputPaths`, not just `drvPath`.
- Owner input matching now checks the exact outputs requested by the old owner
  drv and verifies those output paths against the current package drv.
- This still is not sound authority because it does not prove interface
  classification or owner-set completeness.

Focused benchmark after the tightening:

- `cold-stats/101`:
  - first commit wall time: `21.45938173198374s`;
  - second commit wall time: `0.8134558239835314s`;
  - output hashes matched:
    `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`;
  - second-commit counters:
    - `proofJsonAction.load.hits=1`,
    - `deferredLaneAccepted=1`,
    - `deferredLaneAcceptedChanged=1`,
    - `deferredLaneRejectedProtected=0`,
    - `dependencyOutputBoundaryAttempts=1`,
    - `dependencyOutputBoundaryHits=1`,
    - `dependencyOutputBoundaryRejected=0`,
    - `dependencyOutputBoundaryRejectedOwnerInputMismatch=0`,
    - `dependencyOutputBoundaryOwnerDrvs=4`,
    - `dependencyOutputBoundaryPackageChecks=4`,
    - `dependencyOutputBoundaryCurrentFacetEvalUs=297201`,
    - `outputBoundaryRecoveryAttempts=0`,
    - `proofJsonAction.load.totalUs=473877`,
    - `proofJsonAction.load.verifyUs=397452`,
    - `proofJsonAction.load.guardUs=69727`.
- `cold/101`, no stats:
  - first commit wall time: `15.191485389019363s`;
  - second commit wall time: `0.8011242020293139s`;
  - output hashes matched.

Harness hardening completed:

- `pairwise` now refuses to render if no complete same-commit pairs exist.
- Added a focused semantics test for the zero-pair guard.
- `nix build -L --builders '' .#checks.x86_64-linux.eval-trace-bench` passed:
  `75 passed`, ruff, format check, and pyright.

Causal read:

- The lower-bound result is robust to checking current owner input output names
  and current output paths. The added work did not move the second commit out of
  the target band (`0.801s` no-stats versus `0.806s` before the tightening, well
  within noise).
- The remaining blocker is still semantic authority, not the wall-time shape.
  The next prototype should be shadow-only boundary classification first, then
  serving only if every protected changed owner is classified as dependency
  drv/output-only and the current package facet matches at the required
  outputs.

### 2026-05-26 old package facet ablation and adversarial review

I tried to strengthen the unsafe dependency-output lower bound by persisting
old `packageFacets` on deferred by-name package-file facts and requiring the
old cached package drv/output paths to match the current package facet before
accepting the deferred lane.

Result: this was too strict for the focused workload and killed the lower-bound
hit.

- `cold-stats/102`:
  - first commit wall time: `21.4s`;
  - second commit wall time: `7.6s`;
  - `deferredLaneAccepted=0`;
  - `deferredLaneRejectedProtected=1`;
  - `dependencyOutputBoundaryAttempts=1`;
  - `dependencyOutputBoundaryHits=0`;
  - `dependencyOutputBoundaryRejectedOwnerInputMismatch=1`;
  - `outputBoundaryRecoveryAttempts=1`;
  - `proofJsonAction.load.totalUs=7256340`.
- Certificate inspection showed the changed protected package-file facts for
  `pkgs/by-name/li/libvpx/package.nix` and
  `pkgs/by-name/ne/newt/package.nix` had `packageFacets: null`.

Causal read:

- The old package facet is useful diagnostic/proof material when present, but
  it is not present for the dependency packages that matter in this focused
  transition. Making it a serving precondition forced the path back to full
  output-boundary recovery.
- The reason is structural: `byNameDerivationOutputFacets` records selected
  package derivations that the JSON projection evaluates directly. The protected
  changed files here are dependency packages reached through owner derivations,
  so their old package facets are not necessarily emitted.
- I kept the serialization/parsing support for `packageFacets`, but removed the
  strict serving dependency on them. Serving is still under unsafe ablation
  flags and still lacks semantic boundary authority.

After relaxing the serving predicate back to "current changed package facet is
an input of each protected old owner drv, and the owner-requested outputs match
the current drv output paths", the lower-bound behavior returned:

- `cold-stats/103`:
  - first commit wall time: `21.3s`;
  - second commit wall time: `0.9s`;
  - `deferredLaneAccepted=1`;
  - `deferredLaneAcceptedChanged=1`;
  - `dependencyOutputBoundaryAttempts=1`;
  - `dependencyOutputBoundaryHits=1`;
  - `dependencyOutputBoundaryOwnerDrvs=4`;
  - `dependencyOutputBoundaryPackageChecks=4`;
  - `dependencyOutputBoundaryCurrentFacetEvalUs=307444`;
  - `outputBoundaryRecoveryAttempts=0`;
  - `proofJsonAction.load.totalUs=492207`;
  - `proofJsonAction.load.verifyUs=413155`.
- `cold/103`, no stats:
  - first commit wall time: `15.155685544013977s`;
  - second commit wall time: `0.8096211710362695s`;
  - both outputs hashed to
    `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`.

Adversarial review findings to carry forward:

- Deferred-lane `drvPaths` are only syntax-checked persisted data. They should
  not become sound authorization unless tied to validated construction evidence:
  dropped `FileBytes` fact, expected bytes, owner drv, and retained proof deps.
- Lane `packageFacets` attached from process-global snapshots are only
  diagnostics unless they are proven as a subset of validated
  `byNameDerivationOutputFacets` or another retained proof object.
- `evalByNamePackageDerivationFacets` currently evaluates default
  `import nixpkgs { inherit system; }`, not necessarily the exact flake output,
  overlay, config, or package universe used by the command. That makes it an
  unsafe benchmark oracle unless the workload shape is explicitly restricted.
- Checking dependency package output identity does not prove the owner package
  itself is unchanged. A changed dependency package file could leave drv/output
  identity stable while changing `meta`, `passthru`, attr shape, errors, or
  arbitrary values consumed by the owner.
- The next sound direction is therefore producer-side boundary classification:
  record whether owner construction consumed a dependency only through
  drv/output identity, and fail closed for arbitrary attr selection, shape
  queries, `hasAttr`, `attrNames`, missing-attr behavior, or general
  stringification.

Harness review findings to fix next:

- Pairwise still checks incomplete rows before narrowing to the actual
  comparison window, so long baseline runs versus short target runs can abort
  before the zero-pair guard.
- Stateful subset comparisons ignore cache prefix state. For causal comparison,
  selected windows should be identical prefixes or exact full commit orders
  unless explicitly marked as intersection/noisy.
- `cold/0` and `hot/0` are numbered baseline runs; `reference` is a separate
  unnumbered comparator output. Documentation should keep those concepts
  separate.
- `sourceRef` records the absolute commit-list path and can make equivalent
  commit-list runs incompatible after moving/copying the file. A digest/name
  should be separated from strict workload shape.
- Result-wrapper provenance is improved but still mixes immutable binary
  identity with checkout diagnostics. Strict comparison should key on the store
  binary identity and treat dirty checkout metadata as diagnostic when the
  checkout is only the wrapper source.

Harness fixes applied after that review:

- `pairwise` now builds the comparison window from the two selected runs'
  manifest/directory intersection before reporting incomplete rows. That keeps
  a long baseline versus a short target from failing on commits outside the
  intended comparison window.
- Stateful analysis with a selected comparison window now requires that every
  selected stateful run starts with exactly that window. This rejects the
  unsafe `[A,B,C]` versus `[B,C]` comparison because commit `B` had different
  cache prefix state.
- Strict workload-shape comparison no longer treats the absolute `sourceRef`
  commit-list path as semantic when the manifests already carry the commit
  order and source provenance.
- Result-wrapper benchmark subject comparison now normalizes to immutable
  binary identity (`nixBinSha256`, `nixStorePath`, `nixStoreDeriver`) and treats
  wrapper checkout metadata as diagnostic.
- Verification: `nix build -L --builders ''
  .#checks.x86_64-linux.eval-trace-bench` passed with `79 passed`, ruff,
  format check, and pyright.

## 2026-05-25: harness hardening, fast-memo demotion, and corrected focused runs

After another adversarial pass, I tightened the benchmark harness and separated
the proof-cache result from the exact-head fast-memo lower bound.

Harness hardening applied:

- Manifests now require a `run` field and reject a run directory whose manifest
  names a different run. The check is per-run identity, not equality across
  `cold/0` and `cold/1`.
- `timing.json` now requires a positive finite JSON number for `wallTime`.
  Missing, string-coerced, zero, negative, `NaN`, and `Infinity` wall times are
  rejected as incomplete outputs.
- Source provenance comparison now normalizes clean Git source repos so
  different current `HEAD` values do not invalidate a comparison when
  `commitOrder` pins the evaluated workload. Dirty source provenance remains
  strict.
- Source dirty hashes exclude harness output paths `eval-trace-bench-results/`
  and `result`, so phase-style cold->hot runs do not reject themselves because
  the harness wrote benchmark outputs under the source checkout.
- Result-wrapper subject identity now compares the immutable binary facts
  `nixBinSha256` and `nixStorePath`; `nixStoreDeriver` is diagnostic because
  it can disappear or fail to resolve without changing the binary.
- Pairwise stateful comparisons derive candidates from the shared manifest
  prefix rather than arbitrary dataset/git order when tails diverge.
- Explicit `--baseline-run` expansion now includes requested same-mode
  counterparts even if they are missing, so the normal dataset issue path can
  fail loudly instead of silently omitting the baseline.

Verification:

- `nix build -L --builders '' .#checks.x86_64-linux.eval-trace-bench`
  passed with `87 passed`, ruff, format check, and pyright.

Fast-memo adversarial finding:

- Exact-head fast memo was serving before proof validation and keyed only by
  clean Git `HEAD` plus command/routing material. It checked output-context
  store roots, but not arbitrary non-Git proof deps. That can serve stale JSON
  if an evaluation reads an external absolute file, the file changes, and the
  command source Git `HEAD` stays clean and unchanged.
- I changed `jsonActionCacheExactHeadFastMemoEnabled()` to require the explicit
  unsafe flag
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_EXACT_HEAD_FAST_MEMO=1` in addition to
  `NIX_EVAL_TRACE_JSON_ACTION_ALLOW_UNSAFE_ABLATIONS=1`, and added the flag to
  `proofJsonActionAuthorityMode()`. This keeps the lower-bound experiment
  reproducible while preventing ordinary output-boundary runs from hiding proof
  dependency gaps behind fast memos.

Corrected focused benchmark without exact-head fast memo:

- Built current CLI:
  `/nix/store/n7ydcp0d66fj9qkd4zji5sx0f47061pa-nix-2.35.0pre20260515_dirty`.
- `cold/105`, no stats:
  - `1cfd...`: `15.26860458496958s`;
  - `345...`: `0.8124988210038282s`.
- `hot/105`, no stats:
  - `1cfd...`: `0.37290136696537957s`;
  - `345...`: `0.8389453589916229s`.
- All four outputs hashed to
  `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`.
- Cache inspection showed only proof-v18 files and no `eval-trace-json-action-fast-v1`
  files, so this run measured proof-cache serving rather than fast-memo serving.

Stats for corrected focused proof path:

- `cold-stats/105`:
  - `1cfd...`: `21.327724494971335s`;
  - `345...`: `0.8289584590238519s`.
- `hot-stats/105`:
  - `1cfd...`: `0.35867195698665455s`;
  - `345...`: `0.8313931400189176s`.
- `hot-stats/105` exact-head proof hit for `1cfd...`:
  - `proofJsonAction.load.hits=1`;
  - `deferredLaneAccepted=1`;
  - `deferredLaneAcceptedChanged=0`;
  - `proofJsonAction.load.totalUs=29697`;
  - `verifyUs=17374`;
  - `guardUs=2891`.
- `hot-stats/105` cross-commit duplicate for `345...`:
  - `proofJsonAction.load.hits=1`;
  - `deferredLaneAccepted=1`;
  - `deferredLaneAcceptedChanged=1`;
  - `dependencyOutputBoundaryAttempts=1`;
  - `dependencyOutputBoundaryHits=1`;
  - `dependencyOutputBoundaryOwnerDrvs=4`;
  - `dependencyOutputBoundaryPackageChecks=4`;
  - `dependencyOutputBoundaryCurrentFacetEvalUs=304606`;
  - `proofJsonAction.load.totalUs=492619`;
  - `verifyUs=408762`;
  - `guardUs=70004`.

Causal read:

- The proof path itself is still near the old hot target on this focused
  duplicate (`~0.83s` wall), but it has not beaten it by a large margin.
- The exact-head first commit is cheap because it is an exact proof hit, not
  because of fast memo.
- The cross-commit duplicate still spends about `0.49s` in proof loading, and
  about `0.30s` of that is the current-package facet evaluation. That is now
  the most obvious performance bottleneck in the proof path.
- The dependency-output lower bound remains unsound as authority: it proves the
  current package drv/output appears in old protected owner drvs, but not that
  those owners consumed the dependency only through drv/output identity.

Explicit unsafe exact-head fast-memo lower bound:

- `cold/106`, no stats, with
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_EXACT_HEAD_FAST_MEMO=1`:
  - `1cfd...`: `15.228821430995595s`;
  - `345...`: `0.8162674639606848s`.
- `hot/106`, no stats:
  - `1cfd...`: `0.33456815901445225s`;
  - `345...`: `0.3286384920356795s`.
- Outputs matched the same hash as run 105.
- Cache inspection showed both proof-v18 files and `eval-trace-json-action-fast-v1`
  files.

Causal read:

- The `~0.33s` hot result is real but is an explicitly unsafe exact-head memo
  lower bound. It is useful for measuring the ceiling of "skip all proof work
  on exact source identity", but it should not be used as evidence that the
  cross-commit proof architecture is sound or fast enough.

Current direction:

- Keep lazy positive shape projection as a useful abstraction for future
  navigation, but it is not the current bottleneck.
- The next implementation lead is a producer-side shadow boundary classifier:
  record, without serving from it yet, whether protected owner derivation
  construction consumes changed by-name dependency packages only through
  drv/output identity.
- The classifier should fail closed on attr selection beyond output identity,
  `hasAttr`, `attrNames`, missing-attr behavior, arbitrary stringification, or
  unknown consumption.
- If the known `libvpx`/`newt` focused edges classify as output-only, the unsafe
  dependency-output acceptor can be replaced with a sounder precondition:
  changed file bytes -> dependency derivation facet -> owner input sink for the
  exact output names -> no forbidden owner observations.

### 2026-05-25: construction-owner diagnostic ablation and representation fix

Adversarial review:

- Fresh read-only agents challenged the producer-side `constructionOwners`
  diagnostic, the performance lead, and the benchmark harness.
- The owner-input evidence is owner-wide, not fact-causal: it proves that an
  owner derivation had input drv/output edges while by-name files were observed
  during construction, but not that a specific package file was the sole cause
  of that input edge.
- The first implementation was also representation-wrong: it serialized every
  owner derivation's full input map onto every deferred package-file fact. That
  is diagnostic at best and cannot become serving authority without stricter
  validation and a proof-version/authority decision.
- The performance review pointed out that this diagnostic cannot reduce the
  `~0.30s` current-package facet eval unless it replaces that eval with a sound
  current-equivalence proof. As implemented, the loader ignored the field.
- The harness review found that runs 105/106/108/109 are valid as focused
  two-commit ablations, but not as clean run-0 baseline comparisons. Current
  run 0 is legacy relative to the hardened harness and should be regenerated
  before making final baseline claims.

Bad diagnostic run:

- Built current CLI with the first `constructionOwners` representation:
  `/nix/store/na727512vzv1ah4ayxfxjp82y0pwq33n-nix-2.35.0pre20260515_dirty`.
- `cold-stats/107`:
  - `1cfd...`: `25.44914190203417s`;
  - `345...`: `3.114446833031252s`.
- `hot-stats/107`:
  - `1cfd...`: `2.379117527976632s`;
  - `345...`: `2.974434996023774s`.
- The proof JSON grew from `1669840` bytes in `hot-stats/105` to
  `175993249` bytes in `hot-stats/107`.
- `jq` inspection found `1816` facts with `constructionOwners`,
  `58805` owner records, and `2197689` serialized input-drv mappings.

Causal read:

- The timing regression was not a proof-semantics result. It was JSON bloat
  caused by repeating full owner input graphs across facts.
- That representation would mask any real performance signal, so I changed it
  before drawing conclusions from the diagnostic.

Representation fix:

- Normal proof runs now do not collect or serialize `constructionOwners`.
- The diagnostic path is gated by
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_BY_NAME_PACKAGE_SEMANTICS_DIAGNOSTIC=1`.
- Diagnostic owner summaries are deduped by `(system, owner drv)`.
- Each owner summary now records only:
  - `system`;
  - `drvPath`;
  - `inputDrvCount`;
  - `inputDrvOutputCount`;
  - `matchingInputDrvOutputs` for input drv names that look like the by-name
    package. This is intentionally a heuristic diagnostic, not authority.

Verification:

- `nix build -L --builders '' .#nix-cli` passed after the representation fix.
- Built CLI:
  `/nix/store/20n4sqga1hnmz9g6baz599gxb4885lg8-nix-2.35.0pre20260515_dirty`.

Normal focused stats after the fix:

- `cold-stats/108`:
  - `1cfd...`: `22.8690310040256s`;
  - `345...`: `1.0146446879953146s`.
- `hot-stats/108`:
  - `1cfd...`: `0.4088104420225136s`;
  - `345...`: `0.9616508400067687s`.
- The main proof JSON returned to `1669840` bytes.
- Cross-commit `hot-stats/108` counters for `345...`:
  - `dependencyOutputBoundaryCurrentFacetEvalUs=338747`;
  - `dependencyOutputBoundaryOwnerDrvs=4`;
  - `dependencyOutputBoundaryPackageChecks=4`;
  - `proofJsonAction.load.totalUs=543296`;
  - `verifyUs=457881`;
  - `guardUs=66545`.

Diagnostic focused stats after the fix:

- `cold-stats/109`:
  - `1cfd...`: `23.517013720003888s`;
  - `345...`: `0.9514492219896056s`.
- `hot-stats/109`:
  - `1cfd...`: `0.4409659879747778s`;
  - `345...`: `0.9670051750144921s`.
- The main proof JSON was `2880867` bytes, and the separate existing
  by-name semantics sidecar was `566883` bytes. The diagnostic is no longer a
  176 MB proof-load confounder.
- Compact owner summaries:
  - `1816` facts had `constructionOwners`;
  - `5342` total owner summaries;
  - max `18` owners on one fact;
  - `3603` owner summaries had at least one same-package-looking input match.
- For the focused changed files:
  - `pkgs/by-name/li/libvpx/package.nix` owners were `ffmpeg-headless`
    derivations and matched `libvpx-1.15.2.drv` output `dev`.
  - `pkgs/by-name/ne/newt/package.nix` owners were `networkmanager`
    derivations and matched `newt-0.52.24.drv` output `out`.

Causal read:

- The compact diagnostic supports the hypothesis that these changed by-name
  package files affected the cached top-level output only through dependency
  drv/output identity edges.
- It still does not prove current package equivalence. The acceptor still spends
  about `0.33s` evaluating current package facets, and the new diagnostic does
  not remove that cost.
- This makes two next leads distinct:
  - a sound current-equivalence proof for by-name package drv/output facets;
  - a proof-backed accepted-result sidecar that skips repeated full proof load
    only after a proof has already succeeded for the current authority mode.

### 2026-05-25: accepted-result sidecar exact-head ablation

Implementation state:

- Added an exact-head `*.accepted-result.json` sidecar behind
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_ACCEPTED_RESULT_SIDECAR=1`.
- The sidecar is only a serving shortcut after the normal certificate has been
  accepted for the exact clean `HEAD` proof key. It records the certificate
  hash/ref, proof authority mode, proof key, repo/head/system/eval flags,
  residual deps, store-path availability text ref, and output.
- Cross-commit serving still goes through the normal proof loader and verifier.
  This is intentional for now: accepting a sidecar across commits would need a
  separate proof that the current commit satisfies the same authority, not just
  a previously accepted old result.

Focused run:

- Built current CLI:
  `/nix/store/s8q8b8nhzc1l8r1agm5p7lpm44h1xxgd-nix-2.35.0pre20260515_dirty`.
- Ran focused two-commit sidecar ablation as `cold-stats/111` and
  `hot-stats/111` with the same proof flags as run 108 plus
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_ACCEPTED_RESULT_SIDECAR=1`.
- Sidecars were written for the exact-head `1cfd...` entry in both cold and hot
  state roots. Each sidecar was `10286` bytes.

Timings:

- `cold-stats/111`:
  - `1cfd...`: `22.18628726201132s`;
  - `345...`: `0.8837988549494185s`.
- `hot-stats/111`:
  - `1cfd...`: `0.4200852080248296s`;
  - `345...`: `0.8701462849858217s`.

Counters:

- Exact-head `hot-stats/111` (`1cfd...`) now has:
  - `candidateChecks=0`;
  - `parseUs=0`;
  - `verifyUs=0`;
  - `guardUs=0`;
  - `totalUs=3878`.
- The previous no-sidecar exact-head run still paid roughly `31ms` of proof
  loading (`parseUs=9232`, `verifyUs=17688`, `totalUs=31798` in the failed
  sidecar run 110).
- Cross-commit `hot-stats/111` (`345...`) still has:
  - `dependencyOutputBoundaryCurrentFacetEvalUs=339275`;
  - `verifyUs=447501`;
  - `guardUs=69998`;
  - `totalUs=529755`.

Causal read:

- The sidecar does what it was designed to do: exact clean-HEAD reuse no longer
  reparses and reverifies the full proof certificate.
- The wall-time signal is small and noisy because exact-head hot evaluation has
  a large fixed cost outside the proof loader. This sidecar alone is not a path
  to beating the baseline.
- The dominant remaining cross-commit costs are current package facet
  evaluation and verifier work. The next design-space search should focus on a
  sound current-equivalence proof for by-name package drv/output facts or a
  smaller cross-commit accepted-result authority, not on hardening exact-head
  memoization further.

### 2026-05-25: cross-current accepted-result sidecar ablation

Adversarial review:

- A read-only adversarial pass found a real unsoundness in the first sidecar
  sketch: it bypassed the verifier's current-world git guard/deferred-lane
  checks and trusted the sidecar as authority.
- The exact dirty-worktree concern is mitigated by `buildGitJsonActionCacheKeys`:
  proof keys are only produced after `cleanGitHeadRevision()` accepts the repo.
  Re-running `git status` inside the sidecar loader is therefore redundant for
  this command path and very expensive on the benchmark worktree.
- The sidecar still must not be a free-standing result file. I changed it to
  reference the source certificate by size/hash, re-read that certificate,
  require cert/header identity, require the sidecar's output/deps/store-path
  refs to match the certificate, and for exact-head sidecars re-check the guard
  file identity/acceptance. Cross-current sidecars remain gated behind the
  unsafe ablation flag because the underlying cross-commit acceptance is still
  an ablation.

Implementation change:

- Exact successful cert acceptance still writes an `exact-head-v1` sidecar under
  `<head>.json.accepted-result.json`.
- Cross successful cert acceptance now writes a `cross-current-head-v1` sidecar
  under the current head's `<head>.json.accepted-result.json`, while its
  `certRef` points at the source cert that was actually accepted.
- On load, the sidecar validates:
  - cache/proof/schema/provider/hash identity;
  - proof key, repo root, current head, store dir, current system, eval flags;
  - source cert size/hash and filename;
  - source cert header and source head;
  - sidecar output/deps/store-path refs equal the source cert fields;
  - residual env/session/derived-store-path/derivation-output deps;
  - output context roots and store-path availability.

Bad guarded run:

- Run `cold-stats/112` / `hot-stats/112` included a second
  `cleanGitHeadRevision()` call in the sidecar loader.
- It served both exact and cross sidecars, but `hot-stats/112` still spent about
  `326ms`-`336ms` in uncategorized proof-load time.
- Timings:
  - `hot-stats/112` `1cfd...`: `0.7044778430135921s`;
  - `hot-stats/112` `345...`: `0.7172884659958072s`.
- Causal read: the second `git status --ignored=matching` dominated the sidecar
  path and erased much of the benefit.

Focused sidecar run after removing the redundant clean check:

- Built current CLI:
  `/nix/store/sbz32hl7vdk5ipp04622s7f51s8qik0b-nix-2.35.0pre20260515_dirty`.
- Ran focused two-commit sidecar ablation as `cold-stats/113` and
  `hot-stats/113`.
- Timings:
  - `cold-stats/113` `1cfd...`: `22.93566383898724s`;
  - `cold-stats/113` `345...`: `1.07314564101398s`;
  - `hot-stats/113` `1cfd...`: `0.43492735002655536s`;
  - `hot-stats/113` `345...`: `0.44923071999801323s`.
- Sidecars:
  - exact sidecar:
    `acceptanceKind=exact-head-v1`, `headRev=sourceHeadRev=1cfd...`;
  - cross sidecar:
    `acceptanceKind=cross-current-head-v1`, `headRev=345...`,
    `sourceHeadRev=1cfd...`, `certRef.filename=1cfd...json`.
- Hot counters:
  - exact `1cfd...`: `candidateChecks=0`, `parseUs=0`, `verifyUs=0`,
    `guardUs=0`, `totalUs=47229`;
  - cross `345...`: `candidateChecks=0`, `parseUs=0`, `verifyUs=0`,
    `guardUs=0`, `dependencyOutputBoundaryCurrentFacetEvalUs=0`,
    `totalUs=39354`.
- Output correctness check:
  - All four `eval.json` files from cold/hot and both commits had SHA-256
    `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`.

Causal read:

- The cross-current sidecar is the first ablation in this search that removes
  the dominant hot cross-commit verifier cost: the previous run 111 spent
  `339275us` evaluating current package facets and `447501us` in verifier time
  for `345...`; run 113 spends none of that on the hot hit.
- The focused hot `345...` time moved from `0.8701462849858217s` in run 111 to
  `0.44923071999801323s` in run 113.
- This is promising for hot performance, but it is not a final baseline claim:
  the current restored `reference`/run-0 data is pre-harness-hardening and must
  be regenerated before comparing to the old `cold/938` and `hot/938` numbers.
- Cold remains poor on the seed commit because building the proof remains much
  more expensive than the pre-schema SQLite baseline. If the hot path continues
  to look good on the 10-commit window, the next cold lead is reducing proof
  construction and startup/materialization cost rather than making the sidecar
  loader cheaper.

### 2026-05-25: benchmark harness baseline status

Adversarial harness review:

- The restored `reference`, `cold/0`, and `hot/0` manifests are stale relative
  to the hardened harness. They are missing required fields such as `withStats`,
  and current pairwise/runs commands correctly reject them.
- Therefore, current focused runs 111-113 are only design ablations. They are
  not a fair comparison against run 0/reference.

Next fair comparison procedure:

- Extract the desired commit window from the restored stale manifest before
  moving it aside.
- Move stale `reference`, `cold/0`, and `hot/0` into an archive directory.
- Build the baseline binary with:
  `nix build -L --builders '' ".?rev=92a3df1ab706cf514357ae57b367050b373ff146"`.
- Generate compatible baseline `reference`, `cold/0`, and `hot/0` with the
  hardened harness.
- Rebuild the prototype and generate `cold/1`, `hot/1` over the same commit
  window before using `runs`/`pairwise`.

### 2026-05-25: 10-commit rebaseline and unsafe boundary falsification

Regenerated compatible baseline:

- Archived stale pre-hardening `reference`, `cold/0`, and `hot/0` under
  `eval-trace-bench-results/_archive/2026-05-25-pre-hardening-run0/`.
- Rebuilt the baseline binary with:
  `nix build -L --builders '' ".?rev=92a3df1ab706cf514357ae57b367050b373ff146"`.
- Baseline `result` target:
  `/nix/store/x43bp7y47aqaj6h354mrqbsfnr61iyca-nix-2.35.0pre20260509_92a3df1`.
- Regenerated no-debug/no-stats `reference`, `cold/0`, and `hot/0` for the
  first 10 commits from the stale run-0 manifest.
- New compatible means:
  - `reference`: `7.165099148598s`;
  - `cold/0`: `6.643639611092s`;
  - `hot/0`: `1.156354553392s`.

Unsafe run:

- Rebuilt current prototype:
  `/nix/store/sbz32hl7vdk5ipp04622s7f51s8qik0b-nix-2.35.0pre20260515_dirty`.
- Ran no-debug/no-stats `cold/114` and `hot/114` with the same flags as the
  focused run 113, including
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DEPENDENCY_OUTPUT_BOUNDARY_CURRENT_PACKAGE=1`.
- Timing looked excellent:
  - `cold/114`: mean `3.172402718302s`;
  - `hot/114`: mean `0.372426049295s`.
- Output comparison falsified the design:
  - commits `fd3fbe0cfd62`, `fa143ab863d8`, `1e7e40c82cf9`, and
    `7c395b2da6d6` returned the stale JSON hash
    `a39611216363bc4e8460ad8ecd729aedeae8816d8314fca8a9a13114eac3c0c8`;
  - baseline/reference returned
    `6d24645dee692e4435a52314abb1480c7f5a2b438fe02c9e023ee2ddfdc92c9f`.

Causal read:

- The focused two-commit run did not expose this because both commits happened
  to have identical output JSON.
- The failure is not just a sidecar bug. The cold run itself accepted stale
  cross-commit output for commits where the output should change. The unsafe
  dependency-output-boundary current-package ablation is therefore invalid as a
  serving authority.
- I removed cross-current sidecar writes from the generic
  `verifyProofJsonActionCertificate()` success path. Cross-current sidecars
  should only be written from paths that evaluate the current output and confirm
  equality, such as output-boundary recovery. The unsafe current-package
  boundary flag must be excluded from correctness-gated benchmarks.

### 2026-05-25: exact sidecar hardening and DOI replay memoization

Parallel adversarial review found that the accepted-result sidecar needed to be
closer to normal verifier authority before its numbers could be trusted:

- Exact sidecars must validate source identity guards, observed-key guards, git
  guard ref, and git guard base revision. This is now done on the load path.
- Cross-current accepted-result sidecars are disabled for serving/publication in
  this prototype. A cross-current shortcut is only safe if the normal verifier
  or recovery path has already re-established current-output equality and all
  source/current-world obligations are still replayed.
- Accepted-result sidecars now use v2 and no longer duplicate the large `deps`
  array. Sidecar size on the first-10 run is `2249` bytes instead of roughly
  `894kB`.
- If the unsafe derivation-output facet proof is enabled, accepted-result
  sidecar loading now runs `proofJsonActionDerivationOutputFacetsAccept()`.
- The unsafe facet-proof store path no longer drops non-facet
  `derivationOutputIdentity` deps. This was an authority hole because those deps
  had no replacement verifier obligation.

Implementation refinement:

- DOI residual replay now reads each `.drv` once per proof check, caches all
  static output paths for that derivation, and compares every recorded
  `(drvPath, outputName)` fact against the cached map.
- Added stats counters under
  `evalTrace.projection.proofJsonAction.load`:
  `derivationOutputIdentityDeps`, `derivationOutputIdentityCacheHits`,
  `derivationOutputIdentityCacheMisses`,
  `derivationOutputIdentityTotalUs`, and
  `derivationOutputIdentityReadUs`.

Build status:

- Current benchmark binary:
  `/nix/store/f692n592r6g9yqsghy0ifh4hk1a0kxdd-nix-2.35.0pre20260515_dirty`.
- `libexpr`, `libcmd`, and `nix` built successfully. The aggregate
  `nix build -L --builders '' .#nix` was stopped after the package binary was
  produced; functional tests had already reached a known dirty-branch failure
  in `flakes/source-paths`.

Correctness/timing, no debug/no stats, first 10 commits:

- `cold/123`: mean `4.527818160603s`, min `0.461074728984s`,
  max `17.657329787966s`.
- `hot/123`: mean `0.483130702801s`, min `0.456524058012s`,
  max `0.514211296977s`.
- Outputs matched `reference`.
- Accepted-result sidecars in `cold/123` and `hot/123` are all
  `acceptanceKind=exact-head-v1`; no cross-current accepted-result sidecars were
  published.

Stats run:

- `cold-stats/124`: mean `6.071483456396s`, min `0.458478231041s`,
  max `21.142892446951s`.
- `hot-stats/124`: mean `0.489886316302s`, min `0.462334071985s`,
  max `0.536242663977s`.
- Outputs matched `reference`.
- Hot proof-load totals moved from `2,013,813us` in `hot-stats/119` to
  `1,542,816us` in `hot-stats/124`. Verify time moved from `1,314,691us` to
  `975,779us`.
- DOI replay work in `hot-stats/124`: `30,130` DOI deps across the 10 commits,
  `15,290` cache hits, `14,840` cache misses, `473,760us` total DOI replay,
  and `256,492us` spent reading derivations. Each proof has `3013` DOI deps and
  `1484` unique drv paths.
- Cold slow commits are still dominated by outPath child construction and proof
  publication:
  - `7d761`: `outPathPublish.totalUs=19147504`,
    `childConstructionUs=18804541`, `store.totalUs=953310`;
  - `fd3fbe`: `outPathPublish.totalUs=15655312`,
    `childConstructionUs=15365047`, `store.totalUs=1200172`;
  - `7fb36`: `outPathPublish.totalUs=15802119`,
    `childConstructionUs=15440149`, `store.totalUs=1105667`.

Causal read:

- The DOI memoization is now causal enough to keep. It directly reduces repeated
  derivation reads inside each proof, and the stats counters show the expected
  hit/miss shape.
- Exact accepted-result sidecars remain useful but limited: they cut parse,
  guard, and full verifier time for the three exact changed-output commits.
  The seven alias/non-exact commits still run normal proof verification.
- The next hot lead is not projection routing. Valid stats still show
  `routeAccepted=0`; shape projection remains a router abstraction, not the
  current source of the benchmark win.
- The next cold lead is direct-output proof capture or publication-side guard
  work. Cold outliers are not proof-replay-bound; they are outPath construction
  and proof-store-bound.

### 2026-05-25: direct-output proof capture and source-identity hardening

Direct-output proof capture ablation:

- Built current prototype with direct-output proof capture enabled and compared
  against the regenerated compatible run-0 baseline.
- No-debug/no-stats results:
  - `cold/125`: mean `4.087222195195s`, min `0.443869768060s`,
    max `14.622782692080s`;
  - `hot/125`: mean `0.486246014101s`, min `0.452679274022s`,
    max `0.510359706939s`.
- Outputs matched `reference`.
- The win is cold-only and concentrated in the three changed-output slow
  commits. Hot remained essentially unchanged from `hot/123`.

Stats read:

- `cold-stats/126`: mean `4.847526554990s`.
- `hot-stats/126`: mean `0.484139844694s`.
- Outputs matched `reference`.
- `outPathPublish.totalUs` went to `0`, replaced by
  `directProofCapture.totalUs=42042611us` over the three slow commits.
- Direct capture recorded `118935` deps across those commits.
- The observed gain is therefore from avoiding the old outPath publication path,
  not from reducing proof authority. The cert shape stayed broad: roughly
  `3013` DOI deps, `846` derived-store-path deps, and `4` env deps per exact
  cert, with the same large package-file guard material.

Soundness hardening:

- Direct-output proof capture initially failed to emit a source identity guard
  even though the accepted-result path expects exact source identity for sound
  serving.
- I changed direct capture to record a `gitRevisionIdentity` dependency under
  the active trace access before rendering.
- `cold-stats/127` / `hot-stats/127` verified outputs still matched reference.
  The representative direct-capture cert now has
  `sourceIdentityGuardsLen=1`.
- This intentionally fixes proof shape, not performance.

Negative DOI batching ablation:

- Tried replacing the per-proof DOI `isValidPath()` calls with a batched
  `queryValidPaths()` prepass.
- Build succeeded, but stats regressed:
  - `cold-stats/128`: mean `5.488317473209s`;
  - `hot-stats/128`: mean `0.560279195197s`.
- Outputs matched reference, so this was a performance-only regression.
- Hot load time worsened from `1,760,647us` in run 127 to `2,176,428us` in run
  128. DOI total worsened from `560,418us` to `982,549us`, while DOI read time
  stayed roughly flat.
- Causal read: validity batching was more expensive than the repeated local
  validity probes for this workload. I reverted the batching patch and kept the
  existing per-proof derivation-output map cache.

Adversarial review fixes:

- A reviewer found that the unsafe derivation-output facet proof path dropped
  `DerivedStorePath` deps during proof store without an equivalent replacement
  obligation.
- I removed that skip. The facet path remains exploratory, but the code no
  longer weakens stored authority if the flag is enabled.

Direct-capture phase counters:

- Added counters for direct-capture source-identity deps, git-identity time,
  render time, finalize time, and proof-store time.
- Latest build:
  `/nix/store/y08vbpvzcgvfhk98lbv64ikpdwli1gi0-nix-2.35.0pre20260515_dirty`.
- `cold-stats/129`: mean `5.390357640799s`; outputs matched reference.
- Aggregated direct-capture counters:
  - attempts `3`, successes `3`, fallbacks `0`, unstable deps `0`;
  - deps `118938`, source-identity deps `3`;
  - total `46,201,083us`;
  - git identity `1,844,920us`;
  - render `40,808,554us`;
  - finalize `136,556us`;
  - store `3,409,908us`.

Causal read:

- Direct capture is now sounder and still faster than the old cold publication
  path, but it is not a sufficient end-state. It still evaluates/renders the
  broad output and captures a broad proof.
- The immediate contained lead is to stop recomputing full git identity inside
  each direct-capture render. Session opening already observes git identity for
  the same file-eval root; reusing that hash should recover about `1.8s` total
  on the 10-commit cold-stats run if the repo-root invariant is preserved.
- The larger high-payoff lead remains proof-shape reduction before guard
  publication: a sound by-name/facet discharge would need to replace broad
  package-file/derived-store-path authority with explicit current-world
  obligations, not just omit deps.
- Projection routing remains shelved. Valid runs still show no accepted route
  benefit, and the current bottleneck is command-output proof capture/store, not
  lazy attr-path traversal.

### 2026-05-25: source-identity guard replay and git-identity reuse

Adversarial review found a real soundness gap:

- Direct-output proof capture moved `GitRevisionIdentity` deps into
  `sourceIdentityGuards`, but the replay path only checked guard shape and
  decodability.
- That meant a cached JSON action could serve without validating that the
  recorded root source identity was the clean certificate base that the git
  guard reasons from.
- Direct capture also needed to fail closed when it could not record a stable
  source identity.

Implementation:

- Added `computeCleanGitIdentityHash(headRev)`, a cheap helper for the clean
  GitIdentity digest. It is valid only after a separate clean-worktree/head
  check.
- `GitJsonActionCacheKeys` now carries the clean current identity hash for the
  current clean action key.
- `InstallableAttrPath` stores a session-open source snapshot with proof repo
  root, observed repo root, clean head rev, and observed identity hash.
- Direct capture records a source-identity dep only when:
  - the session-open snapshot proof repo root matches the action key repo root,
  - the snapshot clean head matches the action key head,
  - the snapshot hash equals the action key's clean identity hash, and
  - a post-render `cleanGitHeadRevision()` check still reports the same head.
- If those checks fail, direct capture returns the rendered output but skips
  proof storage.
- Source-identity guard replay now requires a non-empty guard and validates the
  guard against the certificate's clean base head. It must not compare against
  the current head for cross-commit hits; the git guard is the current-world
  proof from the certificate base to the current clean head.
- The active benchmark mode remains proof namespace v15. Stale artifacts without
  a source guard fail closed because replay now requires a non-empty guard.

Negative intermediate:

- `cold-stats/130` originally compared `sourceIdentityGuards` to the current
  clean action key. That made every cross-commit proof miss:
  - `cold-stats/130`: mean `15.699298178894s`;
  - `hot-stats/130`: mean `0.497964178404s`.
- Outputs still matched reference, so this was a performance/semantics issue,
  not an output-correctness issue.
- Causal read: exact current source identity is too strong for cross-commit proof
  replay. The source guard proves the clean certificate base; the git guard
  proves the relevant diff to current.

Corrected run:

- Build:
  `/nix/store/1rrhcib5nhvj9dh06d8pi909jn3m3g1q-nix-2.35.0pre20260515_dirty`.
- `cold-stats/131`: mean `5.284838897001s`, min `0.474825904996s`,
  max `18.271303403017s`.
- `hot-stats/131`: mean `0.528166104510s`, min `0.491461660014s`,
  max `0.570245086041s`.
- Outputs matched `reference` by SHA-256 for all 10 commits.
- Direct-capture counters in `cold-stats/131`:
  - attempts `3`, successes `3`, fallbacks `0`, unstable deps `0`;
  - deps `118938`, source-identity deps `3`;
  - total `44,869,561us`;
  - git identity `845,506us`;
  - render `40,570,407us`;
  - finalize `40,182us`;
  - store `3,412,357us`.
- Compared with `cold-stats/129`, git/source-identity time fell from
  `1,844,920us` to `845,506us`. The remaining direct-capture cost is still
  overwhelmingly render plus proof store.

Causal read:

- The source-identity hardening is now compatible with cross-commit proof hits.
- The git-identity reuse/check saves about `1s` on the 10-commit stats run,
  but it does not change the design direction.
- Hot stayed within the same broad band (`0.49s`-`0.53s` stats runs). The next
  win is not source-identity plumbing; it is avoiding the full direct render or
  narrowing the proof authority published for the tiny `{ system -> outPath }`
  JSON result.
- The best current lead from adversarial review is a verified-trace direct
  action builder: use verifier-accepted cached values for the target attrset and
  child `outPath`s to build the JSON action certificate, instead of falling all
  the way back to no-trace re-render when route/projection metadata is missing.

### 2026-05-25: adversarial review of trace-builder lead and root-output proof

Parallel adversarial review changed the priority order:

- Direct-output proof capture currently returns before the trace-session
  projection path and stores only proof-action files. It does not publish
  normalized trace rows or projection rows on proof misses.
- The run-131 artifacts confirm that: direct capture succeeded `3/3`, while
  `evalTrace.record.count`, outPath publication, route counters, and direct
  trace value hits stayed at zero.
- Therefore moving the route-chain gate or adding a verified-trace builder
  inside `tryServeOutPathJsonObject()` cannot help the current benchmark unless
  direct capture first publishes narrow rows. With current persisted facts, that
  path has no data to consume.
- The stronger lead is proof-shape reduction: each successful cert retained
  `3013` `derivationOutputIdentity` deps, `846` `derivedStorePath` deps, and 4
  environment deps to prove a JSON object with two output paths.

Soundness cleanup from review:

- Direct proof capture now returns whether a proof certificate was actually
  stored. The exact-head fast memo is only populated when proof storage
  succeeded, so an unstable/unsupported proof capture cannot seed the unproven
  fast memo.
- Direct capture now also requires the observed git repo root to match the proof
  repo root before recording the synthetic source-identity dep.
- `derivationOutputIdentity` replay now threads missing `.drv` paths into the
  existing missing-store-path warning path. This covers the requested fallback
  warning when a cached result is otherwise available but required store roots
  are gone.

Root-output proof experiment:

- I implemented a root-output verifier that checks the cached JSON object is
  fully explained by `Built` string-context roots: every JSON string must match a
  declared output of a valid root `.drv`, and every context root must appear in
  the JSON result.
- Broad application of that verifier to cross-commit aliases was unsafe.
  `cold-stats/132` had good-looking timings (`cold` mean `3.869781598414s`,
  `hot` mean `0.421471041907s`) and eliminated DOI replay, but it returned the
  wrong cached output for 4 commits.
- Causal read: the current git/source/observed-key guards do not by themselves
  prove that an older accepted cert's root outputs are the current evaluation's
  root outputs. The broad `derivationOutputIdentity` facts were rejecting those
  aliases. Root-output discharge cannot safely replace them for cross-commit
  replay without an additional current-evaluation boundary proof.

Narrowed exact-head result:

- I restricted root-output discharge to exact-head accepted-result sidecars. In
  that mode, the sidecar cert head must equal the current clean head and the
  source cert/output refs must match exactly. Cross-commit aliases still verify
  the full DOI set.
- Build: `/nix/store/bqgvy237djri0zpfpah5fj08xvzyicy6-nix-2.35.0pre20260515_dirty`.
- `cold-stats/133`: mean `5.473727389210s`; outputs matched reference.
- `hot-stats/133`: mean `0.492651303712s`; outputs matched reference.
- Stats counters:
  - hot proof load attempts/hits/misses: `10/10/0`;
  - hot DOI deps dropped from `30130` in run 131 to `21091`;
  - hot DOI time dropped from `589821us` in run 131 to `369490us`;
  - hot proof total dropped from `1880790us` in run 131 to `1366238us`.
- No-stats run:
  - `cold/134`: mean `4.617586982599s`; outputs matched reference;
  - `hot/134`: mean `0.471849701420s`; outputs matched reference.

Causal read:

- Exact-head sidecar root-output discharge is sound enough for the current proof
  shape and gives a small but real hot win.
- It is not the main design breakthrough. The remaining `21091` DOI deps are
  the seven cross-commit alias hits; replacing those requires a proof that the
  current evaluation still reaches the same top-level root derivations, not just
  that the old roots are valid.
- The next useful search direction is a current-output-boundary proof for
  aliases: either cheaply recompute only the current root derivation identities,
  or capture/prove enough package/root facets during miss-time to show that
  changed by-name packages cannot affect the selected roots. Without that
  additional authority, root-output shortcuts are unsound.

### 2026-05-25: cross-head verified sidecar prototype

Adversarial review found two hardening gaps in the narrowed exact-head result:

- Accepted-result sidecar serving and exact-head fast memo serving trusted the
  clean worktree check performed while building cache keys. I added a serve-time
  clean-head recheck before fast memo, accepted-result sidecar, full proof-cache
  hit, and output-boundary recovery returns.
- The root-output construction-dependency discharge was a boolean. I replaced it
  with an explicit authority enum so a future cross-head path cannot
  accidentally reuse the exact-head-only shortcut by passing `true`.

I also added warning coverage for required `.drv` paths that are valid according
to the store but unreadable/corrupt when DOI or root-output proof replay tries to
read them.

New prototype:

- A `cross-head-verified-v1` accepted-result sidecar is written only after the
  full verifier accepts a non-exact source certificate for the current head.
- Recovery paths that render current output and compare it to the cached output
  intentionally do not write this sidecar yet; the sidecar is currently a memo of
  full verifier acceptance, not a new output-boundary authority.
- Replay rechecks the source certificate ref/hash, git guard, source identity
  guards, deferred dependency lanes, Nix observed-key guards, store path proof
  refs, output/context equality, root `.drv` validity/output equality, and
  serve-time clean head. Only then can it skip the `derivedStorePath` and
  `derivationOutputIdentity` deps.
- The sidecar is a trusted local-cache verifier memo. It is not tamper-resistant:
  if a user edits the proof cache, the loader cannot independently prove the
  sidecar was written after full verifier acceptance. Production authority would
  need protected provenance or would need to keep re-running full DOI.
- After adversarial review, cross-head sidecar writing/loading is additionally
  disabled when unsafe ablation authority modes are enabled.

Hypothesis:

- In cold 10-commit runs, the first alias verification for each current head
  should still pay full DOI, then write a cross-head sidecar at the exact current
  head path.
- In the following hot run, the seven alias heads that previously paid `3013`
  DOI deps each should load their cross-head sidecars and skip DOI, moving hot
  time toward the run-132 performance without reintroducing the run-132
  wrong-output bug.
- The new serve-time clean-head recheck may add fixed overhead. If hot time gets
  worse despite DOI dep counts dropping, the recheck cost is the first causal
  suspect to measure separately.

Result and ablation:

- Build with full serve-time `cleanGitHeadRevision()` recheck:
  `/nix/store/zcx0nz9a2fry3pv0f9ds296j9wv1z8kx-nix-2.35.0pre20260515_dirty`.
- `cold-stats/135`: mean `5.489294257003s`; outputs matched reference.
- `hot-stats/135`: mean `0.679442277201s`; outputs matched reference.
- Hot counters: attempts/hits/misses `10/10/0`, candidate checks `0`, DOI deps
  `0`, DOI time `0`, proof load total `3,288,289us`.
- The sidecar did eliminate DOI, but the full serve-time clean check dominated:
  `git status --porcelain=v1 --untracked-files=all --ignored=matching` measured
  about `0.310s` in `/home/connorbaker/nixpkgs`.

Refinement:

- Keep the full clean/untracked/ignored check when building the cache key.
- Replace the serve-time recheck with a cheaper head/tracked-change check:
  `rev-parse HEAD` plus `git status --porcelain=v1 --untracked-files=no`.
- This does not fully close the untracked-file race after key construction, so
  it is a pragmatic benchmark prototype guard rather than the final concurrency
  story. The final design needs either a stronger source snapshot/lock or a
  cheap way to prove no untracked source became relevant between keying and
  serving.

Refined result:

- Build:
  `/nix/store/8p4dqaaa7qsag3rlqz4w83qg8i1gbwpm-nix-2.35.0pre20260515_dirty`.
- `cold-stats/136`: mean `5.405867372995s`, min `0.521711651003s`,
  max `18.554261341982s`; outputs matched reference.
- `hot-stats/136`: mean `0.443681633909s`, min `0.394041017047s`,
  max `0.499989860982s`; outputs matched reference.
- Hot counters:
  - attempts/hits/misses `10/10/0`;
  - candidate checks `0`;
  - DOI deps `0`, DOI time `0`;
  - proof load total `870,134us`;
  - accepted-result sidecars present: `3` exact-head, `7`
    cross-head-verified.
- Causal read: the cross-head verified sidecar is doing real work. It turns the
  seven alias heads from full DOI replay in hot (`21091` DOI deps in run 133/134
  era) into sidecar loads. The full clean recheck was the only reason the first
  implementation regressed. The remaining gap to the unsound run-132 hot time is
  mostly sidecar replay overhead that is still safety-relevant: guard/source
  authority, root `.drv` reads, store path checks, and the cheap source recheck.

No-stats comparison:

- `cold/137`: mean `4.763890248782s`, min `0.578423146973s`,
  max `16.302135360951s`; outputs matched reference.
- `hot/137`: mean `0.464062917803s`, min `0.433718940999s`,
  max `0.509496368002s`; outputs matched reference.
- Hot accepted-result sidecars: `3` exact-head and `7`
  cross-head-verified.
- Compared with the regenerated run-0 baseline (`cold/0` `6.643639611092s`,
  `hot/0` `1.156354553392s`) this is materially faster.
- Compared with the previous narrowed exact-head no-stats run (`cold/134`
  `4.617586982599s`, `hot/134` `0.471849701420s`), hot improved slightly and
  cold regressed slightly. The hot improvement is smaller than the stats
  counters suggest, so the next search should target sidecar replay overhead and
  the fixed source-keying cost, not DOI.

### 2026-05-26: accepted-result construction-discharge review

Parallel review after the cross-head sidecar prototype challenged the remaining
authority model:

- Soundness review: the accepted-result loader was skipping both
  `derivedStorePath` and `derivationOutputIdentity` replay whenever root-output
  acceptance succeeded. That was implicit in the code rather than represented as
  a proof/memo object. The same review also repeated the known caveat that the
  serve-time source recheck now ignores untracked/ignored files after key
  construction; this is still a prototype guard, not the final source-locking
  design.
- Performance review: on `hot-stats/139`, the big accepted-result sidecar
  buckets were source recheck (`348858us`), sidecar/cert reads (`145073us`),
  guard replay (`125242us`), residual scan (`62400us`), and observed-key verify
  (`46107us`). DOI/store-path counters were zero because construction deps were
  being discharged.
- Design-space review: keep the cross-head accepted-result sidecar as the
  routing/memo layer, but do not invest further in generic projection routing
  for this workload. The next real design target remains a current-root /
  output-boundary proof for aliases, with source snapshot/locking as a safety
  substrate.

Micro-optimizations tried before the review landed:

- Replaced serve-time `git status --untracked-files=no` with a single
  `git diff-index --quiet HEAD --`.
- Read/hash/parse the source certificate once in accepted-result sidecar
  loading instead of doing a separate certificate-ref read.
- Avoided decoding `expected` for construction deps before a possible
  discharge.

Results:

- `cold-stats/140`: mean `5.525346620404s`; outputs matched.
- `hot-stats/140`: mean `0.496502417902s`; outputs matched.
  Hot counters: sidecar hits `10/10`, exact/cross `3/7`, DOI deps `0`,
  source recheck `253293us`, sidecar read `281859us`, guard `123728us`,
  residual `170931us`.
- Repeat run `141`: `cold-stats/141` mean `5.398819124891s`,
  `hot-stats/141` mean `0.450664133596s`; outputs matched.
  Hot counters: source recheck `253289us`, sidecar read `200900us`, guard
  `203573us`, residual `28651us`, DOI deps `0`.

Causal read:

- The new counter accounting made `readUs` include source-cert hashing/parsing,
  so run `139` and run `140` are not directly comparable on that bucket.
- The single `diff-index` recheck reduced the source recheck bucket relative to
  run `139` but did not move wall time reliably. Run-to-run noise and shifted
  accounting dominate this small optimization. Keep the simpler single
  `diff-index` recheck, but do not treat it as the next performance lever.

Conservative ablation:

- I removed construction-dep discharge from accepted-result sidecar loading:
  sidecars still routed, but `derivedStorePath` and `derivationOutputIdentity`
  were replayed.
- Build: `/nix/store/z83z70izbf64j3fdksvxd92kxwgqcxs3-nix-2.35.0pre20260515_dirty`.
- `cold-stats/142`: mean `5.394612917397s`; outputs matched.
- `hot-stats/142`: mean `0.618259436189s`; outputs matched.
  Hot counters: sidecar hits `10/10`, exact/cross `3/7`, source recheck
  `249210us`, sidecar read `355335us`, guard `198268us`, residual
  `1450032us`, DOI deps `30130`, DOI time `569672us`, DOI read `304956us`.

Causal read:

- The regression is exactly where expected: replaying construction deps brought
  back all `30130` DOI deps and made residual replay the dominant hot bucket.
- This proves the hot win is materially caused by construction-dep discharge.
  It does not by itself prove that the discharge is sound.

Refinement:

- Accepted-result sidecars now carry an explicit
  `constructionDepDischarge` object:
  `kind=output-root-boundary-after-verifier-v1`, the sidecar acceptance kind,
  `requiresOutputRootProof=true`, and the exact counts of discharged
  `derivedStorePath` and `derivationOutputIdentity` deps.
- The loader only skips construction deps when:
  - the source certificate hash/size matches the sidecar cert ref;
  - the sidecar is exact-head or cross-head-verified under the existing
    acceptance-kind checks;
  - output-root proof has accepted the cached JSON/context roots;
  - the discharge object is present and its authority/counts match the parsed
    source certificate.
- Older sidecars without the field fall back to conservative replay.
- This is still a trusted local verifier memo, not tamper-resistant authority.
  It makes the assumption explicit and fail-closed for stale sidecar formats;
  production still needs protected provenance or an independently checkable
  current-root proof.

Validation:

- Build with the explicit discharge field:
  `/nix/store/a1n4yq1annhnhp7zm1cpzs85hbhwn2zr-nix-2.35.0pre20260515_dirty`.
- `cold-stats/143`: mean `5.358000215795s`; outputs matched.
- `hot-stats/143`: mean `0.454513907409s`; outputs matched.
  Hot counters: sidecar hits `10/10`, exact/cross `3/7`, source recheck
  `250629us`, sidecar read `297577us`, guard `207585us`, residual
  `124867us`, DOI deps/time `0`.
- All `10` accepted-result sidecars carried the explicit discharge object:
  `7` cross-head and `3` exact-head, each with `846` `derivedStorePath` deps
  and `3013` `derivationOutputIdentity` deps.
- Cleanup build after removing an unused helper warning:
  `/nix/store/apisqilikxsrm8saakgs0lwhrihg8p7i-nix-2.35.0pre20260515_dirty`.

Next read:

- The prototype remains comfortably faster than the regenerated first-10
  baseline (`cold/0` `6.643639611092s`, `hot/0` `1.156354553392s`).
- The largest remaining hot costs are now fixed source recheck plus
  cert/guard/source authority replay. A compact sidecar can reduce read/parse,
  but the better architectural target is still a real current-root proof for
  aliases so construction discharge is no longer just a trusted memo.

### 2026-05-26: compact accepted-result sidecar v3

Experiment:

- Changed newly written accepted-result sidecars from v2 to v3.
- v3 keeps `certRef` as provenance but no longer reads/hashes/parses the large
  source certificate on load. Instead it copies the replay authority fields
  needed by the current sidecar path:
  - `gitGuardRef`;
  - `sourceIdentityGuards`;
  - `nixObservedKeyGuards`;
  - non-construction `residualDeps`;
  - output-context roots and store-path proof refs;
  - optional deferred-lane/facet fields when present;
  - the explicit `constructionDepDischarge` object from the previous step.
- The loader still reads and replays the guard, source-identity guards,
  observed-key guards, root-output proof, residual deps, store-path refs, and
  serve-time source recheck. It only avoids source-cert I/O/parse.
- Unsafe facet-proof sidecar loading currently rejects the compact path rather
  than trying to replay facet authority without the full source cert.

Validation:

- Build:
  `/nix/store/0hzk02ink56gp27l0c9w90zcsvcca1nv-nix-2.35.0pre20260515_dirty`.
- `cold-stats/144`: mean `5.521565175004s`; outputs matched.
- `hot-stats/144`: mean `0.436566986301s`; outputs matched.
- Hot counters, compared with run `143`:
  - total proof load: `982283us -> 602528us`;
  - sidecar read: `297577us -> 52972us`;
  - residual: `124867us -> 117us`;
  - DOI deps/time stayed `0`;
  - guard stayed substantial: `207585us -> 166585us`;
  - source recheck stayed substantial: `250629us -> 235916us`.
- Sidecar size grew from `24606` bytes total in run `143` to `2262146` bytes in
  run `144` because authority fields are now duplicated into every accepted
  result sidecar.

Causal read:

- The compact sidecar achieved the intended local effect: eliminating source
  cert reads cut the read bucket by about `245ms` across 10 hot hits, and total
  sidecar load time by about `380ms`.
- Wall time moved only from `0.4545s` to `0.4366s`. The remaining floor is now
  dominated by fixed process/eval setup plus serve-time source recheck and guard
  replay; source-cert I/O is no longer the primary hot bottleneck.
- Cold did not improve and may have regressed slightly because v3 duplicates
  authority material into every accepted-result sidecar. This argues for either
  a shared per-source authority blob or stopping v3 size growth before a
  100-commit run.

Next read:

- A shared source-authority blob could preserve the hot read win while avoiding
  duplicate v3 sidecar writes. It would not reduce per-process hot reads unless
  the authority blob is also smaller.
- Guard/observed-key acceptance memoization could reduce the next two hot
  buckets, but that would expand the trusted-local-memo surface: the loader
  would trust that guard/source/observed-key checks succeeded when the sidecar
  was written, rather than replaying them. That needs adversarial review before
  implementation.
- The current-root proof direction remains architecturally cleaner because it
  would turn the construction discharge into independently checkable current
  boundary authority instead of piling more trust into accepted-result sidecars.

### 2026-05-26: rejected source-authority memo and corrected v3 binding

Lower-bound ablation:

- I tried a v4 accepted-result sidecar with a `sourceAuthorityMemo` that skipped
  guard/source/observed-key replay and carried only counts/presence facts.
- Build:
  `/nix/store/z1gsnfk4nvc5pz3pv7zqfa10q0crmsll-nix-2.35.0pre20260515_dirty`.
- `cold-stats/145`: mean `5.419052765291s`; outputs matched.
- `hot-stats/145`: mean `0.437971253117s`; outputs matched.
- Hot counters: proof total `282111us`, read `460us`, guard `0`,
  observed-key verify `0`, source recheck `267813us`, DOI deps/time `0`.
- Sidecars were tiny: `36189` bytes total across the ten hot sidecars.

Adversarial read:

- The v4 memo is not sound enough to keep. It did not bind the exact guard set,
  observed-key proof material, diff fingerprint, or source-certificate digest
  being replaced by the memo.
- Treat run `145` only as a timing lower bound for "what if source authority
  replay were free." It is not an acceptable serving design.
- The v4 code path was removed rather than hardened incrementally because the
  next sound version would need enough exact authority material that it becomes
  either a real proof object or a shared authority blob, not the count-only memo
  that was measured.

Corrected v3:

- The v3 compact sidecar now still avoids parsing the full source certificate,
  but it reads the source certificate bytes and checks the sidecar `certRef`
  size/hash before trusting the copied authority fields.
- This restores the important provenance binding: the compact sidecar can only
  serve as a projection of the source certificate it names.
- Build:
  `/nix/store/8hwkmjjd4s3j4kfbbndgffpfdm8nfm2b-nix-2.35.0pre20260515_dirty`.
- `cold-stats/146`: mean `5.518526268104324s`, min `0.6218754649744369s`,
  max `18.831191384000704s`; outputs matched reference.
- `hot-stats/146`: mean `0.4803287160000764s`, min `0.3831442299997434s`,
  max `0.5527865670155734s`; outputs matched reference.
- Hot counters:
  - sidecar attempts/hits/misses `10/10/0`;
  - exact/cross sidecar hits `3/7`;
  - proof load total `755925us`;
  - accepted-result sidecar total `755714us`;
  - sidecar read `70550us`;
  - guard replay `90825us`;
  - observed-key verify `160645us`;
  - root proof `2253us`;
  - residual `108us`;
  - serve-time source recheck `241536us`;
  - DOI deps/time `0`.
- Sidecar size is unchanged from v3: `2262146` bytes total, roughly `226KB`
  per accepted-result sidecar.

Causal read:

- Compared with run `144`, the corrected source-cert hash check moved sidecar
  read from `52972us` to `70550us`. That is the direct cost of reintroducing the
  cert binding without full JSON parse.
- Hot wall time moved from `0.4366s` to `0.4803s`. Some of that is normal
  benchmark variance, but the direction is consistent with the extra cert read
  plus a larger observed-key verify bucket in run `146`.
- Compared with regenerated run `0` (`cold` `6.643639611092s`, `hot`
  `1.156354553392s`), corrected v3 is still materially ahead: cold improves by
  about `1.13s` and hot by about `0.68s` on the first ten commits.
- The source recheck, guard replay, and observed-key verify buckets are now the
  meaningful hot floor. Removing them by memoization is only acceptable if the
  replacement binds exact proof material rather than counts.

Next read:

- A shared source-authority blob is useful mainly to reduce duplicate v3
  sidecar writes and cold/disk pressure. It probably does not move hot much by
  itself unless it also reduces per-process reads.
- A source-authority acceptance memo remains performance-relevant, but the v4
  attempt shows the shape it must not have. It needs exact authority binding or
  an independently checkable current-root/source snapshot proof.
- The current-root/output-boundary proof remains the best architectural next
  direction: it would make construction-dep discharge and alias serving depend
  on a small current proof instead of an accepted-result local memo.

### 2026-05-26: v3 soundness hardening after adversarial review

Adversarial findings:

- The compact v3 loader was hashing the referenced source cert, but then using
  authority fields copied from the sidecar without proving those fields matched
  the source cert. A tampered sidecar could have changed residual deps,
  observed-key guards, guard refs, or discharge counts while leaving `certRef`
  pointed at a real cert.
- The v3 fallback path was also wrong for omitted construction deps. If
  `residualDepsOmitDischargedConstructionDeps=true` and output-root discharge
  failed, the loader attempted to replay only the residual dep list. Since the
  construction deps were omitted from that list, this was not conservative
  replay.

Fix:

- Compact v3 now still checks the `certRef` size/hash, but it also parses the
  source certificate and verifies that the sidecar is a projection of it:
  - `residualDeps` must equal the non-construction deps extracted from the
    source cert;
  - `storePathAvailabilityTextRef`, `gitGuardRef`, `sourceIdentityGuards`, and
    `nixObservedKeyGuards` must match exactly;
  - optional copied fields such as `outputContextStorePathRoots`,
    `unsafeDeferredDependencyLanes`, and `byNameDerivationOutputFacets` must
    have the same presence and value;
  - `constructionDepDischarge` counts must match the construction-dep counts
    extracted from the source cert.
- The residual replay path now sees the full source-cert dep list. If discharge
  is missing or output-root proof fails, construction deps are replayed instead
  of silently skipped.

Validation:

- `git diff --check -- src/libcmd/installable-attr-path.cc
  eval-trace-cache-rewrite-tasks.md`: clean.
- `cold-stats/147`: mean `5.433695015189005s`, min
  `0.607429786992725s`, max `18.776407213998027s`; outputs matched
  reference.
- `hot-stats/147`: mean `0.4523606785049196s`, min
  `0.3998916520504281s`, max `0.518229864013847s`; outputs matched
  reference.
- Hot counters:
  - sidecar attempts/hits/misses `10/10/0`;
  - exact/cross sidecar hits `3/7`;
  - proof load total `553623us`;
  - accepted-result sidecar total `553420us`;
  - sidecar read `79465us`;
  - guard replay `90193us`;
  - observed-key guards `40`, skipped unchanged `32`, verified `8`;
  - observed-key verify `49384us`;
  - root proof `2231us`;
  - residual `106us`;
  - serve-time source recheck `247016us`;
  - DOI deps/time `0`.
- Sidecar size remains `2262146` bytes total across the ten accepted-result
  sidecars.
- `nix build -L .#nix-cli --no-link --print-out-paths` later succeeded after
  the diff-sharing refinement, producing
  `/nix/store/b2vhvy0a4x86043qa71xzk18bva4gv8k-nix-2.35.0pre20260515_dirty`.
- A full `nix build -L .#nix --no-link` still fails in the broader
  `nix-functional-tests` dependency with many existing functional failures,
  including eval-trace/flakes/why-depends cases. The command code and benchmark
  path build and run; the full functional suite is not currently a clean
  validation signal for this branch.

Causal read:

- The adversarial concerns were correct. The v3 source-cert hash was necessary
  but not sufficient because it did not bind the copied sidecar authority
  fields.
- The hardened v3 path remains comfortably ahead of the regenerated run-0
  baseline (`cold` `6.643639611092s`, `hot` `1.156354553392s`) and still avoids
  all DOI replay.
- Parsing the source cert did not erase the win. The hot result is also better
  than run `146`, but the observed-key bucket varied enough that I do not claim
  a parser win. The important conclusion is that the sound version still beats
  baseline.

### 2026-05-26: ephemeral git-diff sharing

Experiment:

- The accepted-result loader now computes the `sourceHeadRev..currentHead`
  `git diff --name-status` vector once after reading the guard base revision.
- The same in-memory vector is passed to guard replay, deferred-lane current
  diff checks, and observed-key guards. This is not persisted authority and does
  not memoize across processes; it only avoids duplicate subprocess work inside
  one accepted-result hit.

Validation:

- `nix build -L .#nix-cli --no-link --print-out-paths` succeeded:
  `/nix/store/b2vhvy0a4x86043qa71xzk18bva4gv8k-nix-2.35.0pre20260515_dirty`.
- `cold-stats/148`: mean `5.496982408495387s`, min
  `0.5673767679836601s`, max `19.043769627984148s`; outputs matched
  reference.
- `hot-stats/148`: mean `0.4469925247016363s`, min
  `0.3779419169877656s`, max `0.5144154549925588s`; outputs matched
  reference.
- Hot counters:
  - sidecar attempts/hits/misses `10/10/0`;
  - exact/cross sidecar hits `3/7`;
  - proof load total `909971us`;
  - accepted-result sidecar total `909770us`;
  - sidecar read `176427us`;
  - guard replay `87684us`;
  - observed-key guards `40`, skipped unchanged `32`, verified `8`;
  - observed-key verify `232364us`;
  - root proof `2094us`;
  - residual `109us`;
  - serve-time source recheck `246168us`;
  - DOI deps/time `0`.

Causal read:

- This did not produce a material improvement. Hot wall time moved from
  `0.4524s` to `0.4470s`, which is inside the normal first-10 noise band.
- The directly related guard bucket moved only `90193us -> 87684us` across ten
  hits. The sidecar read and observed-key verify buckets moved the other way,
  which is not explained by diff sharing and appears to be normal variance.
- Keep this as a small local cleanup if it remains simple, but do not spend more
  search time here. The next real leads are still source/current-root proof and
  exact shared authority binding, not more per-hit git subprocess reshuffling.

### 2026-05-26: v5 minimal accepted-result sidecar and residual fast path

Harness correction:

- My first attempted v5 run, `cold-stats/149`/`hot-stats/149`, was not a valid
  v5 measurement. The harness correctly resolved `result/bin/nix`, but
  `result` still pointed at the previous build
  `/nix/store/8hwkmjjd4s3j4kfbbndgffpfdm8nfm2b-nix-2.35.0pre20260515_dirty`.
- The run produced v3 sidecars:
  `nix.eval-trace.json-action-proof.accepted-result.v3`, with the same
  duplicated authority size (`2262146` bytes total). Its `hot-stats/149` mean
  was `0.43780105889891274s`, but I am treating it only as stale-subject
  evidence.
- I then rebuilt the current branch with `nix build -L .#nix-cli --out-link
  result --print-out-paths`, first producing
  `/nix/store/7iwasqnyds0yc8mdck0ffsjjky717824-nix-2.35.0pre20260515_dirty`.

v5 sidecar:

- The accepted-result writer now emits
  `nix.eval-trace.json-action-proof.accepted-result.v5`.
- v5 removes the copied v3 authority fields from the sidecar:
  `gitGuardRef`, `sourceIdentityGuards`, `nixObservedKeyGuards`,
  `residualDeps`, `unsafeDeferredDependencyLanes`, and
  `byNameDerivationOutputFacets`.
- The sidecar keeps only the serving header, `certRef`, store proof scope/ref,
  output, optional output context roots, and `constructionDepDischarge`.
- Authority remains in the referenced source cert. The loader checks
  `certRef` size/hash, parses the source cert, compares output/scope/roots, and
  replays guard/source/observed-key/residual authority from that source cert.
  This avoids the rejected v4 mistake: v5 does not use a count-only authority
  memo and does not trust copied fields that are not bound to the source cert.

Validation before residual optimization:

- `cold-stats/150`: mean `5.202225123811513s`, min `0.520090400998015s`,
  max `18.08696854300797s`; outputs matched reference.
- `hot-stats/150`: mean `0.4252072914969176s`, min `0.3808248889981769s`,
  max `0.48458226694492623s`; outputs matched reference.
- Hot counters:
  - sidecar attempts/hits/misses `10/10/0`;
  - exact/cross sidecar hits `3/7`;
  - proof load total `668954us`;
  - accepted-result sidecar total `668756us`;
  - sidecar read `132445us`;
  - guard replay `92993us`;
  - observed-key guards `40`, skipped unchanged `32`, verified `8`;
  - observed-key verify `37030us`;
  - root proof `2103us`;
  - residual `113353us`;
  - serve-time source recheck `256374us`;
  - DOI deps/time `0`.
- Sidecar size dropped to `25106` bytes total across the ten hot sidecars,
  roughly `2.5KB` each.

Residual fast path:

- v5 initially regressed residual time because
  `proofJsonActionAcceptedResultResidualDepsAccept` copied all
  `derivedStorePath` and `derivationOutputIdentity` JSON entries before
  discovering that construction discharge accepted. On the benchmark source
  certs that means scanning/copying roughly `846` derived store path deps and
  `3013` DOI deps per cert even when DOI replay is skipped.
- I changed that path to count and validate construction deps during the first
  scan, check `constructionDepDischarge`, and only materialize the construction
  dep JSON vectors if discharge fails and conservative replay is actually
  needed.
- Build:
  `/nix/store/yc8qb8phpb58rxcipm9j9519clg1y37b-nix-2.35.0pre20260515_dirty`.
- `cold-stats/151`: mean `5.183077957184286s`, min
  `0.5101763239945285s`, max `18.007905875972938s`; outputs matched
  reference.
- `hot-stats/151`: mean `0.4076654482167214s`, min
  `0.3724026980344206s`, max `0.44374289200641215s`; outputs matched
  reference.
- Hot counters:
  - sidecar attempts/hits/misses `10/10/0`;
  - exact/cross sidecar hits `3/7`;
  - proof load total `559825us`;
  - accepted-result sidecar total `559629us`;
  - sidecar read `113230us`;
  - guard replay `96044us`;
  - observed-key guards `40`, skipped unchanged `32`, verified `8`;
  - observed-key verify `66605us`;
  - root proof `2231us`;
  - residual `6311us`;
  - serve-time source recheck `237809us`;
  - DOI deps/time `0`.
- Sidecar format remained v5 and size remained `25106` bytes total.

Causal read:

- v5 achieves the shared-authority storage goal without sacrificing the hot
  path: it reduced accepted-result sidecar bytes by about `99%` versus v3
  (`2262146` -> `25106`) and the optimized stats hot mean is now better than
  the hardened v3/diff-sharing runs.
- The residual fast path was causal. The directly targeted residual bucket moved
  `113353us -> 6311us`, and hot wall moved `0.4252s -> 0.4077s`.
- The current remaining measured hot floor is source recheck, source-cert read,
  guard replay, and observed-key replay. DOI replay remains eliminated.
- Because these are stats runs, I started a no-debug/no-stats run `152` on the
  same first-ten commit list to compare against regenerated run `0` in the same
  mode.
- No-debug/no-stats `run 152`:
  - build subject:
    `/nix/store/yc8qb8phpb58rxcipm9j9519clg1y37b-nix-2.35.0pre20260515_dirty`;
  - `cold/152`: mean `4.411865864397259s`, min `0.5300932739628479s`,
    max `15.589130321983248s`;
  - `hot/152`: mean `0.4079825976979919s`, min `0.38233846199000254s`,
    max `0.4391949290293269s`;
  - outputs matched reference;
  - sidecars remained v5 and `25106` bytes total.
- Against regenerated no-stats run `0` (`cold` `6.643639611092s`, `hot`
  `1.156354553392s`), optimized v5 improves the first-ten window by about
  `2.23s` cold and `0.75s` hot.

Adversarial review:

- Fresh read-only reviewers did not find an unbound copied-authority hole in
  v5. The design is simpler than v3 precisely because the source cert remains
  authoritative.
- Remaining soundness caveats:
  - worktree TOCTOU remains around observed-key replay because replay reads the
    mutable current worktree and the final source recheck only uses tracked
    `git diff-index`;
  - shared `git diff --name-status` reuse is currently base-safe in the
    accepted-result path, but the helper API still accepts a raw vector and is
    easy for future callers to misuse with the wrong base;
  - `nixObservedKeyGuards` absence is accepted as empty by both the helper and
    normal verifier. In observed-key authority modes, missing guard material
    should probably reject rather than mean "nothing to check";
  - v5 residual replay intentionally handles only the dep kinds relevant to
    these accepted-result source certs and fails closed on unsupported kinds.
- These are hardening items before promoting the prototype. They do not change
  the current benchmark conclusion that accepted-result sidecars are useful and
  materially beat the regenerated run-0 baseline.

Next read:

- Keep v5. It is the cleaner accepted-result sidecar shape and preserves the
  performance signal.
- Measure fixed command/setup cost next. A reviewer pointed out that hot wall is
  still much larger than the explicit proof-load buckets per hit, so a lower
  bound around earlier serving or command setup may be more informative than
  immediately building a full current-root proof.
- Continue current-root/output-boundary as the soundness architecture for
  replacing trusted local verifier memos, but first add shadow counters for
  owner coverage, escaped attr/shape use, output-closure membership, current
  facet eval time, and reject reasons.

### 2026-05-26: JSON serving setup counters and clean-status lower bound

Setup counters:

- Added command-level counters inside `InstallableAttrPath::tryServeJsonOutput`:
  `serveJsonAttempts`, `serveJsonCacheHits`, `serveJsonFallbacks`,
  `serveJsonTotalUs`, `serveJsonBuildKeysUs`, `serveJsonTraceSessionUs`, and
  `serveJsonRenderFallbackUs`.
- Also hardened observed-key proof mode so a missing `nixObservedKeyGuards`
  field is no longer silently accepted as empty proof material.
- Build:
  `/nix/store/zv9j0ywabarj6jxl2b895pxk1d5mxfsn-nix-2.35.0pre20260515_dirty`.
- `cold-stats/153`: mean `5.160709666105686s`, min
  `0.499160023056902s`, max `18.114255738037173s`; outputs matched
  reference.
- `hot-stats/153`: mean `0.40234015609603374s`, min
  `0.37164149997988716s`, max `0.4373730390216224s`; outputs matched
  reference.
- Hot counters:
  - proof load attempts/hits/misses `10/10/0`;
  - `serveJsonAttempts/cacheHits/fallbacks` `10/10/0`;
  - `serveJsonTotalUs` `3496919us`;
  - `serveJsonBuildKeysUs` `2943665us`;
  - `serveJsonTraceSessionUs` and `serveJsonRenderFallbackUs` `0`;
  - accepted-result sidecar attempts/hits `10/10`;
  - exact/cross sidecar hits `3/7`;
  - accepted-result sidecar total `553011us`;
  - sidecar read `111143us`;
  - guard replay `95905us`;
  - observed-key guards `40`, skipped unchanged `32`, verified `8`;
  - observed-key verify `46556us`;
  - root proof `2168us`;
  - residual `5985us`;
  - serve-time source recheck `255844us`;
  - DOI deps/time `0`.

Causal read:

- The next hot bottleneck is not current-root proof yet. It is building the
  command proof keys before sidecar lookup.
- Local shell timing matches the counter: on `/home/connorbaker/nixpkgs`,
  `git status --porcelain=v1 --untracked-files=all --ignored=matching` takes
  about `0.27-0.28s` even when clean. Ten hot invocations therefore explain the
  `2.94s` `serveJsonBuildKeysUs` bucket.
- `git rev-parse --verify HEAD` is about `0.001s`, while tracked-only
  `diff-index --quiet HEAD --` is about `0.020s`. The expensive part is the
  full untracked/ignored worktree scan.

Unsafe lower-bound ablation:

- Added `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_SKIP_CLEAN_KEY_STATUS`, gated by
  `NIX_EVAL_TRACE_JSON_ACTION_ALLOW_UNSAFE_ABLATIONS`.
- When enabled, key construction uses `git rev-parse HEAD` instead of
  `cleanGitHeadRevision`. This is intentionally unsafe: it does not prove that
  untracked/ignored source files could not affect evaluation. It only measures
  the value of replacing the global clean-worktree proof with something scoped
  or snapshot-based.
- Build:
  `/nix/store/896ca7kfk22mdcl8p1dhw2nxsm16f346-nix-2.35.0pre20260515_dirty`.
- `cold-stats/154`: mean `5.0400424025021495s`, min
  `0.21701060398481786s`, max `18.51850776397623s`; outputs matched
  reference.
- `hot-stats/154`: mean `0.21185306090046652s`, min
  `0.0806389480130747s`, max `0.3024435029947199s`; outputs matched
  reference.
- Hot counters:
  - proof load attempts/hits/misses `10/10/0`;
  - `serveJsonAttempts/cacheHits/fallbacks` `10/10/0`;
  - `serveJsonTotalUs` `1606852us`;
  - `serveJsonBuildKeysUs` `10437us`;
  - `serveJsonTraceSessionUs` and `serveJsonRenderFallbackUs` `0`;
  - accepted-result sidecar attempts/hits `10/3`;
  - exact/cross accepted-result sidecar hits `3/0`;
  - accepted-result sidecar total `150179us`;
  - sidecar read `49408us`;
  - guard replay `9711us`;
  - observed-key guards `40`, skipped unchanged `32`, verified `8`;
  - observed-key verify `72733us`;
  - residual `1482us`;
  - serve-time source recheck `256474us`;
  - DOI deps/time `21091` / `374162us`.

Important caveat:

- Run `154` is not an apples-to-apples v5 sidecar lower bound because
  `ALLOW_UNSAFE_ABLATIONS=1` disables cross-head accepted-result sidecar
  writes/loads. Only the three exact-head cases used v5 sidecars; the seven
  cross-head cases fell through slower proof paths and replayed DOI deps.
- Despite that handicap, hot mean still fell by about `0.19s` from run `153`.
  The key-building scan is therefore definitely causal, and a sound replacement
  for global clean status is now the highest-value design lead.

Next read:

- Do not ship the skip-clean-key-status ablation. It is a timing oracle.
- Look for a sound way to avoid a full untracked/ignored repo scan on every hot
  process:
  - a scoped source proof over the files/directories actually covered by the
    cert and observed-key guards;
  - a source snapshot/current-source object passed from key construction through
    guard/source/observed-key replay and final recheck;
  - a benchmark/harness-level source cleanliness token only if it can be made
    part of the trusted input, not just provenance;
  - or a durable clean-status stamp that is invalidated by enough filesystem and
    Git metadata to be sound, which is hard for untracked/ignored files.

### 2026-05-26: Isolated clean-status oracle

Same-binary control:

- Re-ran the binary from the keyed skip-clean ablation with the skip flag off.
- `cold-stats/155`: mean `5.32704930830514s`; outputs matched reference.
- `hot-stats/155`: mean `0.46314231650321747s`; outputs matched reference.
- Hot counters:
  - `serveJsonAttempts/cacheHits/fallbacks` `10/10/0`;
  - `serveJsonBuildKeysUs` `2917428us`;
  - accepted-result sidecar attempts/hits `10/10`;
  - exact/cross accepted-result sidecar hits `3/7`;
  - DOI deps/time `0`.
- This confirms the full clean-status key build is expensive on the same
  executable that produced run `154`; the run `153` -> `154` improvement was
  not just build-subject drift.

Unkeyed unsafe oracle:

- Added `NIX_EVAL_TRACE_JSON_ACTION_UNSAFE_UNKEYED_SKIP_CLEAN_KEY_STATUS`.
- This env var is deliberately ignored by the proof-routing env filter and does
  not add `unsafe-skip-clean-key-status` to the authority mode. It is therefore
  not a serving authority and is not shippable. Its only purpose is to measure
  "what if routing used cheap `git rev-parse HEAD` but the rest of the v5
  accepted-result proof stack stayed enabled."
- Build:
  `/nix/store/kap6lx584r66sqk9b0fnic2shb2rsh1v-nix-2.35.0pre20260515_dirty`.
- `cold-stats/156`: mean `4.986238383984892s`, min
  `0.2104619829915464s`, max `18.005378355970606s`; outputs matched
  reference.
- `hot-stats/156`: mean `0.14445866320165807s`, min
  `0.09544023900525644s`, max `0.2329723570146598s`; outputs matched
  reference.
- Sidecars: `10` accepted-result files, total `25416` bytes, format
  `nix.eval-trace.json-action-proof.accepted-result.v5`.
- Hot counters:
  - proof load attempts/hits/misses `10/10/0`;
  - `serveJsonAttempts/cacheHits/fallbacks` `10/10/0`;
  - `serveJsonTotalUs` `879426us`;
  - `serveJsonBuildKeysUs` `11088us`;
  - `serveJsonTraceSessionUs` and `serveJsonRenderFallbackUs` `0`;
  - accepted-result sidecar attempts/hits `10/10`;
  - exact/cross accepted-result sidecar hits `3/7`;
  - accepted-result sidecar total `868137us`;
  - sidecar read `314649us`;
  - guard replay `119622us`;
  - observed-key guards `40`, skipped unchanged `32`, verified `8`;
  - observed-key verify `134616us`;
  - root proof `2003us`;
  - residual `5377us`;
  - serve-time source recheck `250555us`;
  - DOI deps/time `0`.

Causal read:

- This is the clean isolation that run `154` could not provide. Cross-head v5
  accepted-result sidecars stayed active (`3/7` exact/cross hits), DOI replay
  stayed at zero, and the key-building bucket collapsed from about `2.9s` to
  about `11ms` across ten hot invocations.
- The hot mean moved from the no-stats v5 result `0.4079825976979919s`
  (`hot/152`) and the stats result `0.40234015609603374s` (`hot-stats/153`) to
  `0.14445866320165807s` with stats enabled. That makes global clean-status
  routing the dominant remaining avoidable cost.
- The result is not sound because it can serve while untracked/ignored source
  changes exist. The performance signal is still highly relevant: if a scoped
  source proof can replace the global status scan for less than roughly
  `250-300ms` per process, the accepted-result design has a clear path to a
  large hot-time win over both the regenerated run-0 baseline and current v5.

Next read:

- Build a shippable replacement for global clean routing, not a larger unsafe
  oracle. The likely shape is:
  - route by repository root and HEAD only;
  - after loading a candidate, validate a typed current-source proof against
    exactly the source facts in the cert: tracked changes, exact files,
    directory listings, existence/absence-sensitive paths, recursive paths, and
    observed-key file reads;
  - keep fail-closed fallback to evaluation for unsupported guard kinds or
    ambiguous untracked/ignored cases;
  - keep publication-time full clean checks for now if needed, because the hot
    bottleneck is serving.
- Before flipping serving, add a shadow/scoped proof mode with counters that
  reports whether it would accept whenever the existing global-clean path
  accepts, its runtime, guard counts, and reject reasons. That gives a soundness
  and performance gate before replacing the current authority check.

### 2026-05-26: Scoped source proof shadow

Pathspec shadow attempt:

- Added `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_SCOPED_SOURCE_PROOF_SHADOW`
  as a diagnostic mode. It does not authorize serving; it only runs after the
  existing accepted-result sidecar path has accepted the source guard.
- The first implementation decoded the source guard paths and checked them with
  coalesced `git status --porcelain=v1 --untracked-files=all
  --ignored=matching -- <pathspecs>`.
- Build:
  `/nix/store/3hzbwywvh1qn8fah58jgqnx3d30lrc3a-nix-2.35.0pre20260515_dirty`.
- `cold-stats/157`: mean `5.109390543203335s`; outputs matched reference.
- `hot-stats/157`: mean `0.4824157268041745s`; outputs matched reference.
- Hot counters:
  - accepted-result sidecar attempts/hits `10/10`;
  - exact/cross accepted-result sidecar hits `3/7`;
  - scoped-source attempts/accepted/rejected/unsupported `10/10/0/0`;
  - `scopedSourceShadowUs` `3457047us`;
  - `scopedSourceShadowTrackedDiffUs` `250146us`;
  - `scopedSourceShadowStatusUs` `2930404us`;
  - exact/directories/recursive guard paths `39810/10420/28890` across ten
    runs.

Causal read:

- The proof facts were sufficient for this benchmark: the shadow accepted all
  ten hot serves.
- The implementation was the wrong shape. The guard contains root-level
  directory-listing facts, so pathspec coalescing includes `.` and degenerates
  into a recursive untracked/ignored scan of the whole source tree. That is the
  same work we are trying to avoid.
- This falsifies "just use narrower Git pathspec status" as the serving
  replacement.

Direct tracked-tree shadow:

- Replaced the pathspec status shadow with a direct proof:
  - check `HEAD` plus tracked dirty state with `diff-index --quiet HEAD --`;
  - load tracked files with `git ls-files -z`;
  - derive a tracked direct-child map for directories;
  - for guarded directory listings, scan only the current directory's direct
    children and reject any current child absent from the tracked tree;
  - for exact/structural/recursive guard paths in this benchmark, reject if a
    current filesystem path exists but is not tracked.
- This is still shadow-only and still paired with the unsafe unkeyed routing
  oracle so the benchmark can isolate proof cost.
- Build:
  `/nix/store/4piz9rlr0wpcjmbx7m9wn6y2ffpmarwx-nix-2.35.0pre20260515_dirty`.
- `cold-stats/158`: mean `5.0885297236905895s`, min
  `0.24693990097148344s`, max `18.153447663993575s`; outputs matched
  reference.
- `hot-stats/158`: mean `0.29930533649749125s`, min
  `0.2444437610101886s`, max `0.3805482629686594s`; outputs matched
  reference.
- Sidecars: `10` accepted-result files, total `25416` bytes, format
  `nix.eval-trace.json-action-proof.accepted-result.v5`.
- Hot counters:
  - proof load attempts/hits/misses `10/10/0`;
  - accepted-result sidecar attempts/hits `10/10`;
  - exact/cross accepted-result sidecar hits `3/7`;
  - `serveJsonBuildKeysUs` `10624us`;
  - accepted-result sidecar total `2140464us`;
  - sidecar read `254080us`;
  - guard replay `117840us`;
  - observed-key verify `34342us`;
  - serve-time source recheck `212246us`;
  - scoped-source attempts/accepted/rejected/unsupported `10/10/0/0`;
  - `scopedSourceShadowUs` `1486087us`;
  - `scopedSourceShadowTrackedDiffUs` `247416us`;
  - `scopedSourceShadowTrackedListUs` `361258us`;
  - `scopedSourceShadowDirectoryChecks` `10420`;
  - `scopedSourceShadowPathChecks` `68700`.

Causal read:

- The direct proof keeps the correctness signal from the pathspec attempt
  while cutting shadow time from `3.46s/10` to `1.49s/10`.
- Compared with the same-binary global clean-status control (`hot-stats/155`
  at `0.46314231650321747s`), a serving path shaped like run `158` should still
  be materially faster, even though it gives back about `0.15s` per process
  relative to the unsafe oracle lower bound (`hot-stats/156` at
  `0.14445866320165807s`).
- The remaining proof cost is mostly `git ls-files`, tracked dirty checking,
  and many direct filesystem checks. That suggests there is still a useful
  optimization lead before promotion: reduce redundant path checks, treat
  recursive guard entries precisely, and reuse one tracked-state proof for both
  scoped-source acceptance and final source recheck.

Next read:

- Keep the direct proof as the candidate architecture; the pathspec proof is
  rejected.
- Before using it as serving authority, harden semantics:
  - fail closed or implement full recursion when a recursive guard path is a
    directory, not just a file;
  - make the source-identity check explicitly rely on scoped current-source
    proof instead of accidentally inheriting whole-worktree-clean semantics;
  - decide whether publication still requires a full clean worktree while
    serving can be scoped;
  - add fault-injection tests for untracked direct children in guarded
    directories, tracked dirty files, untracked exact paths, and unsupported
    recursive directory guards.
- Performance next step: add an actual serving experiment that routes by
  repository root and `HEAD`, then requires the direct scoped-source proof before
  returning a v5 sidecar. This should be benchmarked without the unsafe oracle.

Same-binary controls:

- Built a hardened shadow binary that fails closed when a recursive guard path
  is a directory:
  `/nix/store/0jrl73d123f8nn833vaanj4qshidj5q0-nix-2.35.0pre20260515_dirty`.
- `cold-stats/159` standard global-clean routing: mean
  `5.26353535351227s`; outputs matched reference.
- `hot-stats/159` standard global-clean routing: mean
  `0.4553775479027536s`; outputs matched reference.
- Hot counters for run `159`:
  - proof load attempts/hits/misses `10/10/0`;
  - accepted-result sidecar hits `10/10`;
  - exact/cross sidecar hits `3/7`;
  - `serveJsonBuildKeysUs` `2933664us`;
  - `serveSourceRecheckUs` `251949us`.
- `cold-stats/160` unkeyed cheap-routing oracle, no shadow: mean
  `5.166207138902974s`; outputs matched reference.
- `hot-stats/160` unkeyed cheap-routing oracle, no shadow: mean
  `0.15065272490610368s`; outputs matched reference.
- Hot counters for run `160`:
  - accepted-result sidecar hits `10/10`;
  - exact/cross sidecar hits `3/7`;
  - `serveJsonBuildKeysUs` `12034us`;
  - `serveSourceRecheckUs` `270896us`.
- `cold-stats/161` unkeyed cheap-routing oracle plus hardened shadow: mean
  `5.138940003607422s`; outputs matched reference.
- `hot-stats/161` unkeyed cheap-routing oracle plus hardened shadow: mean
  `0.2827662342984695s`; outputs matched reference.
- Hot counters for run `161`:
  - accepted-result sidecar hits `10/10`;
  - exact/cross sidecar hits `3/7`;
  - `serveJsonBuildKeysUs` `11001us`;
  - scoped-source attempts/accepted/rejected/unsupported `10/10/0/0`;
  - `scopedSourceShadowUs` `1541320us`;
  - `scopedSourceShadowTrackedDiffUs` `250921us`;
  - `scopedSourceShadowTrackedListUs` `404996us`;
  - recursive-directory unsupported `0`;
  - untracked/ignored rejects `0`.

Causal read:

- The same-binary sequence confirms that the earlier result was not executable
  drift:
  - global clean routing costs about `2.9s/10` in key construction;
  - cheap routing plus proof sidecars costs about `0.15s` hot/stats;
  - adding the direct scoped proof raises hot/stats to about `0.28s`, still far
    below the global-clean path.
- The recursive-directory fail-closed hardening had no accept-rate cost on the
  first-ten nixpkgs workload: `scopedSourceShadowRecursiveDirectoryUnsupported`
  stayed at `0`.

### 2026-05-26: Scoped source proof serving experiment

Implementation:

- Added `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_SCOPED_SOURCE_PROOF_SERVE`.
- This is not the unsafe oracle:
  - the flag is ignored by impure-env routing scope so the command environment
    does not make the cache miss itself;
  - the flag is included in `proofAuthorityMode` as
    `scoped-source-proof-serve=1`, so it uses a distinct proof namespace;
  - key construction uses cheap `git rev-parse HEAD`;
  - accepted-result sidecars may return only after the committed Git guard and
    direct scoped-source current-worktree proof both accept;
  - the final tracked-only source recheck is skipped only when that scoped proof
    already accepted.
- Added `scopedSourceServe*` counters separate from the shadow counters.
- Caveat: this is still experimental authority. It demonstrates the performance
  architecture, but before hardening it still needs explicit scoped
  source-identity semantics and construction-dep discharge by identity/subset,
  not counts alone.

Serving benchmark:

- Build:
  `/nix/store/h307pr35vak5s60z739gf827594yfrfc-nix-2.35.0pre20260515_dirty`.
- `cold-stats/162`: mean `5.1680132461886386s`, min
  `0.3683926700032316s`, max `18.42737281898735s`; outputs matched
  reference.
- `hot-stats/162`: mean `0.21597377969883383s`, min
  `0.16721916099777445s`, max `0.278044267965015s`; outputs matched
  reference.
- Hot counters:
  - proof load attempts/hits/misses `10/10/0`;
  - accepted-result sidecar hits `10/10`;
  - exact/cross sidecar hits `3/7`;
  - `serveJsonBuildKeysUs` `11039us`;
  - accepted-result sidecar total `1619465us`;
  - accepted-result sidecar read `111382us`;
  - accepted-result sidecar guard `135077us`;
  - observed-key verify `34121us`;
  - `serveSourceRecheckUs` `0`;
  - scoped-source serve attempts/accepted/rejected/unsupported `10/10/0/0`;
  - `scopedSourceServeUs` `1304526us`;
  - tracked diff `260648us`;
  - tracked list `270989us`;
  - directory checks `10420`;
  - path checks `68700`;
  - recursive-directory unsupported `0`;
  - untracked/ignored rejects `0`.
- Wall-only run:
  - `cold/163`: mean `4.466697397100506s`, min
    `0.33864588901633397s`, max `16.02140025299741s`; outputs matched
    reference.
  - `hot/163`: mean `0.23669341380591505s`, min
    `0.16908456501550972s`, max `0.28818201704416424s`; outputs matched
    reference.
  - Sidecars: `10` accepted-result files, total `25696` bytes, format
    `nix.eval-trace.json-action-proof.accepted-result.v5`, authority mode
    includes `scoped-source-proof-serve=1`.

Causal read:

- This is the first non-oracle implementation that exercises the proposed
  architecture directly. It beats the regenerated first-ten baseline by a wide
  margin:
  - baseline `hot/0`: `1.156354553392s`;
  - scoped-source serving `hot/163`: `0.23669341380591505s`;
  - baseline `cold/0`: `6.643639611092s`;
  - scoped-source serving `cold/163`: `4.466697397100506s`.
- It also beats the earlier no-stats v5 sidecar result:
  - `hot/152`: `0.4079825976979919s`;
  - `hot/163`: `0.23669341380591505s`.
- The remaining hot cost is now the scoped source proof itself, not key
  construction. The largest measured pieces are tracked dirty checking,
  `git ls-files`, and the direct directory/path checks.

Dirty-source adversarial probe:

- First tried a hand-run cache probe with `NIX_ALLOW_EVAL=0`, but it missed the
  cache before source proof because the manual process environment did not match
  the harness impure-env scope closely enough. That result is not a semantic
  proof result.
- Then tried a one-commit `--hot-from cold-stats/162` harness probe. The harness
  correctly rejected it before eval because the commit list did not match the
  source run and, when `NIX_ALLOW_EVAL=0` was set, the planned manifest differed.
- Final probe used the full first-ten commit list and a temporary
  `/home/connorbaker/nixpkgs/.git/hooks/post-checkout` hook that creates an
  untracked root child after the harness source-provenance check and after each
  commit checkout. The hook and probe file were removed and nixpkgs was restored
  to `master`.
- `hot-stats/167` dirty probe:
  - status `0` because fallback evaluation was allowed;
  - mean `14.231949473090935s`, indicating full fallback work;
  - proof load hits/misses `0/10`;
  - scoped-source serve attempts/accepted/rejected/unsupported `27/0/27/0`;
  - untracked/ignored rejects `27`;
  - trace session time `9965839us`.

Causal read:

- The scoped-source serving gate fails closed for an untracked direct child in a
  guarded directory. Because this workload observes the repository root
  directory, an untracked root child invalidates all ten hot sidecar serves and
  forces evaluation.
- This is conservative but sound for the observed facts. It is also a useful
  warning for ergonomics: any untracked direct child in an observed directory can
  disable serving, even if it is not relevant to the user's intended attr.

Next read:

- Keep scoped-source serving as the main performance direction.
- Next implementation refinements should target the proof cost:
  - avoid repeated direct path checks by deduplicating exact/recursive/current
    path probes after guard parsing;
  - reuse one tracked-tree/current-source object across guard, observed-key, and
    final source checks inside a process;
  - consider libgit2/index traversal for tracked file/direct-child loading if
    `git ls-files` remains expensive.
- Next soundness hardening:
  - encode scoped source identity explicitly instead of relying on the old
    whole-clean source identity guard semantics;
  - replace count-based construction-dep discharge with a digest/set or
    per-dep membership proof against output-root derivation authority;
  - add tests for tracked dirty guarded files, untracked direct children,
    ignored direct children, untracked exact paths, recursive directory guards,
    and store-dependency fallback warnings.

## 2026-05-25: Scoped source serving hardening pass

Adversarial review findings:

- The scoped-source serving namespace was distinct, but the source identity
  guard still looked like a current whole-clean repo identity. That was
  semantically misleading once serving intentionally allows dirt outside the
  guarded source facts.
- The accepted-result sidecar ran the scoped current-worktree proof before
  observed-key, store-path, output-root, and residual-dep checks. That was safe
  in the single-process benchmark but widened the stale-worktree race window.
- `proofScopedSourceCurrentDirectoryChildren()` skipped every `.git` entry even
  though `builtins.readDir /home/connorbaker/nixpkgs` reports `.git` as a
  visible `"directory"` entry. This is a real caveat for directory-listing
  semantics. The immediate hardening is to keep the exception only for the root
  repository metadata entry; nested `.git` children now reject as untracked.
- Construction-dep discharge is still count-based and remains the highest-risk
  sidecar authority debt outside the source proof itself.

Implementation changes:

- Added explicit scoped source identity metadata to newly written source certs:
  - `sourceIdentityProofScope = "scoped-source-current-worktree-v1"`;
  - `sourceIdentityBase = { kind = "clean-git-revision-v1", repoRoot, headRev }`;
  - source identity guard role
    `file-eval-root-source-identity-base-clean-revision`.
- `proofJsonActionSourceIdentityGuardsAccept()` now requires those scoped fields
  and the scoped role in scoped-serving mode, and rejects them outside scoped
  mode. This makes the cert say "the base proof was captured from a clean
  revision; current serving authority comes from the accepted Git guard plus the
  scoped current-worktree proof" instead of pretending the dirty worktree is
  globally clean.
- Moved the accepted-result sidecar scoped current-worktree proof to the final
  source-authority check immediately before returning the cached output.
- Restricted the `.git` skip to the root metadata child only. This preserves the
  current benchmark path but records the remaining caveat: root `.git` is still
  treated as repository metadata, so a general proof needs a guard v5 fact for
  expression-visible metadata directory entries or must fail closed for root
  directory listings.
- Added a small recursive-path fast path: when `git ls-files` proves a recursive
  guard target is a tracked file and `diff-index` is clean, avoid a filesystem
  `symlink_status()` just to discover that it is not a directory.

Build:

- `/nix/store/60hi1n45q9h9fzsv8prsj3h3gcn9b9dj-nix-2.35.0pre20260515_dirty`.

Benchmark after explicit `sourceIdentityProofScope` but before the hardening
patch:

- `cold-stats/168`: mean `4.9522802429972215s`, min
  `0.3129485209938139s`, max `17.90108123299433s`; outputs matched
  reference.
- `hot-stats/168`: mean `0.20150982020422817s`, min
  `0.1755770809832029s`, max `0.2457194559974596s`; outputs matched
  reference.
- Hot counters:
  - attempts/hits/misses `10/10/0`;
  - accepted-result sidecar hits `10/10`;
  - exact/cross sidecar hits `3/7`;
  - `serveJsonBuildKeysUs` `11687us`;
  - scoped-source serve attempts/accepted/rejected/unsupported `10/10/0/0`;
  - `scopedSourceServeUs` `1163657us`;
  - tracked diff `259536us`;
  - tracked list `208759us`;
  - directory checks `10420`;
  - path checks `68700`;
  - recursive-directory unsupported `0`;
  - untracked/ignored rejects `0`.

Benchmark after scoped source identity role/base, final revalidation ordering,
root `.git` tightening, and recursive tracked-file fast path:

- `cold-stats/169`: mean `5.023010326176882s`, min
  `0.29067007097182795s`, max `17.89004740898963s`; outputs matched
  reference.
- `hot-stats/169`: mean `0.23575802170089447s`, min
  `0.18288003304041922s`, max `0.29701585398288444s`; outputs matched
  reference.
- Hot counters:
  - attempts/hits/misses `10/10/0`;
  - accepted-result sidecar hits `10/10`;
  - exact/cross sidecar hits `3/7`;
  - `serveJsonBuildKeysUs` `10652us`;
  - accepted-result sidecar total `1897452us`;
  - accepted-result sidecar read `176525us`;
  - accepted-result sidecar guard `106315us`;
  - `serveSourceRecheckUs` `0`;
  - scoped-source serve attempts/accepted/rejected/unsupported `10/10/0/0`;
  - `scopedSourceServeUs` `1526416us`;
  - tracked diff `245546us`;
  - tracked list `337048us`;
  - directory checks `10420`;
  - path checks `68700`;
  - recursive-directory unsupported `0`;
  - untracked/ignored rejects `0`.
- Spot-checked a run `169` source cert: it contains
  `sourceIdentityProofScope`, `sourceIdentityBase`, and the new
  `file-eval-root-source-identity-base-clean-revision` role. Spot-checked a
  v5 accepted-result sidecar: it still points at the source cert by size/hash
  and retains authority mode `scoped-source-proof-serve=1`.

Causal read:

- The semantic hardening preserved the hit path and still beats the regenerated
  baseline by a wide margin:
  - baseline `hot/0`: `1.156354553392s`;
  - hardened scoped serving `hot-stats/169`: `0.23575802170089447s`;
  - baseline `cold/0`: `6.643639611092s`;
  - hardened scoped serving `cold-stats/169`: `5.023010326176882s`.
- The recursive tracked-file fast path was not measurably causal in this run.
  The path-check count is still `68700`, and `scopedSourceServeUs` moved in the
  wrong direction relative to run `168`. Treat the fast path as a small
  correctness-preserving cleanup, not a demonstrated optimization.
- The remaining hot cost is still source proof reconstruction: `git ls-files`,
  direct directory scans, and exact/recursive path checks. The next performance
  step should reduce the amount of current/tracked tree reconstructed for each
  serve, not keep shaving individual stat calls.

Next read:

- Do not spend more time on micro-optimizing `symlink_status()` without a new
  timer proving it is material.
- Prefer a guard/proof-format change that records enough tracked directory/path
  facts at write time to avoid `git ls-files` on serve, while still scanning the
  current filesystem for untracked/ignored direct children in guarded
  directories.
- If keeping `git ls-files` for one more iteration, build only a
  projection-specific tracked summary from the parsed guard instead of a full
  tracked tree for the repository.
- Soundness debts still open:
  - root `.git` directory-listing semantics need a first-class proof or a
    conservative fallback;
  - construction-dep discharge must move from counts to identity/subset proof;
  - tests are still needed for tracked dirty guarded files, untracked/ignored
    direct children, untracked exact paths, recursive directory guards,
    `NIX_ALLOW_EVAL=0` rejection on proof failure, and missing store-dependency
    warning/fallback behavior.

## 2026-05-25: Projection-specific tracked summary ablation

Hypothesis:

- `proofScopedSourceLoadTrackedTree()` was building a full tracked-file and
  directory-child map from `git ls-files -z`. If the guard is parsed first, the
  serve path should only need:
  - direct children for guarded directory listings;
  - existence/kind facts for exact, structural-exact, and recursive guard
    targets.
- Expected win: reduce allocation and set insertion inside
  `scopedSourceServeTrackedListUs` while keeping the same proof model.

Implementation tried:

- Changed the tracked tree builder to take `ProofGitGuardPaths`.
- Retained only files in the wanted exact/structural/recursive/path set.
- Retained directory markers only for wanted paths.
- Retained directory children only for guarded directory-listing paths.

Benchmark:

- Build:
  `/nix/store/6sj74dw2vf6qx7b935yg9dcjwr9zjjvh-nix-2.35.0pre20260515_dirty`.
- `cold-stats/170`: mean `5.173719628108666s`, min
  `0.34797488496406004s`, max `18.62251766899135s`; outputs matched
  reference.
- `hot-stats/170`: mean `0.2409979184914846s`, min
  `0.16792559798341244s`, max `0.28859420598018914s`; outputs matched
  reference.
- Hot counters:
  - attempts/hits/misses `10/10/0`;
  - accepted-result sidecar hits `10/10`;
  - exact/cross sidecar hits `3/7`;
  - `serveJsonBuildKeysUs` `10206us`;
  - accepted-result sidecar total `1928398us`;
  - accepted-result sidecar read `310595us`;
  - accepted-result sidecar guard `106028us`;
  - scoped-source serve attempts/accepted/rejected/unsupported `10/10/0/0`;
  - `scopedSourceServeUs` `1426225us`;
  - tracked diff `251193us`;
  - tracked list `471080us`;
  - directory checks `10420`;
  - path checks `68700`;
  - recursive-directory unsupported `0`;
  - untracked/ignored rejects `0`.

Causal read:

- This implementation did not improve the hot path. It preserved correctness
  and hits, but `trackedListUs` moved from `337048us` in run `169` to
  `471080us` in run `170`. The added wanted-set membership checks during
  `git ls-files` parsing outweighed any savings from retaining fewer entries.
- The result falsifies this specific implementation, not the broader idea of a
  compact tracked proof. A useful version likely needs either:
  - tracked directory/path facts written into the guard at capture time; or
  - a cheaper data structure/trie for filtering while parsing `git ls-files`;
  - preferably both measured with dedicated counters for retained files,
    retained directories, and parse/filter time.

Decision:

- Revert or replace this ablation before continuing performance work. Do not
  treat projection-specific filtering during `git ls-files` parsing as a win
  without a new benchmark showing `trackedListUs` and wall time moving down.

## 2026-05-25: Adversarial hardening and parsed-guard reuse

Adversarial findings:

- The non-scoped `canServeCurrentHead()` path memoized the clean-head result
  inside one `tryLoadProofJsonActionCache()` call. That could allow a later
  candidate in the same lookup to reuse a stale `true` after the worktree
  changed. Scoped serving already re-ran the scoped proof per candidate, but
  the shared helper still carried the stale memo for the older path.
- The recursive tracked-file fast path could treat a submodule gitlink root as
  a normal tracked file. A recursive source guard rooted at a submodule can be
  invalidated by changes inside the submodule without a superproject HEAD
  change, so it must fall back rather than direct-serve.

Implementation changes:

- Removed the clean-head memo from `canServeCurrentHead()`. Non-scoped serving
  now recomputes the current clean-head check for each successful candidate.
- Added a second current-source check after full-certificate accepted-result
  sidecar writes and immediately before returning a full-cert cache hit. This
  narrows the race window on the non-sidecar path.
- Tried to harden the recursive tracked-file fast path by parsing
  `git ls-files --stage -z` and rejecting mode `160000` gitlinks, but that was
  too expensive.
- Reverted the recursive tracked-file fast path entirely. Recursive paths now
  go through `symlink_status()` again; a submodule root is a directory and
  therefore falls back through the existing recursive-directory unsupported
  path.
- Added parsed-guard reuse in the accepted-result sidecar path:
  - parse the guard once into `ProofGitGuardPaths`;
  - reuse it for base revision validation, guard acceptance, source-identity
    command-file coverage, and final scoped-source proof;
  - keep existing byte/hash guard ref validation unchanged.

Benchmark for `git ls-files --stage -z` hardening attempt:

- Build:
  `/nix/store/gpygwhbim4zd57f3plg4r2finkq4kd4l-nix-2.35.0pre20260515_dirty`.
- `cold-stats/171`: mean `5.137672630912857s`, min
  `0.44284861703636125s`, max `18.366625480004586s`; outputs matched
  reference.
- `hot-stats/171`: mean `0.25074958019540644s`, min
  `0.19164519297191873s`, max `0.2950478430138901s`; outputs matched
  reference.
- Hot counters:
  - attempts/hits/misses `10/10/0`;
  - accepted-result sidecar hits `10/10`;
  - `acceptedResultSidecarTotalUs` `2057768us`;
  - `acceptedResultSidecarReadUs` `134597us`;
  - `acceptedResultSidecarGuardUs` `117795us`;
  - `scopedSourceServeUs` `1656693us`;
  - tracked diff `250969us`;
  - tracked list `634616us`;
  - directory checks `10420`;
  - path checks `68700`.

Causal read:

- `git ls-files --stage -z` is too expensive as a blanket hardening mechanism.
  It doubled the tracked-list bucket relative to the best recent runs. Keep the
  submodule safety by removing the fast path instead of requesting file modes
  for every tracked path.

Benchmark after reverting the fast path and adding parsed-guard reuse:

- Build:
  `/nix/store/q03rzj9byn3c24b7zm0a0xig38q2yi93-nix-2.35.0pre20260515_dirty`.
- `cold-stats/172`: mean `5.079508310992969s`, min
  `0.43592308700317517s`, max `18.081171298981644s`; outputs matched
  reference.
- `hot-stats/172`: mean `0.21376937840832397s`, min
  `0.1557871929835528s`, max `0.279903280956205s`; outputs matched
  reference.
- Hot counters:
  - attempts/hits/misses `10/10/0`;
  - accepted-result sidecar hits `10/10`;
  - exact/cross sidecar hits `3/7`;
  - `serveJsonBuildKeysUs` `11097us`;
  - accepted-result sidecar total `1691881us`;
  - accepted-result sidecar read `324925us`;
  - accepted-result sidecar guard `93497us`;
  - `serveSourceRecheckUs` `0`;
  - scoped-source serve attempts/accepted/rejected/unsupported `10/10/0/0`;
  - `scopedSourceServeUs` `1137328us`;
  - tracked diff `255548us`;
  - tracked list `269166us`;
  - directory checks `10420`;
  - path checks `68700`;
  - recursive-directory unsupported `0`;
  - untracked/ignored rejects `0`.

Causal read:

- Parsed-guard reuse is a small but real local win. It does not remove the main
  proof work, but it brought the hardened path back from the `--stage`
  regression and is close to the earlier best stats run.
- The remaining dominant costs are:
  - scoped source proof reconstruction (`1137328us/10`);
  - source/sidecar reads and parsing (`acceptedResultSidecarReadUs` is noisy but
    still hundreds of milliseconds per ten in run `172`);
  - directory/path checks with no dedicated timing split yet.
- The next performance change should either avoid reading/parsing the large
  source cert on accepted-result hits, or change the guard/source proof format
  so the tracked directory/path facts needed for serving are recorded compactly
  at write time.

## 2026-05-25: Scoped-source attribution and sidecar soundness fix

Attribution counters:

- Added scoped-source timing splits for live directory scans and path stats.
- Build:
  `/nix/store/jxmhi99mvc652h6wkzzkgya951fhima7-nix-2.35.0pre20260515_dirty`.
- `cold-stats/173`: mean `5.279956660902826s`, min
  `0.5021701849764213s`, max `18.52752404101193s`; outputs matched
  reference.
- `hot-stats/173`: mean `0.26645940050366335s`, min
  `0.21346399001777172s`, max `0.30447787302546203s`; outputs matched
  reference.
- Hot counters:
  - accepted-result sidecar hits `10/10`, exact/cross `3/7`;
  - `acceptedResultSidecarTotalUs` `2088175us`;
  - `scopedSourceServeUs` `1350494us`;
  - tracked diff `252765us`;
  - tracked list `266751us`;
  - directory scan `153449us`;
  - path stat `28345us`;
  - directory/path checks `10420/68700`.

Causal read:

- The new timers perturb the wall-time result, but the split is still useful:
  live full directory scans are a measurable part of scoped-source proof, while
  path existence stats are small. The larger hidden cost is still reconstructing
  the tracked tree and walking the large guard sets.

Observed-directory scoped-source shortcut:

- The v4 guard already distinguishes relaxed by-name directory listings from
  complete directory keysets by recording `observedDirectoryChildren`. For those
  relaxed directories, serving does not need to rescan the whole current
  directory. It only needs to ensure any observed child that was absent from the
  tracked tree has not appeared as an untracked/ignored current child.
- Implemented `proofScopedSourceDirectoryListingsAccept()` so directories with
  an observed-child scope skip the full `directory_iterator` scan and do exact
  child existence checks for only the observed children missing from the tracked
  tree.

Benchmark:

- Build:
  `/nix/store/6wm3y18v4v4xffcyqp0fs4xidwp9si2f-nix-2.35.0pre20260515_dirty`.
- `cold-stats/174`: mean `5.195091181993485s`, min
  `0.4230112489894964s`, max `18.67641846596962s`; outputs matched
  reference.
- `hot-stats/174`: mean `0.2848967663012445s`, min
  `0.1674540030071512s`, max `0.3321394430240616s`; outputs matched
  reference.
- Hot counters:
  - accepted-result sidecar hits `10/10`, exact/cross `3/7`;
  - `acceptedResultSidecarTotalUs` `1376990us`;
  - `scopedSourceServeUs` `1028823us`;
  - tracked diff `244575us`;
  - tracked list `213144us`;
  - directory scan `35155us`;
  - path stat `28630us`;
  - directory/path checks `10420/68700`.
- Wall-only run:
  - `cold/175`: mean `4.394020770391217s`, min
    `0.4370305300108157s`, max `15.752701206016354s`;
  - `hot/175`: mean `0.22263384429970756s`, min
    `0.14878889499232173s`, max `0.28009857097640634s`;
  - outputs matched reference.

Causal read:

- The shortcut moved the intended bucket: directory scan time dropped from
  `153449us/10` in run `173` to `35155us/10` in run `174`.
- The wall-only hot run improved from the earlier scoped-source wall-only
  `hot/163` mean `0.23669341380591505s` to `0.22263384429970756s`.
- This is a keeper, but it is not the main remaining lever. The proof still
  parses/builds a full tracked tree and still checks tens of thousands of guard
  paths.

Adversarial review:

- The accepted-result sidecar path had a real soundness flaw: output-root proof
  was allowed to discharge both `derivationOutputIdentity` and
  `derivedStorePath` deps. That is too broad. Output-root proof can justify
  skipping root-output derivation identity checks, but it does not recompute the
  copied-source store path observation. `DerivedStorePath` must remain a current
  equality check unless a separate source-copy witness replaces it.
- Scoped-source serving also had a tracked-file TOCTOU: it checked HEAD/clean
  tracked state before scanning live filesystem facts, but did not re-check
  after the scans.

Fixes:

- `proofJsonActionAcceptedResultResidualDepsAccept()` now always recomputes
  retained `derivedStorePath` deps. The output-root discharge only skips
  `derivationOutputIdentity` verification when its sidecar authority and counts
  match.
- `proofJsonActionAcceptedResultResidualDepsJson()` now keeps
  `derivedStorePath` deps in residual sidecar-style dep sets and only omits
  `derivationOutputIdentity`.
- Scoped-source serving now performs the clean HEAD/tracked check after the live
  filesystem proof, immediately before accepting. I first tried pre-and-post
  checks, then removed the pre-check as redundant: the post-check is what
  narrows the tracked-file race, while the pre-check only doubles the Git diff
  cost. This does not fully solve untracked-file TOCTOU after a directory has
  already been scanned; a final design needs either locking, a snapshot source
  accessor, or a stronger filesystem proof.

Benchmark with derived-store-path recomputation plus pre/post clean checks:

- Build:
  `/nix/store/h2mw0vwcxcy634zwhj5lcw8bjwb6l9ds-nix-2.35.0pre20260515_dirty`.
- `cold-stats/176`: mean `5.244190303504001s`, min
  `0.44789898704038933s`, max `18.266501207021065s`; outputs matched
  reference.
- `hot-stats/176`: mean `0.40030678180628454s`, min
  `0.3500219549750909s`, max `0.5233074610005133s`; outputs matched
  reference.
- Hot counters:
  - accepted-result sidecar hits `10/10`;
  - `acceptedResultSidecarTotalUs` `2920417us`;
  - residual checks `775354us`;
  - `scopedSourceServeUs` `1551619us`;
  - tracked diff `448728us`;
  - tracked list `482582us`;
  - directory scan `35065us`.

Benchmark after keeping only the final clean check:

- Build:
  `/nix/store/89rg8r36v5wlnpag1kpj1nwkaqs9xbv9-nix-2.35.0pre20260515_dirty`.
- `cold-stats/177`: mean `5.158294227888109s`, min
  `0.4491128739900887s`, max `18.41916917701019s`; outputs matched
  reference.
- `hot-stats/177`: mean `0.36163309251423925s`, min
  `0.31760424200911075s`, max `0.43139415903715417s`; outputs matched
  reference.
- Hot counters:
  - accepted-result sidecar hits `10/10`, exact/cross `3/7`;
  - `acceptedResultSidecarTotalUs` `2598178us`;
  - residual checks `778525us`;
  - `scopedSourceServeUs` `1287852us`;
  - tracked diff `201102us`;
  - tracked list `389218us`;
  - directory scan `34746us`;
  - path stat `28232us`.
- Wall-only run:
  - `cold/178`: mean `4.368758312799036s`, min
    `0.4454426739830524s`, max `15.567181318998337s`;
  - `hot/178`: mean `0.3020515154872555s`, min
    `0.24904480995610356s`, max `0.3681607689941302s`;
  - outputs matched reference.

Causal read:

- The unsafe/count-only construction discharge was hiding roughly
  `0.08s` hot wall time and about `0.78s/10` in stats-mode residual checks.
  The honest hot number is therefore closer to `0.30s` wall-only, not the
  earlier `0.22s` after the observed-directory shortcut.
- Even after the soundness fix, the prototype still beats the regenerated
  first-10 baseline (`hot/0` mean `1.156354553392s`, `cold/0` mean
  `6.643639611092s`) and the historical `hot/938` target around `0.88s`.
- The next real performance leads are now:
  - replace broad retained `DerivedStorePath` recomputation with a compact,
    sound source-copy witness;
  - avoid rebuilding the full tracked tree for every sidecar hit;
  - move more source-cert authority into a compact self-contained sidecar only
    after the construction-dep semantics are explicit, not count-based.

Follow-up scoped-source tree probes:

- Tested a shell approximation of `git ls-files -z -- <guard pathspecs...>` on a
  representative guard. It listed `53897` paths and took about `1.15s` for one
  command. This is far worse than the current full `git ls-files -z` subprocess
  bucket, so pathspec-filtering in Git is not a promising implementation here.
- Tried replacing the scoped-source tracked tree's ordered `std::set`/`std::map`
  with `std::unordered_set`/`std::unordered_map` and reserving the file table.
  This is semantically neutral because the tracked tree is only used for
  membership checks.
- Build:
  `/nix/store/grx7sm4qg3m44aq4gv36sil4v4n00inh-nix-2.35.0pre20260515_dirty`.
- `cold-stats/179`: mean `5.099534729606239s`, min
  `0.4709482499747537s`, max `17.96816728700651s`; outputs matched
  reference.
- `hot-stats/179`: mean `0.3496441771043465s`, min
  `0.2893790160305798s`, max `0.4333734639803879s`; outputs matched
  reference.
- Hot counters:
  - accepted-result sidecar hits `10/10`;
  - `acceptedResultSidecarTotalUs` `2513742us`;
  - residual checks `847749us`;
  - `scopedSourceServeUs` `1219506us`;
  - tracked diff `210163us`;
  - tracked list `300101us`;
  - directory scan `34857us`;
  - path stat `28444us`.
- Wall-only repeats:
  - `cold/180`: mean `4.4080236841924485s`; `hot/180`: mean
    `0.37045718339504674s`; outputs matched.
  - `cold/181`: mean `4.504592362511903s`; `hot/181`: mean
    `0.33812118250643836s`; outputs matched.

Causal read:

- Stats-mode improved slightly versus run `177` (`hot-stats` `0.3616s` to
  `0.3496s`, `scopedSourceServeUs` `1287852us/10` to `1219506us/10`), but
  wall-only repeats were mixed and slower than the single pre-hash run `178`
  (`0.3021s`).
- Treat this as a low-risk but not yet proven micro-optimization. It does not
  change the strategic direction: the dominant honest costs are retained
  `DerivedStorePath` recomputation and repeated source-proof reconstruction.

### 2026-05-25 derived source-guard skip and scoped-source follow-up

Hypothesis:

- Most retained `derivedStorePath` deps in the accepted-result sidecar are
  source-copy observations of regular Nix files. If the accepted git guard
  already binds that exact regular file, and scoped-source serving proves the
  current worktree still matches the accepted source facts, then replaying
  `observeDerivedStorePath` for those regular-file deps is redundant.
- Directory source copies remain rechecked. The skip is only for current
  regular non-symlink files covered by exact/structural-exact accepted guard
  paths.

Changes kept:

- Added exact git guard coverage for regular-file `DerivedStorePath` deps at
  publication time while still retaining the deps in the source certificate.
- During accepted-result residual checking, skip replay for retained
  `derivedStorePath` deps only when the current path is a regular non-symlink
  file and the accepted guard covers it exactly.
- Added counters for derived-store-path deps seen, rechecked, and skipped by
  source guard.
- Bound the output-root construction-dep discharge to canonical multiset
  digests for both `derivedStorePath` and `derivationOutputIdentity`, not just
  counts.
- Tightened observed-directory scoped-source serving after adversarial review:
  observed directories now scan current child names and reject any untracked
  child, instead of checking only the observed missing children.
- Accepted-result sidecar write/load now requires scoped-source serving to be
  enabled. The fallback clean-head check only covers tracked changes and is not
  sufficient authority for this prototype.

Benchmarks:

- Build before observed-directory repair:
  `/nix/store/ls14rda8sgv5yxf37h952qbhbi9clhdh-nix-2.35.0pre20260515_dirty`.
- `cold-stats/182`: mean `5.175206798000727s`, min
  `0.4148079570150003s`, max `18.199514368956443s`; outputs matched.
- `hot-stats/182`: mean `0.24659598020371049s`, min
  `0.16877497802488506s`, max `0.3237102520070039s`; outputs matched.
- Hot counters: accepted-result hits `10/10`, exact/cross `3/7`,
  `acceptedResultSidecarResidualUs` `65762us`,
  `derivedStorePathDeps` `0` in the old replay counter,
  `scopedSourceServeUs` `955332us`.

- Build after observed-directory repair:
  `/nix/store/7b6qpannnqqpkb6m8njhjziwvihqagxw-nix-2.35.0pre20260515_dirty`.
- `cold-stats/184`: mean `5.200240925687831s`, min
  `0.4813238980132155s`, max `18.336686438997276s`; outputs matched.
- `hot-stats/184`: mean `0.2553404361882713s`, min
  `0.18811846297467127s`, max `0.3243320269975811s`; outputs matched.
- Hot counters: residual `60984us`, scoped-source serve `1015950us`,
  directory scan `152437us`. The soundness repair costs roughly `0.009s` hot
  mean on this run and keeps the derived replay win.

- Build after counters and construction-dep digest binding:
  `/nix/store/6kb8gll9wvpvrvj4zi2h8gr2iks3m4y7-nix-2.35.0pre20260515_dirty`.
- `cold-stats/185`: mean `5.193586918601068s`, min
  `0.45975542603991926s`, max `18.260764887963887s`; outputs matched.
- `hot-stats/185`: mean `0.26372476388933136s`, min
  `0.1853223229991272s`, max `0.3252680109580979s`; outputs matched.
- Hot counters: `acceptedResultSidecarDerivedStorePathDeps` `8460`,
  `acceptedResultSidecarDerivedStorePathRechecked` `80`,
  `acceptedResultSidecarDerivedStorePathSourceGuardSkipped` `8380`,
  residual `217535us`, scoped-source serve `927496us`, tracked list
  `165191us`, directory scan `152020us`.
- Cleaned keeper build after removing the direct-index prototype:
  `/nix/store/f9f5jgvg9vm5my8ala0ipdb9r0wl3l0p-nix-2.35.0pre20260515_dirty`.
- `cold-stats/188`: mean `5.200051576911937s`, min
  `0.5024071160005406s`, max `18.348219394043554s`; outputs matched.
- `hot-stats/188`: mean `0.2904337971005589s`, min
  `0.24710221902932972s`, max `0.3286985190352425s`; outputs matched.
  Stats-mode residual was noisy (`369040us/10`), but derived skip counts
  stayed stable (`8380` skipped, `80` rechecked).
- No-stats wall run on the same cleaned keeper build:
  `cold/189` mean `4.463150830101222s`, min `0.4274631930165924s`,
  max `16.03124696901068s`; `hot/189` mean `0.2467406668991316s`,
  min `0.18201049597701058s`, max `0.319602242030669s`; outputs matched.
- Build after switching construction-dep digest material from whole JSON object
  dumps to canonical length-framed selected fields:
  `/nix/store/rqwylzbpbvg6x47ar3v8xwhval4frp8p-nix-2.35.0pre20260515_dirty`.
- `cold-stats/190`: mean `5.2262070597033015s`, min
  `0.5114534639869817s`, max `18.276933469984215s`; outputs matched.
- `hot-stats/190`: mean `0.26061929350253193s`, min
  `0.18028929503634572s`, max `0.3516702919732779s`; outputs matched.
- Hot counters after the digest material change: residual `271731us`, scoped
  source serve `1002150us`, tracked list `196097us`, directory scan
  `150728us`, derived deps skipped/rechecked still `8380/80`.

Causal read:

- The derived regular-file source-guard skip is a real win: it skips `838/846`
  derived-store-path deps per hit and leaves only the `8` directory source-copy
  deps to replay. This directly explains the drop from run `177` residual
  `778525us/10` to run `184` residual `60984us/10` before digest accounting.
- The remaining hot floor is now source-proof reconstruction and sidecar
  overhead, not derivation output recomputation. The scoped-source proof still
  performs a tracked listing/diff, scans `1042` guarded directories per hot hit
  set, and checks `7704` exact/structural paths per hit.
- The canonical construction-dep digest change is semantically cleaner than
  hashing entire JSON object dumps and modestly improves residual timing versus
  the noisy cleaned run (`369040us/10` to `271731us/10`), but residual timing is
  still noisy enough that this should be treated as a cleanup, not a major
  performance lever.
- The root `.git` exception in scoped-source directory scanning remains a
  prototype assumption. The representative guard contains `.` as a directory
  listing, and serving would reject if root `.git` were treated as a normal
  untracked child. This is acceptable only under a source-access model where
  repo metadata is not expression-visible; production authority needs an
  explicit invariant or a different source snapshot.
- The remaining TOCTOU is also prototype-only: live filesystem checks, git
  diff, and sidecar reads are not atomic. A final design needs a source
  snapshot, lock, or equivalent race fence.

Rejected follow-up:

- Full-repo untracked checks (`git ls-files --others --ignored --exclude-standard`)
  took about `0.27s` per check on `/home/connorbaker/nixpkgs`, so they are too
  expensive as a replacement for scoped directory checks.
- Direct `.git/index` parsing was tried twice. A full tracked-tree reader
  (`hot-stats/186`) was correct but worsened tracked-list time
  (`354034us/10`) and hot mean (`0.2746116195048671s`). A narrowed
  guard-targeted index reader (`hot-stats/187`) was also correct, reduced
  scoped-source total to `734824us/10`, but did not improve wall time
  (`0.2692641686007846s`) and added Git-index semantic risk. Removed the
  index-reader prototype.

Next leads:

- The promising next implementation is a compact accepted-result/source witness
  that avoids rebuilding source proof state on every hit without widening the
  authority surface. It should bind the parsed guard/source facts used by the
  accepted-result sidecar and still require scoped-source validation.
- Revisit a faster observed-directory path only with a testable proof that the
  `observedDirectoryChildren` map never represents complete keyset semantics.
- Do not spend more time on Git pathspec filtering, full-repo untracked checks,
  or direct index parsing unless new counters contradict these runs.

### 2026-05-25 digest-v2 wall run and exact-path source authorization repair

No-stats wall check for the digest-v2 keeper:

- `cold/191`: mean `4.451372467412147s`, min `0.39543908002087846s`,
  max `15.750571513024624s`.
- `hot/191`: mean `0.2546431295981165s`, min `0.18039628805126995s`,
  max `0.30526652198750526s`.
- Outputs matched `reference` for all 10 commits.

Adversarial read while looking for the next performance lead:

- Scoped-source serving treated an exact path missing from `git ls-files` as
  acceptable when it also did not currently exist. That is not sound for exact
  source facts: a cache entry could have been recorded from an untracked source
  file, the file could later disappear, and the Git guard would not prove the
  old bytes.
- Patched `proofScopedSourceExactPathsAccept()` so exact, structural-exact, and
  recursive file paths must be present in the tracked tree. If a guarded path is
  currently untracked, serving rejects; if it is absent and untracked, serving
  falls back as unsupported. This preserves the prototype's fail-closed shape
  while keeping the hot benchmark path, where the exact paths are tracked,
  effectively unchanged.

Causal read:

- The digest-v2 no-stats wall run stayed in the same band as run `189`
  (`hot/189` `0.2467406668991316s`, `hot/191`
  `0.2546431295981165s`). Treat the digest material change as a correctness
  cleanup, not a performance lever.
- The exact-path repair is not expected to improve the clean benchmark. Its
  value is removing a false authorization path before trying more aggressive
  source-witness optimizations.

Validation:

- `cold-stats/192`: mean `5.175471267488319s`, min
  `0.44668343098601326s`, max `18.224853361025453s`; outputs matched
  reference.
- `hot-stats/192`: mean `0.2480832979141269s`, min
  `0.18907919601770118s`, max `0.30241232505068183s`; outputs matched
  reference.
- Hot counters: accepted-result sidecar hits `10/10`, exact/cross `3/7`,
  scoped-source accepted/rejected/unsupported `10/0/0`, derived deps
  skipped/rechecked `8380/80`. This confirms the exact-path hardening is
  neutral on the clean first-ten workload.

Observed-directory provenance tightening:

- Removed the empty relaxed-directory scope from
  `populateObservedByNameDirectoryScopes()`. A directory now gets
  `observedDirectoryChildren` only when a concrete observed child was recorded
  by `populateObservedByNameDirectoryChildren()`.
- Rationale: relying on absence of a complete-keyset marker is a negative
  authority signal. The prototype's positive-navigation discipline should only
  relax a directory listing when there is positive child provenance.

Validation:

- `cold-stats/193`: mean `5.177566498995293s`, min
  `0.43789923295844346s`, max `18.428840940992814s`; outputs matched
  reference.
- `hot-stats/193`: mean `0.2561465873965062s`, min
  `0.17801050696289167s`, max `0.3358115149894729s`; outputs matched
  reference.
- Hot counters: accepted-result sidecar hits `10/10`, exact/cross `3/7`,
  scoped-source accepted/rejected/unsupported `10/0/0`, derived deps
  skipped/rechecked `8380/80`, scoped-source time `961979us/10`.

Causal read:

- Empty observed-directory scopes were not needed by this benchmark. Tightening
  the provenance rule preserved cross-head hit rate and output correctness.
- This is a keeper for soundness. It does not materially change the hot floor:
  source-cert read/parse, guard replay, residual digesting, and scoped-source
  validation remain the measured costs.
- A performance reviewer independently confirmed that the next plausible lead
  is a compact source witness bound to the source cert/guard. The design must
  avoid the rejected v4 memo shape: persisted witness facts can accelerate
  replay only if the source cert or guard binds them precisely enough that they
  do not add unaudited authority.

### 2026-05-26 scoped-source v5 snapshot guard

Harness correction:

- `eval-trace-bench` runs the `result` symlink. Rebuilding with `--no-link`
  is not enough for this harness. Run `194` was invalid for the v5 snapshot
  patch because `result` still pointed at the previous binary; the cache path
  and guard header proved it was still using proof-v15/v4 artifacts.
- The valid rebuild command for these runs was:

```bash
nix build -L .#nix-cli --out-link result --print-out-paths
```

Implementation:

- Added proof-v19/v5 scoped-source guard material. Store-time proof capture now
  records the current scoped directory child sets, exact paths, structural
  exact paths, and recursive file paths already required by the source proof.
- Hot replay no longer calls the full tracked-tree listing. It validates the
  scoped snapshot plus base-to-current tracked diff and direct directory/path
  checks for the guarded scope.
- Removed the empty observed-directory relaxed scope. Only positive observed
  child provenance can relax directory-listing authority.
- Exact paths absent from the publication snapshot are treated as scoped
  missing-path facts: replay accepts only if they still do not exist, and
  rejects if they now exist.
- Directory child snapshots are adjusted with the base-to-current tracked diff
  before comparing against the current directory scan. This fixed the cold
  rejection caused by tracked additions between adjacent benchmark commits.

Validation:

- Valid v5 build after the first snapshot implementation:
  `/nix/store/hr6kjihgjp7zx44fkf2h3plihw6g2454-nix-2.35.0pre20260515_dirty`.
  `cold-stats/195` was killed after six commits because every replay attempt
  rejected or fell back; the representative v5 guard was present under
  proof-v19 and exposed `.version-suffix` as a missing exact path.
- After missing-exact handling,
  `/nix/store/5vbwyk3sgi3rm5ajqa9dardxm6729inr-nix-2.35.0pre20260515_dirty`
  produced `cold-stats/196` mean `6.395694828213891s` and
  `hot-stats/196` mean `0.11878919039154426s`; outputs matched reference.
  Hot accepted-result sidecar hits were `10/10`, scoped-source accepted
  `10/10`, and tracked-list time was `0us`, but cold still missed four
  commits. The extra cold miss was a directory snapshot comparison that did
  not account for tracked additions.
- After diff-aware directory snapshot comparison,
  `/nix/store/g0srdgrx8nzq0rs16f3pl6zlaa38jj52-nix-2.35.0pre20260515_dirty`
  produced `cold-stats/197` mean `5.011338797589997s` and
  `hot-stats/197` mean `0.15502924640313723s`; outputs matched reference.
  Cold hit/miss count was `7/3`. Hot accepted-result sidecar hits were `10/10`;
  exact/cross hits were `3/7`; derived deps skipped/rechecked were `8380/80`;
  scoped-source accepted/rejected/unsupported was `10/0/0`; tracked-list time
  remained `0us`.
- No-stats wall run on the same build: `cold/198` mean
  `4.26478167399182s`, min `0.3215685809846036s`, max
  `15.823603230004665s`; `hot/198` mean `0.15907265001442283s`, min
  `0.12704270204994828s`, max `0.20379611500538886s`. Outputs matched
  reference.

Causal read:

- The v5 snapshot guard is a real performance lever. It removes the hot
  `git ls-files` tracked-list cost while preserving scoped validation, moving
  the first-ten no-stats wall run well below both the regenerated run-0
  baseline (`cold/0` `6.643639611092s`, `hot/0` `1.156354553392s`) and the
  historical target (`cold/938` about `6.06s`, `hot/938` about `0.88s`).
- The diff-aware child augmentation is causally tied to cold recovery:
  commit `3f7b...` changed from a full-eval cold miss in run `196` to a
  subsecond hit in run `197`.
- The remaining first-ten cold misses are different classes, not the v5
  directory proof bug: the first commit has no prior cache; `fd3f...` accepts
  a guard but has no accepted-result sidecar hit; `7fb...` rejects exact source
  guards twice.

Open risks:

- This is still a prototype authority shape. Live directory scans, path stats,
  and Git diff are not atomic with each other, so a production design still
  needs a source snapshot/race fence or an explicit experimental boundary.
- The root `.git` exclusion remains a source-access-model assumption. If Nix
  expressions can observe repository metadata through the guarded source path,
  the proof needs a firmer invariant or an explicit guard for that path.
- Guard v5 is larger than v4 because it persists scoped child sets. The hot win
  says that is worthwhile for this workload, but a 100-commit run is needed to
  make sure the write/read cost does not move the broader distribution in the
  wrong direction.

### 2026-05-26 cold-tail rejected-guard classifier

Context:

- First-26 cold stats exposed a tail that the first-ten benchmark hid.
  Commits `c5cf...` and `9e30...` were proof hits, but both spent about
  eight seconds in the command action cache load path.
- The first hypothesis was that full-proof replay reparsed the large v6
  scoped-source guard in multiple places. I changed the loader to parse guard
  bytes once into `ProofGitGuardPaths`, pass that parsed guard into
  `verifyProofJsonActionCertificate()`, and reuse it in `canServeCurrentHead()`.
  I also removed dead v2-v6 guard acceptance wrappers left unused by that
  change.

Falsified hypothesis:

- `cold-stats/204` was a valid v20 run before parsed-guard reuse:
  `c5cf...` `8.798056102008559s`, `9e30...`
  `8.233320117986295s`.
- `cold-stats/205` was a valid v20 run after parsed-guard reuse:
  `c5cf...` `8.858069756999612s`, `9e30...`
  `8.326592699042521s`.
- Guard, verifier, scoped-source, store-path, and derivation-output counters
  were all subsecond in both runs. The parsed-guard optimization is still a
  cleanup, but it did not explain the eight-second tail.

Actual cause:

- Additional counters around the accepted full-certificate replay path showed
  that sidecar writing, pre/post scoped-source checks, and parsed object cleanup
  were small. For `cold-stats/206` `9e30...`, accepted sidecar write was only
  `6274us`; pre/post scoped-source checks were about `46ms` and `42ms`.
- The missing time was before acceptance, when rejected prior candidates ran
  the output-boundary recovery candidate classifier. This distinction matters:
  the output-boundary recovery attempt/hit/reject counters were zero in the
  slow runs, so the expensive part was not recovery itself. It was the
  pre-attempt rejected-guard classification path that decided whether recovery
  would even be eligible.
- The old classifier reparsed the guard and did O(changes * guarded exact
  paths) overlap checks to decide whether a rejected guard failed only on
  modified by-name package files. In the tail this consumed about eight seconds
  even when recovery did not attempt or hit.

Implementation:

- Replaced the load-time candidate classifier with
  `proofGitGuardPathsRejectedOnlyByNamePackageExactPaths()`, which consumes the
  already-parsed `ProofGitGuardPaths` and the already-loaded base-to-current
  Git diff.
- Exact-path overlap is now checked by ancestor membership plus a sorted-set
  descendant range scan. Directory-listing checks only visit guarded directory
  ancestors of the changed path and still call `directoryListingGuardRejects()`
  for the semantic listing rule. Recursive overlap uses the same ancestor/range
  pattern.
- The classifier remains narrow: it only returns a candidate when every
  rejection is explained by `M` changes to exact by-name package paths and
  there are no other hard exact, directory, or recursive overlaps. Unsupported
  cases still fall back.
- Added temporary timing counters around accepted-certificate post-verify
  replay and accepted-result sidecar writes to close the accounting gap. These
  counters should either be kept if useful or removed before hardening.

Validation:

- `cold-stats/207`, a three-commit tail reproduction after the fast classifier,
  moved `9e30...` from `8.301170638005715s` in `cold-stats/206` to
  `0.8136084690340795s`; outputs matched `206`.
- `cold-stats/208`, the full first-26 stats window, moved the cold tail from
  `cold-stats/205`:
  - `cdeeb...`: `39.82231086899992s` -> `16.81937141495291s`
  - `c5cf...`: `8.858069756999612s` -> `1.1109096849686466s`
  - `9e30...`: `8.326592699042521s` -> `0.7960310340276919s`
- Repeating the full first-26 stats window as `cold-stats/209` on the same
  build produced mean `2.7786820167675614s`, min `0.2964390949928202s`, max
  `18.07344741502311s`. Tail: `cdeeb...` `16.54766766296234s`, `c5cf...`
  `0.951029882999137s`, `9e30...` `0.7349973939708434s`. Outputs matched
  `cold-stats/208`.
- First-26 stats distribution improved from `cold-stats/205` mean
  `4.314442696269207s`, min `0.3102443150128238s`, max
  `39.82231086899992s` to `cold-stats/208` mean
  `2.8120710699966787s`, min `0.2870798539952375s`, max
  `17.881202969001606s`.
- First-26 no-stats wall run `cold/202` on the same build produced mean
  `2.4456047720338505s`, min `0.28944051096914336s`, max
  `15.559944917971734s`; the tail was `cdeeb...`
  `14.59622892999323s`, `c5cf...` `0.8465285359998234s`, `9e30...`
  `0.8064469550154172s`.
- Outputs matched between `cold-stats/208` and the pre-classifier
  `cold-stats/205`; outputs also matched between no-stats `cold/202` and
  `cold-stats/208` for the same commits.
- The hot-from-cold harness initially rejected `hot/201`, `hot/202`,
  `hot/210`, and `hot/211` because documentation-only dirty checkout changes
  changed the recorded benchmark-subject provenance even though the linked Nix
  binary and store path were identical. I patched the compatibility check to
  compare result-wrapper `benchmarkSubject` values by `nixBinSha256` and
  `nixStorePath`.
- After that harness fix, `hot/212` from `cold/202` completed: mean
  `2.2512944387227227s`, min `0.27435762499226257s`, max
  `13.508430355985183s`; tail `cdeeb...` `13.508430355985183s`,
  `c5cf...` `1.2374936830019578s`, `9e30...` `0.7452051150030456s`.
  Outputs matched `cold/202`. This is not yet the all-cached hot floor because
  the same four first-26 commits still fall through to full-ish work.

Causal read:

- The improvement is causally tied to the classifier: after the change,
  `9e30...` load `totalUs` fell to about `702ms` in the tail reproduction and
  `771ms` in the full first-26 window, with the same candidate count and guard
  rejection count. The expensive rejected-candidate proof was eliminated.
- The parsed-guard reuse and sidecar-write cleanup were not sufficient by
  themselves. They are small supportive changes, not the main win.
- I added direct counters after this review so the next stats run can measure
  `candidateCheckUs`, directory iteration/sort time, and rejected-guard
  classifier attempts/hits/us directly instead of inferring the missing block
  from residual `load.totalUs`.

Adversarial design review:

- A design reviewer argued that output-boundary recovery should remain a
  repair/oracle mechanism, not the long-term default fast path in scoped-source
  accepted-result mode. The current fast classifier makes it affordable for
  this workload, but load-time reconstruction is still less attractive than a
  persisted, guard-bound witness.
- Recommended next design direction: write a source/candidate witness bound to
  the source cert ref, guard hash/size/base rev, proof key, authority mode, and
  scoped-source snapshot. The witness may cache guard indexes and by-name
  candidate facts, but it must not become standalone authority; replay still
  needs the scoped-source proof, residual deps, and store/root checks.
- Correctness review found no false accept in the fast classifier. The checked
  cases were root `.`, exact self/ancestor/descendant overlaps, directory
  listing semantics, recursive overlaps, and the existing by-name FileBytes
  ablation skip. One safe false negative existed: the accepted recovery branch
  called `canServeCurrentHead()` without passing the parsed guard while
  scoped-source serving requires a guard. I changed that branch to recheck with
  the same guard bytes and parsed guard paths used for the rejected-guard
  decision.

Open risks:

- Need a full 100-commit run. First-26 is now strongly positive, but the broader
  distribution may expose different rejected guard shapes or write amplification.
- Decide whether to keep the new temporary counters. They helped explain the
  issue, but the hardened version should avoid unnecessary counter noise.
- Need to investigate why `hot/212` still has four full-ish misses/fallbacks.
  The leading hypotheses are missing accepted-result sidecar authority for
  fallback-shaped commits, residual store/source checks that are still too
  heavy on sidecar hits, or a remaining proof shape that cannot be served by
  scoped-source accepted-result authority.

### 2026-05-26 paired hot-state and sidecar safety refinement

Adversarial review results:

- Performance review showed `hot/212` was valid as a hot-from-cold run, but not
  a hot-floor result. It copied the old cold state but then created a different
  proof-key namespace, so the four slow hot commits were repopulating a fresh
  proof tree.
- Harness review confirmed the result-wrapper compatibility fix is reasonable:
  hot/cold compatibility should compare `nixBinSha256` plus `nixStorePath` for
  result-wrapper builds, rather than rejecting documentation-only dirty checkout
  changes.
- Soundness review found two issues worth addressing before further benchmark
  claims:
  - aggregate proof publication could render/project output and then write a
    proof under the pre-render clean `HEAD` without a post-render clean-source
    recheck;
  - accepted-result sidecar DOI discharge was too broad unless tied to a proven
    output-root closure.

Safety changes:

- Added `proofJsonActionSourceCleanForPostRenderPublish()` and now require a
  post-render clean `HEAD` before publishing aggregate/child proof actions or
  exact-head fast memo entries from freshly rendered output. Proof-cache hits
  can still seed the exact-head memo without this extra check because their
  returned output is already proof-authorized rather than freshly rendered from
  possibly dirty source.
- Stopped treating `structuralExactPaths` as authority for skipping
  `derivedStorePath` dep recomputation. Only exact byte-guarded paths now allow
  that skip.
- First attempted to tie DOI discharge to the output-root closure. That was
  safe but bad for the hot path: `hot-stats/211` showed about `190ms` of
  `acceptedResultSidecarRootUs` on every sidecar hit.
- Replaced that with the simpler prototype rule: accepted-result sidecars no
  longer discharge DOI deps at all. They recheck DOI deps directly. In
  `hot-stats/212`, the DOI recheck cost was about `39-48ms` on the largest
  sidecar hits, while `acceptedResultSidecarRootUs` dropped to zero. This is
  safer than broad discharge and faster than closure-bound discharge. A future
  hardened design can reintroduce discharge only with an explicit compact
  output-root/closure witness.

Current build:

- `result` points to
  `/nix/store/18fvlxdhp0ld10aqww96y9fvcs12aydf-nix-2.35.0pre20260515_dirty`.
- `result/bin/nix` sha256:
  `f188b572fa2017e7bd4c74a1808df4f5491547f322f5265c3beed53512eaf22d`.

Fresh first-26 validation:

- `cold-stats/211,hot-stats/211` on the closure-bound DOI-discharge attempt:
  - cold mean `2.8148055158425658s`;
  - hot mean `0.3309321305042921s`;
  - proof namespace matched in the paired run;
  - outputs matched.
- `cold-stats/212,hot-stats/212` after disabling DOI discharge:
  - cold mean `2.875357936729695s`, min `0.3015345979947597s`, max
    `18.643207517045084s`;
  - hot mean `0.1832493622675359s`, min `0.13814756699139252s`, max
    `0.44923029700294137s`;
  - `acceptedResultSidecarRootUs` was zero on inspected hot hits;
  - outputs matched.
- `cold/213,hot/213` no-stats on the same build:
  - cold mean `2.5436017828827833s`, min `0.2992426339769736s`, max
    `16.143616278015543s`;
  - hot mean `0.21723304050437248s`, min `0.1446189489797689s`, max
    `0.5004423659993336s`;
  - cold tail remains the four full-work anchors:
    `7d761...` `16.143616278015543s`, `cdeeb...`
    `14.611651365004946s`, `7fb36...` `13.143809355970006s`,
    `fd3fb...` `12.916790268965997s`;
  - hot tail is now only `c5cf...` `0.5004423659993336s` and `cdeeb...`
    `0.4641735010081902s`;
  - cold and hot used the same proof namespace
    `05/05889c36086152748b41592c5bb733ca1595bb0d693728e28aaa0b51dc6cadcd`;
  - outputs matched.

Causal read:

- The earlier hot tail was not fundamental; it was a benchmark/state-history
  artifact from a proof-key namespace mismatch. Paired current runs reuse one
  namespace and hit accepted-result sidecars for all first-26 commits.
- Disabling DOI discharge made sidecar serving both safer and faster than the
  closure-bound attempt. The remaining hot cost is mostly scoped-source serve
  plus DOI rechecks and guard/source cert reads, not verifier replay.
- Cold is now dominated by exact-anchor/full-eval commits. The rejected-guard
  classifier win remains intact, but beating cold further requires avoiding or
  amortizing those anchor evaluations rather than optimizing proof-hit replay.

100-commit current run:

- Ran `cold/214,hot/214` no-stats on the current build with the 100-commit
  window from `/tmp/eval-trace-baseline-100-window.txt`.
- Cold distribution:
  - mean `2.568654519789852s`, min `0.2903873639879748s`, max
    `16.145585660997313s`;
  - largest spikes were `208e...` `16.145585660997313s`, `7d761...`
    `16.034344283980317s`, `cdeeb...` `14.57272544899024s`,
    `8ca334...` `14.47241536504589s`, `713e...`
    `14.418845070991665s`, `169b...` `14.412771601986606s`,
    `8417...` `13.849429525027517s`, `3451...`
    `13.786105956998654s`, `1cfd...` `13.635504678008147s`,
    `a292...` `13.574107096006628s`;
  - there is also a broad cold plateau around `1.1-1.3s` for many commits
    after the first exact-anchor sequence.
- Hot distribution:
  - mean `0.22887924861279316s`, min `0.14127800497226417s`, max
    `0.6562696620239876s`;
  - largest hot timings were `0d3e...` `0.6562696620239876s`,
    `208e...` `0.5212903930223547s`, `c5cf...`
    `0.513362332014367s`, then a cluster below `0.36s`;
  - hot stayed below the historical `0.88s` target across all 100 commits.
- Cold and hot used the same proof namespace
  `05/05889c36086152748b41592c5bb733ca1595bb0d693728e28aaa0b51dc6cadcd`.
- Outputs matched between `cold/214` and `hot/214`.

Current interpretation:

- The prototype now clearly beats the historical hot target on both 26 and 100
  commit windows.
- Cold also remains well below the historical first-10 baseline mean, but it is
  now limited by repeated exact-anchor/full-eval commits in the 100-commit
  window. Those anchors are the next cold target.
- The 1.1s cold plateau after the first large anchor sequence is a second lead:
  it is likely accepted-result/full-proof replay cost, scoped-source proof
  cost, or DOI/source-cert read cost that does not show up in the first-26
  summary. A focused stats run over commits around positions 27-40 and 70-75
  should distinguish this from exact-anchor fallback.
- Still need a regenerated no-debug/no-stats baseline from
  `92a3df1ab706cf514357ae57b367050b373ff146` for a fully fair branch-local
  comparison. The historical baseline is already beaten by a wide margin, but
  the harness review is right that a current regenerated baseline should be
  recorded before treating this as final benchmark proof.

## 2026-05-26 adversarial follow-up and cold plateau ablation

Spawned/consulted three parallel review agents from the current dirty branch:

- Harness/fairness review confirmed that `cold/214,hot/214` are an internally
  fair 100-commit current-prototype pair: same commit list, same binary
  `/nix/store/18fvl...`, same nix SHA
  `f188b572fa2017e7bd4c74a1808df4f5491547f322f5265c3beed53512eaf22d`,
  same experiment flags, no stats/debug, empty stderr, and matching outputs.
  It also correctly warned not to make the headline 100-commit baseline claim
  until a matching 100-commit baseline is regenerated from
  `92a3df1ab706cf514357ae57b367050b373ff146`.
- Safety review found no false accept in the exact-path-only
  `derivedStorePath` skip and no false accept in the rejected-guard classifier.
  It did flag two open hardening items:
  - clean-source proof publication is still a TOCTOU check rather than an
    immutable source-read token;
  - accepted-result sidecars are safe after DOI recheck was restored, but the
    writer still emits misleading construction-discharge metadata that the v5
    loader intentionally does not trust.
- Performance review independently identified the cold plateau as candidate
  discovery/order, not final proof serving. In `cold/214`, the exact-anchor
  commits averaged about `14.14s`, while the alias-backed commits averaged
  about `0.99s`. The accepted target certs were often late in the arbitrary
  lexicographic scan order, e.g. the heavily reused `7fb36...json` target ranked
  sixth and backed most aliases.

Focused stats run before the change:

- `cold-stats/213` over the first 40 commits:
  - mean `2.5787993022750015s`, min `0.2877422299934551s`, max
    `18.07261928700609s`;
  - positions 27-40 formed the plateau at roughly `1.0-1.4s`;
  - representative plateau counters:
    - `4681...`: `candidateChecks=4`, `guardRejected=3`,
      `rejectedGuardClassifyUs=551654`, `scopedSourceServeUs=363660`,
      `verifyUs=174820`, wall `1.3602882060222328s`;
    - `88c3...`: `candidateChecks=4`, `guardRejected=3`,
      `rejectedGuardClassifyUs=559455`, `scopedSourceServeUs=89223`,
      `verifyUs=156620`, wall `1.0516788389650173s`;
    - `05d6...`: `candidateChecks=4`, `guardRejected=3`,
      `rejectedGuardClassifyUs=561213`, `scopedSourceServeUs=105127`,
      `verifyUs=203450`, wall `1.1445748070254922s`.
- Output hashes showed that `c5cf...`, `9e30...`, and `4681...` through
  `05d6...` all produced the same result hash. The problem was not output
  variance; the cache simply tried stale full certificates before the already
  known accepted target.

Implementation change:

- Added non-authoritative cross-head alias mining in
  `tryLoadProofJsonActionCache()`:
  - during directory scan, read existing `.json.alias` files;
  - count how often each target certificate is referenced;
  - sort full certificate candidates by alias target score before the previous
    filename fallback order;
  - still call the unchanged `tryEntry()` verifier for the selected candidate.
- This is intentionally only a candidate-order hint. It does not alter guard,
  source, store-path, DOI, scoped-source, or proof-verifier acceptance.

Validation after the change:

- Built successfully:
  `/nix/store/smc334pbla9jxf9ch1lag931vwyss8cf-nix-2.35.0pre20260515_dirty`.
- Ran `cold-stats/214` over commits 24-40 as a focused ablation. This window is
  harsher than the first40 run because it starts after the earlier `7fb36...`
  anchor is absent, so `c5cf...` becomes a new anchor. Even so, the plateau
  after the new anchor collapsed:
  - `4681...`: wall `0.585163782001473s`, `candidateChecks=1`,
    `guardRejected=0`, `rejectedGuardClassifyUs=0`;
  - `88c3...`: wall `0.2980436380021274s`, `candidateChecks=1`,
    `guardRejected=0`, `rejectedGuardClassifyUs=0`;
  - `05d6...`: wall `0.3627278939820826s`, `candidateChecks=1`,
    `guardRejected=0`, `rejectedGuardClassifyUs=0`.

Causal read:

- The alias-frequency ordering directly removed the repeated rejected-candidate
  classifier cost on the plateau. This is a useful abstraction to keep because
  it is authority-neutral: the index ranks candidates but cannot serve cached
  data by itself.
- Remaining cold cost is now mostly exact anchors and the first proof hit after
  a new anchor, not the old stale-candidate scan. Next step is to run a fresh
  100-commit no-stats current pair and, if the cold win holds, regenerate the
  matching 100-commit baseline from the baseline rev.

Full 100-commit current run after alias-frequency ordering:

- Built binary:
  `/nix/store/smc334pbla9jxf9ch1lag931vwyss8cf-nix-2.35.0pre20260515_dirty`;
  `result/bin/nix` sha256
  `9061624e37e895212775ffc4162039bc608c311287441bbba32062fc7d9c37c1`.
- Ran `cold/215,hot/215` no-stats on the 100-commit window.
- Cold:
  - mean `2.116276295846328s`, min `0.29848564200801775s`, max
    `16.115499310020823s`;
  - median dropped from `cold/214` `1.1397911320091225s` to `0.41581517696613446s`;
  - top exact anchors remained: `208e...` `16.115499310020823s`,
    `7d761...` `15.702725549985189s`, `cdeeb...`
    `14.767794098996092s`, `169b...` `14.558738525025547s`,
    `713e...` `14.40119267301634s`, `8ca3...` `14.2927690789802s`,
    `a292...` `13.859271574998274s`, `3451...`
    `13.804202526982408s`, `8417...` `13.68957358901389s`,
    `1cfd...` `13.49568209098652s`;
  - non-anchor >0.8s tail shrank to only `66262...` `2.1587932049878873s`,
    `41e2...` `1.7704631790402345s`, `0d3e...`
    `1.2159654140123166s`, `a23a...` `0.9435979020204395s`,
    `c5cf...` `0.8316777839795686s`.
- Hot:
  - mean `0.24286277930019423s`, min `0.1447544789989479s`, max
    `0.6620006010052748s`;
  - top timings were `0d3e...` `0.6620006010052748s`, `c5cf...`
    `0.46278219198575243s`, `cdeeb...` `0.42347074300050735s`,
    `4681...` `0.38074408198008314s`.
- `cold/215` and `hot/215` used the same proof namespace
  `05/05889c36086152748b41592c5bb733ca1595bb0d693728e28aaa0b51dc6cadcd`.
- `cold/215` and `hot/215` eval outputs matched, and stderr logs were empty.

Updated interpretation:

- Alias-frequency candidate ordering is causally responsible for most of the
  cold mean improvement from `2.5687s` to `2.1163s`: the p50 collapse and
  focused stats counters show the stale-candidate verifier/classifier work was
  removed.
- The remaining cold problem is not the original projection abstraction; it is
  exact-anchor proof incompleteness. Several exact anchors are surrounded by
  output-identical commits but still cannot be accepted cross-head. The next
  research question is why the existing recovery/proof facts cannot bridge
  those anchors.

Post-review instrumentation and design-space update:

- Adversarial alias-ordering review found no false-accept path because aliases
  only rank candidates and `tryEntry()` still performs normal authorization.
  It did flag two practical risks:
  - stale or malicious aliases can poison ordering and reintroduce rejected
    candidate cost;
  - scanning all aliases is `O(alias count)` per cold lookup and was previously
    invisible inside `directoryIterUs`.
- Added alias-ordering counters:
  `aliasFilesScanned`, `aliasDecodeAccepted`, `aliasDecodeRejected`,
  `aliasDecodeUs`, and `aliasScoreTargets`.
- Built successfully with those counters:
  `/nix/store/zkyby30clh3qxvnibbbz6vrhij9k8vhl-nix-2.35.0pre20260515_dirty`;
  `result/bin/nix` sha256
  `3baca34282f8a2c77ad9627c7c7871d7b4762a49297dc442cb317940f5b86a91`.
- Focused `cold-stats/216` over commits 24-40 showed current alias scan cost is
  tiny:
  - `4681...`: `aliasFilesScanned=1`, `aliasDecodeUs=18`,
    `directoryIterUs=293`, `candidateChecks=1`, wall `~0.6s`;
  - `88c3...`: `aliasFilesScanned=2`, `aliasDecodeUs=16`,
    `directoryIterUs=257`, `candidateChecks=1`, wall `~0.3s`;
  - `05d6...`: `aliasFilesScanned=13`, `aliasDecodeUs=105`,
    `directoryIterUs=413`, `candidateChecks=1`, wall `~0.4s`.

Exact-anchor review:

- `cold/215` had 12 full certificate roots, 100 accepted-result sidecars, and
  88 aliases.
- Output equality shows most anchors are not obvious bugs in cross-head reuse:
  `7d761`, `fd3f`, `7fb3`, and `1cfd` are first occurrences of output groups;
  `cdeeb`, `8417`, `208e`, `8ca3`, and `713e` are unique outputs in the window.
- The suspicious recoverable misses are:
  - `a292...`, which has the same output as the earlier `7fb3...` group and
    immediate previous commit;
  - `3451...`, which has the same output as the earlier `1cfd...` group;
  - `169b...`, which has the same output as the earlier `7fb3/a292...` group.
- `cold-stats/215` shows why they miss:
  - exact anchors have `hits=0`, `misses=1`, all tried candidates rejected by
    git guard, `rejectedGuardClassifyHits=0`, and
    `outputBoundaryRecoveryAttempts=0`;
  - `a292...`: `candidateChecks=7`, `guardRejected=7`;
  - `3451...`: `candidateChecks=8`, `guardRejected=8`;
  - `169b...`: `candidateChecks=11`, `guardRejected=11`.
- Diff probes show the narrow rejected-guard classifier is the blocker:
  - `7fb3 -> a292` includes guarded exact non-by-name changes such as
    `pkgs/tools/package-management/nix-prefetch-scripts/default.nix`, plus
    by-name renames/adds/deletes and top-level changes;
  - `a292 -> 169b` includes by-name renames/adds/deletes plus
    `pkgs/top-level/aliases.nix`;
  - `1cfd -> 3451` includes modified by-name files but also deletions/moves and
    `pkgs/top-level/all-packages.nix`.

Hot-sidecar design-space review:

- `hot/215` is already strong: mean `0.24286277930019423s`, p90 about
  `0.339s`, max `0.6620006010052748s`.
- The reviewer argues not to start with pathspec-limited dirty checks. Current
  scoped-source serving still pays for whole-worktree cleanliness, but the guard
  includes root directory listing `"."`, thousands of exact paths, and many
  directory/recursive scopes. A naive pathspec restriction is either ineffective
  or unsound unless directory snapshots are strengthened to detect missing
  tracked children and type changes.
- Better hot lead: a v6 compact accepted-result sidecar witness that avoids
  parsing the full ~1MB source cert on hot hits and carries an output-aware DOI
  closure witness. This must bind `(drvPath, outputName, expectedOutputPath)`,
  not just say a drv is in the output-root closure. If roots are missing, warn
  and fall back.

Next ranked work:

1. For cold, add classifier rejection reason counters, then prototype a broader
   cross-head recovery path for rejected guards that still renders current output
   and requires exact JSON/context equality before serving. This targets only
   the three suspicious recoverable anchors and is worth roughly `0.4s` on the
   100-commit cold mean if successful.
2. For hot, prototype compact sidecar witness in shadow/strict mode, with full
   fallback when observed-key files changed or output-root DOI witness is
   incomplete.
3. Regenerate the 100-commit baseline from
   `92a3df1ab706cf514357ae57b367050b373ff146` before making the final headline
   performance claim.

Classifier rejection-reason counters:

- Added counters:
  `rejectedGuardClassifyRejectGitComponent`,
  `rejectedGuardClassifyRejectNonByNameExact`,
  `rejectedGuardClassifyRejectByNameNonModify`,
  `rejectedGuardClassifyRejectExactOverlap`,
  `rejectedGuardClassifyRejectDirectory`,
  `rejectedGuardClassifyRejectRecursive`, and
  `rejectedGuardClassifyRejectEmpty`.
- Built successfully:
  `/nix/store/mhfh27m36fxyif3svys306j5ipyz3yv7-nix-2.35.0pre20260515_dirty`.
  `result/bin/nix` sha256
  `3a3ecad024ac98974ae3c782e8f886c74c3814e6510985facad38726501e3a53`.
- Ran `cold-stats/217` on a 5-commit anchor-reason window:
  `7fb3`, `1cfd`, `a292`, `3451`, `169b`.
- Output hashes confirmed the intended repeated-output setup:
  - `7fb3`, `a292`, and `169b` all produced
    `4a62b91571db14b5bd57dcf3a56600d1ce606f942957436f30e8aa5c582c469f`;
  - `1cfd` and `3451` both produced
    `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`.
- Rejection reason counters:
  - `a292...`: `candidateChecks=2`, `guardRejected=2`,
    `rejectNonByNameExact=2`, `rejectDirectory=2`, `rejectRecursive=1`;
  - `3451...`: `candidateChecks=3`, `guardRejected=3`,
    `rejectNonByNameExact=2`, `rejectDirectory=3`, `rejectRecursive=2`;
  - `169b...`: `candidateChecks=4`, `guardRejected=4`,
    `rejectNonByNameExact=3`, `rejectDirectory=3`, `rejectRecursive=2`.

Implication:

- These misses are not just "by-name package file changed" cases. They cross
  non-by-name exact deps plus directory/recursive guard authority. A narrow
  extension of the existing by-name classifier will not safely recover them.
- A broad "render current output and compare JSON/context" recovery would be
  authority-correct for that single run, but it does not create a reusable proof
  sidecar and could make hot slower by forcing the same render again. Do not add
  that as the main path unless it is clearly marked as a diagnostic oracle or is
  paired with a reusable witness.
- The next cold proof lead must explain why those non-by-name/directory/
  recursive changes cannot affect the demanded output, or accept that they are
  outside the current proof language.

Fresh adversarial review while regenerating baseline:

- `result` was switched to the historical baseline rev with
  `nix build -L --builders '' ".?rev=92a3df1ab706cf514357ae57b367050b373ff146" --out-link result --print-out-paths`.
  The resulting baseline binary is
  `/nix/store/x43bp7y47aqaj6h354mrqbsfnr61iyca-nix-2.35.0pre20260509_92a3df1`,
  sha256 `1f301376e2c54718a8ae98c89b55371bb68e72b6f978d9cbf7871e3e88d54051`.
  Stale `cold/0` and `hot/0` outputs were moved to
  `eval-trace-bench-results/_archive/pre-baseline-run0-20260526T153804Z`;
  baseline `cold/0,hot/0` is being regenerated with no experiment flags and
  no `NIX_SHOW_STATS`.
- Reviewers independently warned against making scoped dirty/pathspec checks
  serving authority. The current guard has broad exact, directory, and recursive
  scopes plus final whole-worktree clean checks; relaxing those without a
  stronger directory snapshot proof would be a likely soundness bug.
- Reviewers converged on the hot lead: compact accepted-result proof witness,
  scoped first to the proof JSON action accepted-result path. The useful target
  is reducing repeated source-cert parsing and residual DOI/derived-store-path
  verification on hot sidecar hits, not building a second libexpr serving plane.
- Important stale-format finding: accepted-result sidecar v5 writes
  `constructionDepDischarge`, including derived/DOI counts and digests, but the
  reader still treats the source certificate as authority and does not validate
  those digest fields as a proof. That is sound because the source cert is still
  read and verified, but it means v5 is not yet a compact authority format. A
  v6 sidecar must either validate copied proof fields directly or keep this
  metadata explicitly diagnostic.
- Run `hot-stats/212` shows why this matters. On a representative 26-commit
  stats run, accepted-result sidecar hits averaged about `157741us` total:
  `12667us` reading/parsing sidecar/cert, `17763us` guard work, `54826us`
  residual verification, `48117us` derivation-output-identity work, and
  `66171us` scoped-source serving. The largest safe next opportunity is the
  DOI/residual portion, while scoped-source clean validation remains a separate
  floor.

### 2026-05-26 baseline reset, v6 sidecar ablation, and source-cert-authorized result

The deleted benchmark results were regenerated with run `0` as the reference.
The baseline binary was built from
`92a3df1ab706cf514357ae57b367050b373ff146` using:

```
nix build -L --builders '' ".?rev=92a3df1ab706cf514357ae57b367050b373ff146" --out-link result --print-out-paths
```

Actual regenerated baseline over the 100-commit window:

- `cold/0`: mean `3.563573s`, p50 `1.138732s`, p90 `11.016958s`,
  max `13.859663s`.
- `hot/0`: mean `1.047246s`, p50 `1.036625s`, p90 `1.145514s`,
  max `1.171738s`.
- Baseline manifest binary:
  `/nix/store/3j3qgg7cdi2qfx10n62ycl3qrn16ci7l-nix-2.35.0pre20260509_92a3df1/bin/nix`,
  sha256 `1f301376e2c54718a8ae98c89b55371bb68e72b6f978d9cbf7871e3e88d54051`.
- `/home/connorbaker/nixpkgs` was clean on `master`,
  HEAD `7d7616894ec49a9ef1959295dd0be90f46f2c5b6`.

The accepted-result v6 experiment tried to make the sidecar self-contained by
copying `residualDeps`, `derivationOutputIdentityDeps`, guard refs, source
guards, observed-key guards, and output into `accepted-result.json`.

Runs on the 5-commit anchor window:

- `cold-stats/218,hot-stats/218`: v6 DOI-closure parser bug. Hot mean
  `0.373073s`; DOI closure rejected with zero parsed deps and fell back.
- `cold-stats/219,hot-stats/219`: parser fixed, but DOI closure still rejected
  every time and was too expensive. Hot mean `0.732973s`; DOI closure walked
  `15065` deps total and averaged about `357ms` per commit before fallback.
- `cold-stats/220,hot-stats/220`: DOI closure removed from the hot path. Hot
  mean `0.214063s`, output hashes matched, stderr was empty, and counters
  showed `5/5` exact accepted-result sidecar hits. Average hot sidecar cost was
  about `151ms`, including about `53ms` DOI verification and `54ms`
  scoped-source serving.

Adversarial read:

- The v6 sidecar was fast, but it widened authority. The loader synthesized a
  source certificate from the sidecar and no longer proved the copied fields
  matched the original accepted source cert. That is not acceptable as a
  hardening path unless the sidecar becomes a first-class proof certificate with
  its own completeness argument.
- The DOI-closure idea was the wrong shape for this workload. The output
  context roots are top-level system derivations, while the copied DOI set
  contains thousands of intermediate derivations. Walking/checking that closure
  was slower than the direct DOI verifier and still rejected.
- The useful abstraction is still the accepted-result sidecar as a small,
  exact-result locator. It is not yet useful as a standalone proof authority.

Implementation adjustment:

- Removed the unused DOI-closure acceptance function and counters.
- Removed v6 self-contained sidecar loading.
- Restored new sidecars to `accepted-result.v5` shape: the sidecar records the
  output, source cert ref, store-path ref, and roots, but the loader hash-checks
  and reads the source certificate before serving. This keeps authority on the
  source cert and avoids the large copied dep arrays.
- Built the benchmarkable CLI package directly with:

```
nix build -L --builders '' .#nix-cli --out-link result --print-out-paths
```

Candidate binary:
`/nix/store/hqnkji43dk7kw4rh94a0k8bk8g21scsw-nix-2.35.0pre20260515_dirty/bin/nix`,
sha256 `96cfa9437ea2311d0c62cce8c7d39054982f56fd360e4d6337502a0fcc996279`.
The top-level `nix build .` currently pulls in functional tests and failed
there; `.#nix-cli` is the correct benchmark target for this iteration.

Run `216` is the 100-commit wall-only gate for the source-cert-authorized
candidate. It used the same commit window as `cold/0,hot/0`, no stats/debug,
explicit `--nixpkgs-branch master`, and the proof-action experiment flags.

- `cold/216`: mean `2.076039s`, p50 `0.377351s`, p90 `13.437558s`,
  max `17.011066s`.
- `hot/216`: mean `0.220917s`, p50 `0.216901s`, p90 `0.300133s`,
  max `0.644694s`.
- Output hashes matched `cold/0`, `hot/0`, `cold/216`, and `hot/216` for all
  100 commits.
- `cold/216` and `hot/216` stderr logs were empty.
- Pairwise hot comparison: `hot/216` total `22.09s` vs `hot/0` total
  `104.72s`; mean ratio `0.211x`; all 100 commits faster by more than 5%.
- Pairwise cold comparison: `cold/216` total `207.60s` vs `cold/0` total
  `356.36s`; mean ratio `0.439x`; 86 commits faster by more than 5%, 14
  slower by more than 5%.
- Artifact shape across `cold/216` and `hot/216`: `200` accepted-result
  sidecars totaling `233891` bytes, max `2340` bytes; `176` alias files;
  `24` guard files; `24` store-path sidecars.

Causal read:

- The design is not fundamentally incapable of beating the baseline. The
  source-cert-authorized accepted-result path beats the regenerated hot baseline
  by about `4.7x` and improves cold mean by about `42%`.
- The v6 copied-dep sidecar was the wrong implementation even though the
  5-commit hot number looked good. It moved proof payload into the sidecar and
  made the sidecar a second authority plane. The v5-sized sidecar keeps the
  performance-relevant exact-result hit while preserving the existing cert
  authority story.
- Cold p90/max are still worse than baseline because exact proof publication
  anchors are slower than the baseline on changed-output commits. The next cold
  work is not shape projection; it is reducing exact-anchor publication cost or
  improving candidate selection so fewer commits become expensive anchors.
- Hot residual cost remains DOI verification and scoped-source checks. The next
  safe leads are batched DOI verification, a shadow root-output DOI witness, and
  a shadow scoped-dirty check before any serving relaxation.

Focused stats after the v5/source-cert-authorized cleanup:

- `cold-stats/221`: mean `15.876903s`, p50 `15.320746s`, max
  `18.317363s` on the 5 anchor commits.
- `hot-stats/221`: mean `0.177952s`, p50 `0.177497s`, max `0.183342s`.
- Output hashes matched cold vs hot for all 5 anchor commits; stderr was empty.
- Hot counters: `acceptedResultSidecarHits=5`, `exact=5`, `cross=0`,
  average `acceptedResultSidecarTotalUs=116055us`,
  `acceptedResultSidecarReadUs=6420us`, `guardUs=5929us`,
  `residualUs=51384us`, `derivationOutputIdentityDeps=15065`,
  `derivationOutputIdentityTotalUs=46741us`, and
  `scopedSourceServeUs=50145us`.
- Compared to v6 self-contained run `hot-stats/220`, source-cert-authorized
  v5 improved average sidecar time from `151365us` to `116055us`. The copied
  dep arrays were not only the wrong authority model; they were not faster on
  this anchor window.

### 2026-05-26 DOI validity batching ablation

Tried a narrow optimization in `proofJsonActionDerivationOutputIdentityDepsAccept`:
parse all unique DOI derivation paths first, batch their validity check with
`queryValidPaths`, then call `computeProofDerivationOutputIdentityOutputs` with
the per-path `isValidPath` check skipped. This preserved the later derivation
read and `computeStorePath` identity check, so it was intended to be a safe
transport-only optimization.

The ablation was built as:

```
nix build -L --builders '' .#nix-cli --out-link result --print-out-paths
```

Candidate binary:
`/nix/store/d5bbzl4rnbli9sqh0yfv698g6wcdpxsa-nix-2.35.0pre20260515_dirty/bin/nix`,
sha256 `79e45d94902442532648aa8b446121fd8ec47f2f4df15181fa628f24725e572b`.

Focused stats run `222` used the same five anchor commits as `221`, no
`NIX_SHOW_STATS`, explicit `--nixpkgs-branch master`, and the same proof-action
experiment flags.

- `cold-stats/222`: mean `15.893580s`, p50 `15.375448s`, max
  `18.208132s`.
- `hot-stats/222`: mean `0.191090s`, p50 `0.191716s`, max `0.197062s`.
- Output hashes matched cold vs hot for all five commits; stderr was empty.
- Hot counters stayed at `acceptedResultSidecarHits=5`, `exact=5`, `cross=0`,
  and `derivationOutputIdentityDeps=15065`, but average
  `acceptedResultSidecarTotalUs` worsened from `116055us` to `126357us`.
  `derivationOutputIdentityTotalUs` worsened from `46741us` to `50001us`;
  `residualUs` worsened from `51384us` to `54994us`; scoped-source time also
  moved up from `50145us` to `55444us`.

Causal read:

- The batch validity query did not reduce the dominant DOI cost. The old path
  already benefited from store-local caching and avoided building the extra
  parse/map/set prepass. For this workload, reading/checking derivations and
  comparing output identities dominates more than individual `isValidPath`
  calls.
- Because the patch made the measured hot target slower and did not help cold,
  it was removed. The current `result` is back on the source-cert-authorized
  non-batched path:
  `/nix/store/awpscddgddicb12wmrclc0g3y9cp4nph-nix-2.35.0pre20260515_dirty/bin/nix`,
  sha256 `8e2a91faec423224cf0e3e08026fc01b7771e8f000a1c5a4890f88def4cdefbf`.
- Next hot work should not be more validity-call batching. It needs to either
  reduce the number of DOI identities that must be checked on a sidecar hit, or
  introduce a separately validated compact witness that is at least shadowed
  before serving.

### 2026-05-26 direct capture vs trace-session outPath publication

Adversarial reviewers challenged the cold tail and pointed at exact-anchor
publication, not hot sidecar loading, as the current limiting factor. Existing
stats already split the anchor path enough to reject a few hypotheses:

- On `cold-stats/221`, average `serveJsonTotalUs` was `15763587us`.
- `directProofCaptureTotalUs` averaged `14482290us`.
- Inside that, `directProofCaptureRenderUs` averaged `12947317us` and
  `directProofCaptureStoreUs` averaged `1248881us`.
- The certificate write path itself is not the tail: average
  `proofJsonAction.store.totalUs` was `1248881us`; most of that was guard
  encoding (`966024us`). Cert dump and filesystem writes were negligible.

The direct-capture experiment runs before the trace-session JSON/outPath
projection path, so `cold-stats/221` had zero `outPathPublish` attempts. I
ablated the direct-capture flag on the same five anchor commits by removing only
`NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_DIRECT_OUTPUT_PROOF_CAPTURE` and keeping
the rest of the proof-action flags unchanged.

Run `223` results:

- `cold-stats/223`: mean `20.254824s`, p50 `19.520983s`, max
  `22.621723s`.
- `hot-stats/223`: mean `0.245977s`, p50 `0.249994s`, max `0.275119s`.
- Output hashes matched cold vs hot for all five commits; stderr was empty.
- `cold-stats/223` had `outPathPublishAttempts=5` and `successes=5`, but
  `outPathPublishTotalUs` averaged `17243574us`; nearly all of that was child
  capture/force (`childCaptureUs=17088704us`, `forceUs=17085115us`).

Causal read:

- Direct proof capture is not the source of the cold tail; it is currently the
  least-bad publication path for this workload. The trace-session outPath
  publication path records more fine-grained rows, but it pays extra child
  capture overhead and loses badly on both cold and hot.
- The next cold question is inside direct render itself. The `13ms`-scale
  certificate dumping/writing path is not relevant; the `13s` render block is.
  We need phase counters for file eval, auto-call, attr-path lookup, target
  force, narrow outPath rendering, and generic JSON fallback before picking a
  rewrite.

Instrumentation caveat:

- Added direct-render phase counters for setup, input eval, auto-call,
  attr-path lookup, target force, narrow outPath rendering, and generic JSON
  rendering. These counters are diagnostic only; headline comparisons must stay
  on no-stats runs.
- To check whether the added compiled counter sites perturb no-stats behavior, I
  ran an A/B/A on the same five anchor commits with `NIX_SHOW_STATS` unset:
  - `cold/217` pre-counter binary
    `/nix/store/awpscddgddicb12wmrclc0g3y9cp4nph-nix-2.35.0pre20260515_dirty`,
    sha256 `8e2a91faec423224cf0e3e08026fc01b7771e8f000a1c5a4890f88def4cdefbf`:
    mean `14.283100s`, p50 `13.785852s`, max `16.935031s`.
  - `cold/218` instrumented binary
    `/nix/store/lisrbg58w02jja40nxjcvyladypfwvy3-nix-2.35.0pre20260515_dirty`,
    sha256 `9f5bd87997c061b7ac2c515a601ea7d29736c4dfa45c4a793567ba9b001569d7`:
    mean `13.680320s`, p50 `13.037536s`, max `16.274037s`.
  - `cold/219` pre-counter binary again: mean `13.493317s`,
    p50 `12.904299s`, max `16.004131s`.
  - Hot means were `0.197486s`, `0.189979s`, and `0.198402s`.
- Read: no-stats performance changed less than ordering/cache noise. The new
  counters are acceptable for diagnosis, but their stats-run timings should not
  be mixed with no-stats benchmark claims.

### 2026-05-26 narrow outPath child-force split and tail ablation

Added a second layer of diagnostic counters inside
`tryRenderAttrsetOfDerivationOutPathsAsJson` for key recording, child force,
child attr lookup, `outPath` printing, and final JSON dump. The current
instrumented binary is
`/nix/store/5ggn7w4kprl1230qmnpxlnmhr88z4f9w-nix-2.35.0pre20260515_dirty/bin/nix`,
sha256 `bca35e9fe01ed164de715ae26ed8633db69a1f243537cdb6e99e6924400ebb13`.

Run `225` repeated the five exact-anchor diagnostic with stats enabled only for
attribution:

- `cold-stats/225`: mean `16.014198s`, p50 `15.458071s`, max
  `18.572931s`.
- `hot-stats/225`: mean `0.222332s`, p50 `0.219598s`, max `0.235438s`.
- Output hashes matched cold vs hot for all five commits; stderr was empty.
- Average direct-capture render split:
  - `directProofCapture.totalUs`: `14465249us`
  - `directProofCapture.renderUs`: `12991790us`
  - `renderNarrowOutPathUs`: `12989199us`
  - `narrowOutPath.childForceUs`: `12989170us`
  - `keyRecordUs`: `2us`, `childLookupUs`: `0us`,
    `outPathPrintUs`: `1us`, `jsonDumpUs`: `9us`

Causal read:

- The direct-render cold tail is not JSON formatting, attr lookup, key
  recording, or output string printing. It is `state.forceValue` on the two
  child derivations while dependency/proof capture is active.
- This also means micro-optimizing `printValueAsJSON` or object construction is
  not relevant to the benchmark goal.
- The counters were used only to locate cost. The headline comparisons below
  are no-stats runs.

Adversarial review corrected the framing: the full `~13s` child-force cost is
not all avoidable because the pre-schema baseline also spends `~10.7-13.9s` on
these heavy outputs. The actual cold problem is the candidate's excess tail.
From the 100-commit `cold/216` vs `cold/0` comparison there are 14 commits
slower than baseline by more than 5%; their ordered window is recorded at
`/tmp/eval-trace-cold216-regressors-window.txt`.

No-stats control on that 14-commit window, current source-cert/direct-proof
capture flags:

- `cold/226`: mean `12.630749s`, p50 `14.364322s`, max `16.451882s`.
- Outputs matched `cold/0` for all 14 commits; stderr was empty.
- The largest deltas versus baseline remained `208e...` `+5.770s`,
  `8ca...` `+4.454s`, `345...` `+3.688s`, `713...` `+3.429s`, and
  `169...` `+3.420s`.

No-stats render-only lower bound on the same window, with direct proof capture,
accepted-result sidecar serving, and aggregate proof serving disabled, and
`NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_DIRECT_OUTPUT_MEMO=1` enabled:

- `cold/227`: mean `7.673143s`, p50 `7.391402s`, max `10.758287s`.
- Outputs matched `cold/0` for all 14 commits; stderr was empty.
- On the 12 heavy anchors, render-only beat baseline by roughly `2.9-4.4s` and
  cut `5.6-8.7s` from the current direct-proof-capture control.
- The two small regressors got much worse: `41e...` went from baseline
  `1.235733s` / current `2.094108s` to render-only `7.367838s`; `662...`
  went from baseline `1.028385s` / current `2.231398s` to render-only
  `7.261413s`.

Current conclusion:

- For the heavy exact-anchor shape, the cold regression is mostly proof/deps
  capture overhead around child derivation forcing, not unavoidable Nix
  evaluation. There is a real potential cold win if we can publish/accept a
  narrower authority for exactly the `{ system -> outPath }` result.
- Render-only is not a serving strategy. It lacks proof authority and regresses
  small-output cases that the current traced/projection paths handle quickly.
- The next design target should be selective: keep current fast paths for small
  cases, but add a sound narrow proof for heavy aggregate outPath objects that
  avoids full construction-dependency capture. A candidate proof must bind:
  source identity/route, child attr membership, child `.drv` path, `out` output
  identity, and the store-path context needed for realization warnings.

### 2026-05-26 exact-source proof prototype and hybrid ablation

Implemented a gated exact-source direct proof prototype in
`src/libcmd/installable-attr-path.cc`. It renders the command JSON without a
trace session, then publishes a certificate whose source authority is the full
clean git revision of the evaluated repo. This deliberately avoids the cold
child-force cost of dependency capture, but it is exact-head authority unless a
later proof form is added.

Always-exact-source run `232` on the 14 cold regressors:

- Candidate binary:
  `/nix/store/vm9049f10kf4gsdxw551lbzndb33knlw-nix-2.35.0pre20260515_dirty/bin/nix`,
  sha256 `0f51f69baa02c2718dd81d49ad632b7b2b0fe2734f94a2c95811fc166c67dfb7`.
- `cold/232`: mean `9.547536s`, p50 `9.482666s`, p90 `10.713537s`,
  max `11.044910s`.
- `hot/232`: mean `0.665580s`, p50 `0.643955s`, p90 `0.857852s`,
  max `0.998898s`.
- Outputs matched. This beats the `cold/0` 14-regressor mean and max, and it is
  much better than `cold/226`, but hot is far slower than the source-cert
  sidecar path (`hot/216` mean `0.220917s`).

Then added a blunt history gate with
`NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_EXACT_SOURCE_DIRECT_PROOF_MIN_CERTS=1`
to seed the first miss with a full proof and exact-source later misses. Run
`233` used:

- Candidate binary:
  `/nix/store/637ypn686rw84wlp1k200bpx5ckf91g3-nix-2.35.0pre20260515_dirty/bin/nix`,
  sha256 `71df2ab09cabd699075e7b0dfebca5c35fc543e88ef32db5eab87f9ee39d2162`.
- `cold/233`: mean `10.027597s`, p50 `9.799910s`, p90 `10.716950s`,
  max `16.383889s`.
- `hot/233`: mean `0.689522s`, p50 `0.669754s`, p90 `1.009083s`,
  max `1.022711s`.
- Outputs matched.

Per-commit cold read versus `cold/0`:

- Heavy commits generally improved by `0.4-3.8s`.
- The first seed commit regressed by `+2.524s` because it still paid the full
  proof-capture publication cost.
- `41e249e270a3` regressed by `+8.367s`, and `66262d0a8ea1` regressed by
  `+9.689s`. These are the same small-cache-reuse commits that render-only
  damaged.

Adversarial review found two important design flaws in the blunt gate:

- Counting existing certificate files is not the same as observing that the
  just-completed proof-cache lookup checked and rejected stale cross-head
  candidates. It can select exact-source on small commits for reasons unrelated
  to full-proof tail cost.
- Encoding the threshold/stride in `proofJsonActionAuthorityMode` changes the
  proof namespace. Tuning the publication policy then changes the population
  being measured, which makes threshold ablations hard to compare unless every
  run starts from a clean matched state.

Current conclusion:

- Exact-source proof is a useful lower-bound/prototype because it proves the
  child-force capture overhead is avoidable, and it caps the heavy tail.
- It is not the final architecture. It weakens hot/cross-commit reuse and, when
  selected bluntly, destroys the standard source-cert reuse that made the small
  commits fast.
- The next implementation step is to make `tryLoadProofJsonActionCache` return
  a non-authoritative miss summary (`candidateChecks`, guarded candidate
  rejects, recovery attempts, full-clean cert checks) and gate exact-source on
  that summary. Separately, the longer-term candidate remains a narrow child
  output witness proof: child membership + child `.drv` + output identity +
  output context, without full construction-dependency capture.

Counter caveat:

- The counter A/B/A from runs `217-219` remains the only evidence that the
  compiled counter sites are not a large no-stats perturbation on the anchor
  window. Stats runs are still diagnostic only. Plain wall-time benchmark
  reports synthesize zero stats when `NIX_SHOW_STATS` is unset, so zero counters
  in no-stats results mean “not collected”, not “no work”.

### 2026-05-26 miss-summary gate and full-clean guard cleanup

Implemented a local `ProofJsonActionLoadMissSummary` returned from
`tryLoadProofJsonActionCache` instead of using directory certificate count as a
selector. The summary is not a serving authority; it records only the local
miss shape (`candidateChecks`, guarded candidates, full-clean candidates, guard
rejects, verifier rejects, and recovery attempts/hits). Threshold knobs were
removed from `proofJsonActionAuthorityMode` so tuning publication policy no
longer changes the proof namespace.

Run `234` used the adversarially suggested gate:
`MIN_CANDIDATES=2`, `MIN_GUARD_REJECTS=2`, no refresh.

- Candidate binary:
  `/nix/store/f5p2mfkbwqliwahi4fh5v6bkjb9cakk7-nix-2.35.0pre20260515_dirty/bin/nix`,
  sha256 `93b4ba98802aca6a85c711e96a6af423618bba2c4f18bf0adaa4e4c039aba631`.
- `cold/234`: mean `9.800744s`, p50 `9.402121s`, p90 `12.270482s`,
  max `16.418697s`.
- It improved most heavy commits versus baseline, but still regressed
  `41e249e270a3` by `+7.472s` and `66262d0a8ea1` by `+9.249s`; the first two
  seed commits also regressed because they paid full proof capture.

Run `235` added `REFRESH_STRIDE=3` to test whether periodic standard full
proofs recover the small commits.

- `cold/235`: mean `10.807104s`, p50 `9.923125s`, p90 `15.217045s`,
  max `15.522512s`.
- The refreshes landed on heavy commits and brought back full-proof tail cost.
  They did not recover `41e249e270a3` (`+7.882s` vs baseline). They did improve
  `66262d0a8ea1` to `3.459s`, but at the cost of several `14-15s` heavy
  refreshes.

Inspection of `cold/226` sidecars explained the small-commit behavior:

- `41e249e270a3` was served from a cross-head accepted result whose source cert
  was `1cfdcfa5620e13451b5c869742dd7cc00fb02e81`.
- `66262d0a8ea1` was served from source cert
  `169bbf0bfcad3beedc46540d09efb92411f8db24`.
- Exact-source publication does not persist the same reusable proof/trace state,
  so later output-boundary recovery loses the fast path even when the output is
  unchanged.

While reviewing this, found and fixed a prototype bug: full-clean exact-source
certificates were still writing a `.guard` file and `gitGuardRef`, causing later
lookups to treat exact-source certs as guarded stale candidates and pay
unnecessary guard work. Full-clean certs now use their source identity proof as
the source authority and do not write a guard sidecar.

Run `236` repeated always-exact-source after this cleanup:

- Candidate binary:
  `/nix/store/63y5gbl92xb8i0m2zcd0yh21pv3b25z8-nix-2.35.0pre20260515_dirty/bin/nix`,
  sha256 `d9d0e8d890a8e700f1960d2e7c6dbcc3349b98ddd0939d41895d526f20d03688`.
- `cold/236`: mean `9.045572s`, p50 `8.881934s`, p90 `10.361919s`,
  max `10.510705s`.
- `hot/236`: mean `0.679276s`, p50 `0.614559s`, p90 `0.894215s`,
  max `0.948756s`.
- The certs in `cold/236` have `sourceIdentityProofScope =
  full-source-clean-git-revision-v1`, no `gitGuardRef`, no `.guard` file, and
  empty retained deps.

Updated conclusion:

- Exact-source is now a cleaner lower bound and is better than baseline on all
  heavy commits in the 14-regressor window, but it still destroys the two
  small cross-head reuse cases.
- Threshold and periodic-refresh policies are not enough. The missing
  abstraction is a reusable narrow publication path: publish enough trace/proof
  state for future cross-head acceptance without forcing child derivations under
  full dependency capture. This points back to child output witnesses or
  projection/fragment rows, not more selector tuning.

### 2026-05-26 child-fragment reuse check

Tested the existing trace-session child-fragment/projection path as a causality
check on the `1cfd -> 208e -> 41e` small-reuse neighborhood. The run disabled
direct proof capture, enabled child fragments, and kept the standard proof
serving flags. It used the current guardless full-clean build, but did not use
exact-source publication.

Run `237`:

- `cold/237`: `1cfd... = 18.1s`, `208e... = 15.8s`,
  `41e... = 1.6s`.
- `hot/237`: `0.1s`, `0.4s`, `0.5s`.
- The `41e...` accepted-result sidecar was cross-head verified from source
  cert `1cfdcfa5620e13451b5c869742dd7cc00fb02e81`, matching the cheap
  behavior seen in `cold/226`.

Causal read:

- The broad trace-session/projection path can preserve the small cross-head
  reuse that exact-source loses.
- It does so by paying the same full child-capture/proof publication cost on
  source commits (`18.1s`, `15.8s`), which is worse than both current direct
  capture and the exact-source lower bound for heavy anchors.
- Therefore the missing piece is not “warm trace rows somehow”; it is a narrow
  reusable publication that gives output-boundary recovery enough authority
  without recording thousands of construction deps under child forcing.

### 2026-05-26 output-boundary existing-projection preflight

Added a feature-flagged preflight in the output-boundary recovery callback:
`NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_OUTPUT_BOUNDARY_TRACE_REUSE=1`. Before
falling back to `renderFileInstallableJsonOutputWithoutTraceSession`, it opens
the trace session and calls `tryServeOutPathJsonObject` for the current demand.
That path accepts only already-authorized existing projections; it does not run
the publishing `tryServeJsonOutputProjection` path and does not fabricate trace
rows from command-level exact-source authority.

The flag is intentionally not part of `proofJsonActionAuthorityMode`; it changes
how the recovery callback recomputes the current output, not the proof authority
used to serve.

Focused source-cert A/B on `1cfd -> 208e -> 41e`:

- Candidate binary:
  `/nix/store/5g3iki6km4h7z2yl3z8imlriz6rrbm6n-nix-2.35.0pre20260515_dirty/bin/nix`,
  sha256 `0c8d3eb6d68b0461df90e6b8f9d50c44638ab8e6be081862442f8ae4ec4f357d`.
- With trace reuse, `cold/238`: `1cfd = 15.455s`, `208e = 12.986s`,
  `41e = 0.882s`.
- Without trace reuse, same binary `cold/239`: `1cfd = 15.468s`,
  `208e = 13.184s`, `41e = 1.585s`.
- Both runs served `41e` from the same cross-head accepted source cert
  `1cfdcfa5620e13451b5c869742dd7cc00fb02e81`. The preflight cuts the recovery
  recomputation cost when suitable trace rows already exist.

Focused exact-source check:

- `cold/240`, exact-source plus trace reuse: `1cfd = 10.363s`,
  `208e = 7.727s`, `41e = 7.630s`; hot mean `0.801754s`.
- This confirms the preflight does not solve exact-source’s missing reusable
  rows. Exact-source remains good for the source commits and poor for the later
  small cross-head reuse commit.

14-regressor source-cert A/B on the current binary:

- With trace reuse, `cold/241`: mean `11.914411s`, p50 `13.308643s`,
  p90 `15.244301s`, max `15.464590s`.
- Without trace reuse, `cold/242`: mean `12.012258s`, p50 `13.460623s`,
  p90 `15.356949s`, max `15.507903s`.
- Hot was similar: `hot/241` mean `0.231224s`, `hot/242` mean `0.212834s`.
- Per-commit cold deltas were small except `41e249e270a3` (`-0.609s`).
  `66262d0a8ea1` was effectively unchanged.

Current conclusion:

- Existing-projection preflight is a safe, small additive improvement for the
  source-cert path. It is not the heavy-tail solution.
- Exact-source still needs a reusable narrow publication form. The next useful
  prototype is a shadow child-output witness, starting with facts already
  observable in the narrow `{ system -> outPath }` renderer: target keyset,
  child shape, child `.drvPath`, `out` output path, context, and store roots.
  It must remain shadow-only until it also proves the current source-to-child
  `.drv` binding.

### 2026-05-26 child-output witness shadow prototype

Implemented a gated shadow-only child-output witness in
`src/libcmd/installable-attr-path.cc`, controlled by
`NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_CHILD_OUTPUT_WITNESS_SHADOW=1`.

The witness is intentionally not serving authority. It writes a sibling
`*.child-output-witness-shadow.json` diagnostic file only after the normal proof
certificate was stored. With the flag unset, headline runs do not write the
sidecar or run the witness verifier.

Captured facts for the narrow object-of-derivations renderer:

- target keyset in rendered order;
- child attr name;
- child `drvPath` and its string context;
- child `outputName`;
- rendered child `outPath` and its string context;
- a shadow verification result that the rendered JSON matches the captured
  facts;
- a shadow DOI check that each child `.drv` maps the recorded output name to the
  recorded output path.

One-off diagnostic on current `master` Nixpkgs with the standard source-cert
flags plus the witness flag produced:

- witness file:
  `/tmp/eval-trace-witness-shadow.izmkdf/cache/eval-trace-json-action-proof-v20/e4/.../7d7616894ec49a9ef1959295dd0be90f46f2c5b6.json.child-output-witness-shadow.json`;
- `complete=true`;
- `renderedOutputMatches=true`;
- `derivationOutputIdentityAccepted=true`;
- `missingRequiredStorePaths=[]`;
- `children=2`: `aarch64-linux` and `x86_64-linux`.

This is a useful clarification: for `closures.gnome`, the narrow JSON object is
only the two top-level NixOS system derivations. The child witness can prove
that each already-recorded `.drv` maps `out` to the rendered `outPath`, but it
does **not** prove that a later source revision still selects those same top
level `.drv` paths. That source-to-child binding remains the hard proof
boundary.

No-stats focused A/B on `1cfd -> 208e -> 41e`, current build
`/nix/store/la19ybmw4mdwk2pxcyaadqw91b7iz7dy-nix-2.35.0pre20260515_dirty/bin/nix`
sha256 `3a515e3903c702b5121646b3e67c343915e92d3869f897154b0cc689c5a411e5`:

- run `243`, witness flag off:
  - cold: `15.469632611s`, `12.873839254s`, `1.596400952s`;
  - hot: `0.132517857s`, `0.504401693s`, `0.324108184s`.
- run `244`, witness flag on:
  - cold: `15.441400223s`, `13.339600696s`, `0.869673445s`;
  - hot: `0.138668918s`, `0.504317137s`, `0.347554376s`.
- output hashes matched between runs:
  - `1cfd...` and `41e...`:
    `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`;
  - `208e...`:
    `ac2dc39c6de15e043e57125e1846c02cb8e16ac1ca7808a13e0a5185072e9609`.
- run `244` wrote two cold witness sidecars and two hot witness sidecars; the
  `41e...` commit did not publish a new witness because it served through the
  existing cross-head path.

Counter/diagnostic caveat:

- The witness flag is treated like stats/counters: it is diagnostic-only and
  must not be included in headline timing comparisons.
- The A/B above suggests the compiled witness code is not materially perturbing
  the flag-off source-cert path, while the flag-on path can still move timings
  through sidecar writes, DOI reads, and ordinary benchmark noise.

Current conclusion:

- The witness abstraction is useful, but only as one half of a future narrow
  proof. It gives the child `.drv -> output` side of the argument.
- It does not by itself beat the baseline, because serving would still need
  proof that the current source revision selects the same child `.drv` paths or
  that any changed dependency packages are consumed only through accepted
  drv/output identity.
- The next design search should reuse the earlier dependency-output/current
  package lower-bound evidence without carrying forward its unsound serving
  decision: record a shadow producer-side boundary classifier for protected
  owner derivations, and check whether the owners of the changed by-name files
  consumed those packages only through drv/output identity.

Adversarial reviews after the witness prototype:

- Child witness review: the shadow witness proves only the cached child
  `.drv -> output` relation. Promoting it to serving authority without a
  current source-to-child `.drv` binding would repeat the same stale-output
  class as the root-output and current-package lower bounds.
- Performance strategy review: the safe source-cert architecture has already
  beaten the full-100 baseline on mean and hot performance (`cold/216` mean
  `2.076039s`, `hot/216` mean `0.220917s` versus regenerated baseline
  `cold/0` mean `3.563573s`, `hot/0` mean `1.047246s`), but it is not robust
  because cold p90/max are worse than baseline. The next benchmark gate should
  require both mean and tail improvement.
- Producer-boundary review: existing `ByNameDerivationConstructionFileBytes`
  and deferred-lane data is insufficient. It proves a by-name file was read
  during construction and that an owner drv has input drv/output edges; it does
  not prove the owner consumed the dependency only through drv/output identity.
  Run `114` is the falsifier: that unsafe lower bound was fast but returned
  stale JSON for four first-10 commits.

Refreshed safe-path control after adding the witness code, with the witness
flag **off**, current build
`/nix/store/la19ybmw4mdwk2pxcyaadqw91b7iz7dy-nix-2.35.0pre20260515_dirty/bin/nix`
sha256 `3a515e3903c702b5121646b3e67c343915e92d3869f897154b0cc689c5a411e5`:

- run `245`, 14 cold regressors, no stats:
  - cold times:
    - `7d7616894ec4`: `15.702217854s`;
    - `fd3fbe0cfd62`: `12.118391601s`;
    - `7fb36e13811a`: `12.334186216s`;
    - `cdeeb7e117a7`: `14.069810300s`;
    - `84172e791e34`: `13.243060308s`;
    - `1cfdcfa5620e`: `13.330792575s`;
    - `208e7dd03b5c`: `15.815959209s`;
    - `41e249e270a3`: `2.270023319s`;
    - `a292268556ef`: `13.394754321s`;
    - `34513330fcc1`: `13.447416014s`;
    - `8ca33461aec9`: `14.060241109s`;
    - `713e4d0f99dd`: `13.856190293s`;
    - `169bbf0bfcad`: `13.883053843s`;
    - `66262d0a8ea1`: `2.080487721s`.
  - hot times range from `0.142432524s` to `0.527416393s`.
  - no witness sidecars were written (`find ...child-output-witness-shadow`
    count `0`), as intended.

Current implementation decision:

- Do not spend more time on OBR trace reuse, DOI batching, or exact-source
  selector tuning as the main path; prior ablations show those are small,
  negative, or destroy cross-head reuse.
- The only plausible path to a robust baseline win is a conservative
  `by-name-boundary-v1` shadow classifier. It must be false-negative-friendly:
  allowed observations are output identity (`outPath`, `drvPath`, concrete
  output attrs and matching string context); forbidden or unclassified
  observations are arbitrary attrs, `meta`, `passthru`, `hasAttr`, `attrNames`,
  missing-attr behavior, custom `__toString`, function application, shape
  operations, and generic attr forcing.
- Serving must remain disabled until the classifier has zero false positives on
  the run `114` falsifiers and a broader by-name-change window.

### 2026-05-26 by-name boundary shadow diagnostic

Implemented the first non-serving `by-name-boundary-v1-shadow` diagnostic.
Build:

- `/nix/store/xw2d0xwlw8yqwb7mqcddgih68hziv6ra-nix-2.35.0pre20260515_dirty/bin/nix`
- sha256 `4f9aaac9ce78d9c9a44ebbd6d616ddc77d30e9c61663add95e0ca14b27144713`

What changed:

- `DerivationStrictFileBytesScope` now carries shadow
  `ByNameBoundaryObservationSummary` data alongside the existing
  by-name `FileBytes` construction facts.
- The deferred by-name diagnostics can emit per-owner `byNameBoundary` data:
  allowed output-identity observations, forbidden observations, unknown count,
  allowed attrs, forbidden attrs, and forbidden observation kinds.
- Hooked common Nix-level consumers:
  - `ExprSelect` success, defaulted miss, and missing-attr error;
  - `ExprOpHasAttr`;
  - string coercion through `outPath` / custom `__toString`;
  - `builtins.getAttr`, `hasAttr`, `attrNames`, `attrValues`,
    `unsafeGetAttrPos`, `removeAttrs`, `intersectAttrs`, `catAttrs`,
    `mapAttrs`, `zipAttrsWith`, `genericClosure`;
  - JSON/XML conversion.

This remains shadow-only:

- It is emitted only through the diagnostic/deferred-lane path.
- It is not part of serving authority.
- It does not alter output-boundary recovery acceptance.
- It is not a headline benchmark feature; runs with
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_BY_NAME_PACKAGE_SEMANTICS_DIAGNOSTIC`
  are diagnostic runs.

Adversarial read:

- The current shadow data is **not** enough to authorize cross-head serving.
- It still lacks complete coverage for C++ direct attr readers such as
  `findAlongAttrPath`, `get-drvs.cc`, and arbitrary `EvalState::getAttr`
  callsites.
- `unknown == 0` currently means "no instrumented unknowns", not "complete
  observation coverage"; there is not yet a central poison hook for every
  unclassified derivation-attrset use.
- Subject discovery is order-sensitive: a consumer can inspect a dependency
  derivation before its by-name output facet has registered the subject.
- The recorded owner data is still owner-wide, not causal. It does not prove
  that a particular package attrset flowed only as a drv/output identity edge
  into a particular owner input.
- Most importantly, this still does not prove current source-to-child `.drv`
  equality. The child-output witness proves cached child `.drv -> output`;
  the boundary diagnostic can suggest output-only consumption; neither proves
  that the current source revision still selects the same top-level child
  derivations.

Counter/stats discipline:

- Headline comparisons must be wall-only: no `NIX_SHOW_STATS`, no
  `NIX_SHOW_STATS_PATH`, no `--debug`, and no diagnostic sidecar flags unless
  that sidecar is the subject of an explicit ablation.
- Counter-enabled runs are causal diagnostics only. `NIX_SHOW_STATS` is not
  passive: it enables atomic counter writes, timing calls, extra diagnostic
  scans/maps, full GC, stats JSON writes, and in some paths different
  writeback behavior.
- Existing wall-only benchmark manifests record `NIX_EVAL_TRACE_*` flags, but
  the ledger must keep exact command lines, binary path/hash, commit list, env
  flags, cache-state policy, run IDs, and artifact counts. A zero counter in a
  wall-only report means "not collected", not "the event did not happen".

Near-term ablation plan:

1. Wall-only current binary with no JSON-action flags: isolates general branch
   overhead.
2. Wall-only current binary with the source-cert publication flags but without
   accepted-result sidecar serving: measures proof publication without the hot
   sidecar path.
3. Wall-only current binary with the full run-216-style flag bundle: headline
   candidate.
4. Stats/counter shadow only for the focused anchor window after the wall-only
   runs, to explain route/load/verifier/materialization costs without using
   those wall times as benchmark results.
5. Diagnostic by-name-boundary runs only on the focused window first, checking
   whether `outputOnlyCandidate` is rare/common and whether run `114`
   falsifier-style commits would be rejected before any serving design is
   considered.

### 2026-05-26 direct-capture by-name semantics diagnostic

Follow-up after the first boundary diagnostic: the aggregate direct-output proof
capture path was writing the main certificate with
`byNameDerivationOutputFacets`, but it was not writing the separate
`*.by-name-package-semantics.json` diagnostic sidecar. That meant the previous
diagnostic run proved facet capture, not the boundary classification. Fixed
`renderFileInstallableJsonOutputWithProofCapture` to emit the diagnostic sidecar
only when
`NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_BY_NAME_PACKAGE_SEMANTICS_DIAGNOSTIC=1`.
The extra dep copy and JSON write are therefore diagnostic-only and should not
affect headline wall-only runs.

Build:

- `/nix/store/3d3dn368kdn988xa3c7y8f7h4ny3784r-nix-2.35.0pre20260515_dirty/bin/nix`
- sha256 `44b27acd93ebbf6729497dcec6137b0ab0908a8abacbe434f878110667be92ea`

Counter/diagnostic ablation:

- Wall-only full source-cert candidate, run `249`:
  - cold `1cfd...`: `17.194983060006052s`
  - cold `208e...`: `14.722446961037349s`
  - cold `41e...`: `1.3012111639836803s`
  - hot `1cfd...`: `0.13474677497288212s`
  - hot `208e...`: `0.5061828759498894s`
  - hot `41e...`: `0.3736286530038342s`
- Diagnostic sidecar but no stats, run `251`:
  - cold `1cfd...`: `17.517065032967366s`
  - cold `208e...`: `15.256689784000628s`
  - cold `41e...`: `1.2998499309760518s`
- Diagnostic plus stats/counters, run `250`:
  - cold `1cfd...`: `19.83016964897979s`
  - cold `208e...`: `17.593207767989952s`
  - cold `41e...`: `1.5766302339616232s`
  - hot `1cfd...`: `0.13301553699420765s`
  - hot `208e...`: `0.40529338299529627s`
  - hot `41e...`: `0.5046951369731687s`

This confirms the counter concern: `--with-stats` materially perturbs cold
timings, adding roughly `+2.6s` and `+2.9s` on the two source-heavy focused
commits versus run `249`. The no-stats diagnostic sidecar is also not free
(`+0.32s` and `+0.53s` on those commits), but it is far less intrusive. Use
run `249` for headline timing and runs `250`/`251` only for causal diagnosis.

Correctness check for run `251`:

- `1cfd...` and `41e...` eval JSON:
  `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`
- `208e...` eval JSON:
  `ac2dc39c6de15e043e57125e1846c02cb8e16ac1ca7808a13e0a5185072e9609`

Diagnostic sidecars from run `251`:

- `1cfd...json.by-name-package-semantics.json`
  - aggregate deps: `41095`
  - by-name file deps: `913`
  - derivation output facets: `1484`
  - construction diagnostics: `1898`
  - derived-store-path deps: `846`
  - boundary observations: `1640`
  - output-only candidates: `1639`
  - allowed output-identity observations: `1744`
  - forbidden observations: `2`
  - unknown observations: `0`
- `208e...json.by-name-package-semantics.json`
  - aggregate deps: `38738`
  - by-name file deps: `872`
  - derivation output facets: `812`
  - construction diagnostics: `1328`
  - derived-store-path deps: `844`
  - boundary observations: `924`
  - output-only candidates: `922`
  - allowed output-identity observations: `982`
  - forbidden observations: `4`
  - unknown observations: `0`

Observed allowed output attrs are overwhelmingly `out` and `dev`, with smaller
counts for `man`, `bin`, `doc`, `lib`, and a few named outputs. The only
forbidden boundary case in this focused window is `vte.options`: `vte` is
otherwise used through `out`, but an owner `etc.drv` also selects the `options`
attribute. That is exactly the kind of case a conservative classifier must
reject before serving.

Interpretation:

- The sidecar falsifies the fear that output-only boundary observations are too
  rare for this workload: they are common in the source-heavy `closures.gnome`
  window.
- It does **not** yet authorize serving. The current sidecar is diagnostic only,
  `unknown == 0` still means no instrumented unknowns rather than complete
  semantic coverage, and the classifier still needs a source-to-current-child
  binding or an owner-local proof that changed package facts are consumed only
  through accepted drv/output identity.
- The promising next implementation path is a false-negative-friendly
  acceptance check over the already-captured sidecar facts:
  reject if any changed by-name package has forbidden/unknown boundary
  observations for an owner reachable from the output context drv closure;
  otherwise require matching derivation-output facets for the exact
  `(system, drvPath, outputName)` edges before serving. This should be tested as
  a shadow verifier against run `114` falsifiers before any serving promotion.

Wall-only control after the diagnostic-hook patch, run `252`, full source-cert
bundle without the diagnostic env var:

- cold `1cfd...`: `17.350632865040097s`
- cold `208e...`: `15.12319093203405s`
- cold `41e...`: `1.5713199310121126s`
- hot `1cfd...`: `0.13854686199920252s`
- hot `208e...`: `0.3565400739898905s`
- hot `41e...`: `0.526734935992863s`
- by-name semantics sidecars written: `0`
- output hashes matched run `251` and the known expected hashes.

This is not a new performance claim; it is a control showing the diagnostic
hook is gated. The cold/hot variation relative to run `249` is ordinary focused
window noise plus the new binary layout, not evidence that the diagnostic
change improves the hit path.

### 2026-05-26 rejected-guard classification diagnostic and counter ablation

Adversarial review after the direct-capture diagnostic found a measurement hole:
the by-name shadow verifier was only attached to
`*.by-name-package-rejection.*.json`, and those rejection sidecars are written
only after `proofGitGuardPathsRejectedOnlyByNamePackageExactPaths` forms a
candidate. If the guard rejects because a by-name package change is mixed with a
directory-listing, recursive, non-by-name exact, add/delete, or overlap blocker,
the classifier returned `nullopt` and the durable artifact disappeared. With
stats enabled this was visible only as counters such as
`rejectedGuardClassifyRejectDirectory`; without stats there was no record.

Implementation change:

- Added diagnostic-only
  `*.by-name-package-classification.<currentHead>.json` sidecars.
- The sidecar is emitted under
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_BY_NAME_PACKAGE_SEMANTICS_DIAGNOSTIC=1`
  whenever a rejected Git guard is classified, including classifier-null cases.
- It records cert/guard refs, changed paths, per-change reject reasons,
  candidate by-name package paths when present, and whether the classifier
  formed an accepted exact-by-name candidate.
- If a candidate is accepted, it embeds the fail-closed
  `byNameBoundaryShadowVerdict`. If a hard blocker prevents an accepted
  candidate but by-name package paths were still identified, it embeds a
  `partialByNameBoundaryShadowVerdict` so we can separate "global guard blocker"
  from "package-level semantic proof unavailable".
- Serving behavior is unchanged. These artifacts have
  `servingAuthority=false`.

Shadow verifier fix from adversarial review:

- Boundary observations record selectors (`outPath`, `drvPath`,
  `outputName`) and output-name attrs (`out`, `dev`, ...), while facet maps are
  keyed by output name.
- The previous shadow check treated every allowed attr as an output name. That
  would reject valid `outPath` observations as `missing-cached-output:outPath`.
- The diagnostic verifier now handles selectors explicitly:
  - `drvPath` is covered by the current/cached drv-path equality check.
  - `outPath` maps only to standard output `out` for this prototype.
  - `outputName` remains unsupported/fail-closed until the fact schema records
    it.
  - other attrs are treated as output names.
- This deliberately stays conservative for multi-output/default-output details.

Builds:

- Classification sidecar build:
  `/nix/store/6w524ggj6ka73l15jbc4m38fyxjx49jr-nix-2.35.0pre20260515_dirty/bin/nix`
  sha256 `a805754314aea1f22f358fe1c2bff558e593e04b1094a3b969fd59e3531e97b6`.
- Partial-shadow build:
  `/nix/store/7mq0k9dcv0fi953c36pqam02x4w0ib2c-nix-2.35.0pre20260515_dirty/bin/nix`
  sha256 `8b30d4ca0302614ec9b9fd68ad25826dc8a573c430141bf6bd18e82379250b36`.

Counter discipline / same-binary ablations:

- Run `255` was stats+diagnostic on the run-114 falsifier pair
  `7d761689... -> fd3fbe0...`, binary
  `12e2831e528c1b9b0c739f6872a56015b5f818a77c770195b813b8d19aecf9fa`.
  It produced two semantics sidecars and no rejection sidecars. The second
  commit accepted the guard (`guardAccepted=1`, classifier attempts `0`), so it
  did not exercise the shadow verifier.
- Run `256` was the same binary/env/commit list as `255` but no stats:
  - `7d761689...`: `18.12841250799829s` vs stats `24.086323263007216s`
  - `fd3fbe0...`: `14.708369065017905s` vs stats `20.885458411998115s`
  - same output hashes:
    `a39611216363bc4e8460ad8ecd729aedeae8816d8314fca8a9a13114eac3c0c8`
    and
    `6d24645dee692e4435a52314abb1480c7f5a2b438fe02c9e023ee2ddfdc92c9f`.
  This is a clean counter/stats perturbation for that diagnostic workload:
  stats added about `+36.9%` total wall time.
- Run `261` and `262` repeat the same discipline on the
  `1cfdcfa... -> 345133...` v11-tail window, binary
  `8b30d4ca0302614ec9b9fd68ad25826dc8a573c430141bf6bd18e82379250b36`:
  - `cold/261` no stats: `19.188209137995727s`,
    `15.774809536989778s`, total `34.963018674985506s`.
  - `cold-stats/262`: `25.26090194901917s`,
    `22.18207906896714s`, total `47.44298101798631s`.
  - Same binary, dirty-tree hash, env, commit list, outputs, and sidecar
    counts; only `withStats` differs.
  - Stats therefore added about `+35.7%` total wall time on this exact
    diagnostic workload.

Conclusion: counters materially perturb the workload. Use stats runs only to
explain causality; never use them as headline timings or as evidence that a
serving path is fast.

Run `260`: classification sidecar validation, no stats, binary
`a8057543...`, same `1cfd -> 345` window.

- Timings:
  - `1cfd...`: `18.024698749999516s`
  - `345...`: `14.589219694025815s`
- Output hashes both matched
  `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`.
- Artifacts:
  - two semantics sidecars
  - one classification sidecar
  - no rejection sidecar
- Classification sidecar result:
  - `outcome`: `rejected-by-name-package-classifier`
  - candidate by-name package files: `libvpx`, `newt`
  - hard blocker: directory-listing guard on deleted `outline` paths under
    `pkgs/by-name/ou`
  - accepted candidate: `false`

Run `261`: same window after partial-shadow attachment, no stats, binary
`8b30d4ca...`.

- Timings:
  - `1cfd...`: `19.188209137995727s`
  - `345...`: `15.774809536989778s`
- Output hashes both matched
  `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`.
- Classification sidecar again reported:
  - accepted candidate: `false`
  - hard blocker: directory-listing guard
  - partial package candidates: `libvpx`, `newt`
- `partialByNameBoundaryShadowVerdict`:
  - `wouldServe=false`
  - both `libvpx` and `newt` had aggregate by-name file deps and construction
    diagnostics, but `boundaryObservations=0` and
    `outputOnlyObservations=0`.
  - reasons included `construction-owner-missing-boundary-observation`,
    `missing-boundary-observation-for-path`,
    `missing-output-only-observation-for-path`, `no-accepted-output-only-
    observations`, and `no-systems`.

Interpretation:

- The new artifact distinguishes two independent blockers for this cold-tail
  transition:
  1. The overall guard cannot be recovered because the diff includes deleted
     `outline` files that invalidate a directory-listing guard.
  2. Even if we ignored that global blocker, the package-level by-name boundary
     proof is absent for `libvpx`/`newt`; the current boundary design has no
     semantic authority for them.
- That is causal evidence against spending more time trying to make this exact
  v11-tail transition serve through the current output-boundary proof. A better
  path for this case is either a different source proof that handles directory
  deletion safely, or a package/source-to-child proof that does not depend on
  boundary observations that were never recorded.
- The classification sidecar is still useful: it gives a durable, no-stats
  explanation of missed recovery opportunities and lets us build a corpus of
  blockers before designing another serving rule.

Current design-space read:

- By-name boundary facts are useful abstractions for candidates where
  construction observes a package derivation only through output identity.
- They are not universal enough to be the only cold-tail strategy. The first
  concrete v11-tail near miss lacks boundary observations for the exact
  candidate packages.
- The design is not fundamentally incapable of beating the baseline, but the
  viable path is narrower than "serve if by-name output facets match":
  1. keep accepted-result/source-scoped sidecars for hot wins;
  2. use classification sidecars to inventory cold-tail blockers without stats;
  3. pursue boundary serving only for transitions whose artifacts show complete
     output-only observations;
  4. for transitions like `1cfd -> 345`, investigate directory-guard/source
     proof improvements or current source-to-child drv binding instead.

Run `263`: broadened the no-stats classification diagnostic to the original
focused window `1cfd... -> 208e... -> 41e...`, binary `8b30d4ca...`.

- Timings:
  - `1cfd...`: `18.912959351029713s`
  - `208e...`: `16.844339756004047s`
  - `41e...`: `1.6838122389744967s`
- Output hashes:
  - `1cfd...`: `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`
  - `208e...`: `ac2dc39c6de15e043e57125e1846c02cb8e16ac1ca7808a13e0a5185072e9609`
  - `41e...`: `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`
- Artifacts:
  - semantics sidecars for `1cfd...` and `208e...`
  - classification sidecar for `1cfd... -> 208e...`
  - classification sidecar for `208e... -> 41e...`
  - no rejection sidecars

Classification summaries:

- `1cfd... -> 208e...`
  - `acceptedCandidate=false`
  - changed paths considered: `12902`
  - partial candidate package paths: `55`
  - hard blockers: `byNameNonModify`, `directory`, `nonByNameExact`,
    `recursive`
  - partial shadow packages with boundary observations: `0/55`
  - partial shadow packages with output-only observations: `0/55`
  - top semantic reasons:
    `construction-owner-missing-boundary-observation` (`112`),
    `missing-boundary-observation-for-path` (`55`),
    `missing-output-only-observation-for-path` (`55`).
- `208e... -> 41e...`
  - `acceptedCandidate=false`
  - changed paths considered: `12903`
  - partial candidate package paths: `383`
  - hard blockers: `directory`, `nonByNameExact`, `recursive`
  - partial shadow packages with boundary observations: `0/383`
  - partial shadow packages with output-only observations: `0/383`
  - partial shadow packages missing construction diagnostics: `4/383`
  - top semantic reasons:
    `construction-owner-missing-boundary-observation` (`774`),
    `missing-boundary-observation-for-path` (`383`),
    `missing-output-only-observation-for-path` (`383`),
    `missing-construction-diagnostic-for-path` (`4`).

This is the strongest evidence so far against using the current
by-name-boundary abstraction as the primary cold-tail fix for
`closures.gnome`. The workload command is:

```sh
nix eval -f /home/connorbaker/nixpkgs/nixos/release.nix closures.gnome --json --no-pretty
```

and its JSON is just:

- `aarch64-linux` -> NixOS system top-level output path
- `x86_64-linux` -> NixOS system top-level output path

So the thing we ultimately need to preserve is the selected top-level system
derivation outputs. The classification artifacts show that many by-name package
files are exact deps or near-candidates, but the package boundary facts are not
present for those changed package files. In other words, this is not "changed
package derivation consumed only through output identity"; it is "changed
source was read somewhere during NixOS/module/package-set evaluation, but the
final top-level system output may or may not change."

Next search direction:

- Keep the classification sidecar because it tells us why a guard-rejected
  entry missed without enabling counters.
- Stop trying to turn the current boundary sidecar into a serving rule for the
  observed cold-tail transitions unless a future artifact actually shows
  complete output-only observations for candidate packages.
- Investigate a different proof for the outPath workload:
  1. prove current top-level selected derivation output identity directly;
  2. prove directory-listing changes like deleted by-name package directories
     are irrelevant when the observed directory child set was unchanged or the
     changed child was not selected;
  3. record a source-to-top-level-drv binding or child-output witness that is
     cert-bound, not diagnostic-only.

Run `264`: direct-output proof capture plus child-output witness shadow on the
`1cfdcfa... -> 345133...` v11-tail window, no debug, no stats, binary
`8b30d4ca...`.

- Env:
  - `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_DIRECT_OUTPUT_PROOF_CAPTURE=1`
  - `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_CHILD_OUTPUT_WITNESS_SHADOW=1`
- Timings:
  - `1cfdcfa...`: `18.570906111970544s`
  - `345133...`: `14.385104510991368s`
- Output hashes matched
  `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`.
- Artifacts:
  - two semantics sidecars
  - one classification sidecar
  - two child-output-witness shadow sidecars

Child-output witness findings:

- Both commits had `complete=true`, `renderedOutputMatches=true`, and
  `derivationOutputIdentityAccepted=true`.
- Both had the same target keyset:
  `["aarch64-linux", "x86_64-linux"]`.
- Both had the same children:
  - `aarch64-linux` drv
    `/nix/store/a0y4mram5p7bamwcwagwmr62jh1rk145-nixos-system-nixos-26.05pre708350.gfedcba.drv`
    -> outPath
    `/nix/store/7knbp1p6dkg3firld2fdk0saa1zfh9nq-nixos-system-nixos-26.05pre708350.gfedcba`
  - `x86_64-linux` drv
    `/nix/store/1vyrgpv6s5g5lk92ng8ks7fr66ks3jy2-nixos-system-nixos-26.05pre708350.gfedcba.drv`
    -> outPath
    `/nix/store/hi49lrx4gzi85m23k5v561n91phsh8bk-nixos-system-nixos-26.05pre708350.gfedcba`
- This confirms the rendered value is just the two selected top-level system
  derivation outputs, and those outputs are identical across this transition.
- It does not prove serving authority by itself. The witness proves
  cached-child `.drv -> outPath` identity and rendered JSON agreement after
  evaluation; it still lacks a cheap proof that the current source selects the
  same top-level drv paths without doing NixOS evaluation.

Classification findings for the same transition:

- `acceptedCandidate=false`
- candidates: `libvpx`, `newt`
- reject buckets: only `directory=true`
- the only recorded rejecting changes were deleted `outline` files under
  `pkgs/by-name/ou`:
  - `pkgs/by-name/ou/outline/missing-hashes.json`
  - `pkgs/by-name/ou/outline/package.nix`
- partial boundary shadow still had `wouldServe=false`; `libvpx` and `newt`
  each had construction diagnostics but zero boundary observations and zero
  output-only observations.

Interpretation:

- The child witness makes the output-identity target concrete, but the missing
  proof is source-to-selected-drv, not drv-to-output.
- The classification artifact is now pointing at a narrower directory-guard
  question: did the cached guard actually observe `outline` as a direct child
  of `pkgs/by-name/ou`, and did its entry kind/presence change? If yes, the
  current directory rejection is semantically justified. If no, the diagnostic
  needs to expose why the broad directory guard rejects despite the observed
  child filter.
- Next diagnostic edit: enrich classification sidecars for directory rejects
  with the immediate child, whether that child was recorded in
  `observedDirectoryChildren`, whether the change is below the direct child,
  and the base/head entry kind used by `directoryListingGuardRejects`.

### 2026-05-26 directory rejection provenance and witness binding

Adversarial review summary:

- Three parallel reviewers checked the current dirty branch, not master.
- They agreed the current by-name boundary shadow is useful only as a bare
  package-facet diagnostic. It does not prove the original
  `release.nix closures.gnome` command universe.
- They agreed the child-output witness is a compact payload/projection format,
  not serving authority. It proves cached `.drv -> outPath` facts for already
  selected child derivations, but it does not prove current source selects the
  same top-level derivations.
- They flagged that diagnostic sidecars can perturb no-stats wall time even
  without counters. I changed classification detail capture so per-change JSON
  and directory diagnostics are only built for the recorded 200-change prefix;
  aggregate reject buckets still scan all changes.
- They also flagged that witness sidecars needed self-binding metadata before
  they could be audited. The witness sidecar now records proof key, head rev,
  proof mode, schema/provider epochs, output digest, and `certRef`.

Implementation changes:

- Added `proofDirectoryListingGuardDiagnosticJson(...)` and attached
  `directoryListingDiagnostics` to recorded classification changes.
- Added diagnostic-only write-time directory provenance to certs when the
  by-name semantics diagnostic is enabled:
  - total directory-listing guard count
  - complete-keyset directory count
  - observed-child-map count
  - suppressed-complete-keyset observed-child-map count
  - per-directory `completeKeyset`, `observedChildrenRecorded`,
    `observedChildrenCount`, and suppression counts
- Copied that provenance into classification sidecars when present.
- Added metadata to child-output witness sidecars:
  - `servingAuthority=false`
  - `proofKey`, `repoRoot`, `headRev`
  - `cacheVersion`, `proofAuthorityMode`, `schemaEpoch`, `providerEpoch`,
    `hashAlgorithm`
  - `storeDir`, `currentSystem`
  - output digest and `certRef`

Builds:

- Full `nix build -L --builders '' .` compiled the binary but failed in the
  unrelated functional-test aggregate: 22 functional tests failed, including
  existing eval-trace/flakes cases. I did not use that as a blocker for the
  diagnostic binary.
- Narrow `nix build -L --builders '' .#nix-cli` after directory diagnostics:
  `/nix/store/crs52zmsn6l0n2sy2wwdfrs0519h5pb9-nix-2.35.0pre20260515_dirty`,
  sha256 `2fb4f86bfd77fde3791af3bc240ad174a0a1ac76d0f815e4811fa3940ec04efc`.
- Narrow `.#nix-cli` after witness/provenance metadata:
  `/nix/store/rllw7j5i7w3n7fwn8fmklqkn4gk3akad-nix-2.35.0pre20260515_dirty`,
  sha256 `5f875f4c61803a47bf12495f9947bd909fac443f2feaac740f598967ba010757`.

Run `265`: no-stats/no-debug directory diagnostic on the
`1cfdcfa... -> 345133...` v11-tail window, binary `2fb4f86b...`.

- Timings:
  - `1cfdcfa...`: `19.08165246999124s`
  - `345133...`: `14.807745333993807s`
- Output mode: wall-only; `withStats=false`.
- Classification:
  - candidates: `libvpx`, `newt`
  - reject bucket: only `directory=true`
  - rejecting changes:
    `pkgs/by-name/ou/outline/missing-hashes.json` and
    `pkgs/by-name/ou/outline/package.nix`
  - both reject diagnostics showed:
    - `guardedPath=pkgs/by-name/ou`
    - `immediateChildName=outline`
    - `directChildChange=false`
    - `baseEntryKind=directory`
    - `headEntryKind=missing`
    - `observedChildrenRecorded=false`
    - `observedChild=null`
    - `rejectReason=entry-kind-changed`

Run `266`: no-stats/no-debug after adding write-time provenance and witness
metadata, same window, binary `5f875f4c...`.

- Timings:
  - `1cfdcfa...`: `18.389472327020485s`
  - `345133...`: `14.837474754021969s`
- Output mode: wall-only; `withStats=false`.
- Classification sidecar now carries directory provenance:
  - `directoryCount=1042`
  - `completeKeysetDirectoryCount=7`
  - `observedDirectoryChildrenCount=339`
  - `suppressedCompleteKeysetDirectoryCount=3`
  - `pkgs/by-name/ou`:
    `completeKeyset=false`, `observedChildrenRecorded=false`,
    `observedChildrenCount=0`, suppression count `0`
  - `pkgs/by-name/li`:
    `observedChildrenRecorded=true`, `observedChildrenCount=303`
  - `pkgs/by-name/ne`:
    `observedChildrenRecorded=true`, `observedChildrenCount=8`
- Child-output witness sidecar now has `certRef`, `proofKey`, `headRev`,
  `servingAuthority=false`, and output digest. It remains complete and
  accepted by the drv-output identity checker for the two children.
- Direct-output cert scale for `1cfdcfa...`:
  - cert: `2,232,597` bytes
  - guard: `418,102` bytes
  - semantics sidecar: `3,074,216` bytes
  - child witness: `3,124` bytes
  - cert deps: `3863` total
    - `3013` derivation-output-identity deps (`1484` unique drv paths)
    - `846` derived-store-path deps
    - `4` environment deps

Causal read:

- The `1cfd -> 345` directory reject cannot be safely relaxed with the current
  persisted facts. `outline` was not recorded as an observed child, and the
  `ou` directory was not a complete-keyset suppression case. Treating the
  deletion as ignorable would be cached absence, not positive authorization.
- The current direct-output proof capture is safe but much too broad to be the
  compact top-level-output proof we need: it captures thousands of derivation
  and derived-store-path facts for a JSON payload that ultimately contains two
  outPath strings.
- The child witness is the right size/shape for the payload, but the missing
  authority is source-to-selected-top-level-drv. The next useful shadow
  experiment should measure the dependency surface of proving the two child
  `drvPath` selections, without consuming or weakening the normal full-output
  proof capture.

### 2026-05-26 child drvPath proof lower-bound probe

I followed the counter/stats discipline for this probe: no `NIX_SHOW_STATS`, no
`NIX_SHOW_STATS_PATH`, and no debug. This was not a headline benchmark run; it
was a focused diagnostic in `/tmp/eval-trace-drvpath-probe` using a detached
nixpkgs worktree at `1cfdcfa5620e13451b5c869742dd7cc00fb02e81`.

Question:

- If the output is "just computing `outPath` on the derivation", can a compact
  command proof be found by evaluating the selected child `.drvPath` instead of
  rendering the full `{ aarch64-linux, x86_64-linux }` JSON?

Probe:

- First attempt: `closures.gnome.aarch64-linux.drvPath` with
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_DIRECT_OUTPUT_PROOF_CAPTURE=1` but
  without the parent by-name output-boundary gate. It produced the expected
  string in `9.50s`, but wrote no proof cache files. Cause: direct-output proof
  capture is gated by
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_BY_NAME_OUTPUT_BOUNDARY_RECOVERY=1`.
  This is now recorded so future probes do not accidentally measure the
  uninstrumented fallback path.
- Second attempt: same command with the parent gate enabled:
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_BY_NAME_OUTPUT_BOUNDARY_RECOVERY=1`,
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_DIRECT_OUTPUT_PROOF_CAPTURE=1`,
  observed-key/by-name directory flags enabled, no stats/debug.
  It produced:
  `"/nix/store/a0y4mram5p7bamwcwagwmr62jh1rk145-nixos-system-nixos-26.05pre708350.gfedcba.drv"`
  in `10.87s`.

Published proof shape for the single `aarch64-linux.drvPath` leaf:

- JSON payload size: `91` bytes.
- Output context: one drv root,
  `=a0y4mram5p7bamwcwagwmr62jh1rk145-nixos-system-nixos-26.05pre708350.gfedcba.drv`.
- Cert size: `1,188,003` bytes.
- Guard size: `418,009` bytes.
- Store-path availability file: `4,824` bytes.
- Deps: `2,354` total:
  - `1,505` derivation-output-identity deps,
  - `845` derived-store-path deps,
  - `4` environment deps.

Additional artifact check:

- The compact child-output witness for the full two-system JSON has two
  top-level system `.drv` paths.
- Comparing witness child drv basenames against the full direct-output cert's
  `1,484` unique derivation-output-identity drv paths found `0` overlap.
- Therefore the broad direct-output proof is not already carrying a reusable
  "source selects these top-level system drv paths" fact. It is mostly proving
  the derivation/output and source-copy facts needed while instantiating the
  system derivations.

Causal read:

- Evaluating `.drvPath` is not cheap for this workload. It instantiates the
  selected NixOS system derivation, which forces a large dependency surface.
- The two-system JSON cert's `3,013` derivation-output-identity deps are
  consistent with roughly the union of the two selected systems, not a compact
  root-selection proof.
- A nested `DepCaptureScope` inside the existing direct proof capture would be
  unsound for measurement unless merged back into the parent: recording routes
  to the current top scope, and `finalizeAndTakeDeps...()` moves deps out of
  that scope before the outer proof can see them. The existing
  `PublicationWarmupScope` in `eval.cc` demonstrates a safe merge pattern, but
  it is local implementation detail, not a reusable capture API.
- This pushes against a source-to-drv proof based on ordinary `.drvPath`
  evaluation. To beat the remaining cold tail, the next viable leads are either
  a more structural command-specific source proof that avoids derivation
  instantiation, or accepting that these guard-rejected transitions must fall
  back unless a future source/static analysis can prove the changed
  directory/by-name package facts cannot affect the selected systems.

Follow-up from parallel adversarial review:

- A reviewer agreed the `outline` directory rejection is sound and should not
  be relaxed.
- They also pointed out that the witness metadata was bound by opaque
  `proofKey`, but not standalone-auditable for the command shape. I extended
  `GitJsonActionCacheKeys` so child-output witness sidecars also record
  `filePath`, `attrPath`, and `outputShape`. This is still diagnostic-only
  (`servingAuthority=false`); it just makes artifact review less dependent on
  reconstructing the proof-key material from source.
- The same review reinforced that "wall-only" means only no `NIX_SHOW_STATS`
  and no debug. Diagnostic sidecar flags still add work on the timed path, so
  runs with `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_BY_NAME_PACKAGE_SEMANTICS_DIAGNOSTIC`
  or witness/shadow flags remain diagnostic, not headline performance claims.

Counter/stat ablation correction:

- A parallel counter review found a real stats/no-stats semantic hazard: the
  output-boundary recovery path inferred "the verifier rejected because a
  protected deferred lane changed" by comparing the global
  `nrProofJsonActionLoadDeferredLaneRejectedProtected` counter before and after
  verification.
- That counter only increments when `NIX_SHOW_STATS` enables counters, so the
  recovery path could be taken under stats and skipped without stats. This is
  unacceptable even for diagnostics because it confounds behavior with
  measurement mode.
- I replaced that control-flow dependency with an explicit local verifier
  diagnostic (`ProofJsonActionCertificateVerifyDiagnostics`); the counter
  remains telemetry only. The next same-binary invariant check should compare
  stats-on and stats-off for accept/fallback decisions, not wall time.

Harness correction after that fix:

- I initially ran a quick two-cell check through
  `benchmarks/eval-trace-bench/.venv/bin/eval-trace-bench` because
  `./result/bin/nix run .#eval-trace-bench -- --help` hung. That was the wrong
  harness for evidence. Treat run `267` as throwaway/non-authoritative.
- The repo's `native` devshell does not put `eval-trace-bench` itself on
  `PATH`, so the reproducible command I used afterward was:
  `nix develop -L --builders '' .#native -c bash -lc 'nix shell -L --builders "" .#eval-trace-bench -c eval-trace-bench ...'`.
- Rebuilt `.#nix-cli` after the counter-control-flow fix:
  `/nix/store/sbzf4smxnf0yd1w5h5fpy7c0y4j6x3r1-nix-2.35.0pre20260515_dirty`,
  sha256 `12b4f5154a25ed33b741135a85d8d30f8e38c05612605377003bac7dcdef29c8`.
- After defensively clearing the verifier out-parameter on entry, rebuilt
  again:
  `/nix/store/yi8s5jan9hgfqx5cd3z3mrmxyikmr5v4-nix-2.35.0pre20260515_dirty`,
  sha256 `356275a99005a8e23fa9b70aabdff577e4041347721abb482686810bbf597948`.

Devshell/Nix-package stats ablation:

- Run `269`, focused `v11-tail-1cfd-345`, env:
  `NIX_EVAL_TRACE_JSON_ACTION_ENABLE_OBSERVED_KEY_PROOF=1` and
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_BY_NAME_OUTPUT_BOUNDARY_RECOVERY=1`.
- `cold/269` wall-only:
  - `1cfdcfa5620e`: `19.29456987802405s`
  - `34513330fcc1`: `16.08041257801233s`
- `cold-stats/269`:
  - `1cfdcfa5620e`: `21.54801462899195s`
  - `34513330fcc1`: `18.15694578102557s`
- Both commits produced identical `eval.json` hashes across stats/no-stats:
  `a23639586dfa46f8379ab82f63e8fec0c96b2c68a79efebe89d0f5358ce23b62`.
- This run validates output parity only. It did not exercise the protected
  deferred-lane verifier path (`deferredLaneRejectedProtected=0`), so it is not
  a direct reproducer for the counter-control bug.

Targeted strict deferred-lane attempt:

- Run `270` added the known strict flags:
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_SUPPRESS_CONSTRUCTION_KEYSETS=1`,
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_RELAX_BY_NAME_DIRECTORY_LISTINGS=1`,
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_OUTPATH_DEP_FACETS=1`,
  `NIX_EVAL_TRACE_JSON_ACTION_ALLOW_UNSAFE_ABLATIONS=1`,
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_OUTPATH_DEFER_CONSTRUCTION_BY_NAME_FILEBYTES=1`,
  and
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DERIVATION_OUTPUT_FACET_PROOF=1`.
- `cold/270` wall-only:
  - `1cfdcfa5620e`: `18.81299822201254s`
  - `34513330fcc1`: `15.665679851954337s`
- `cold-stats/270`:
  - `1cfdcfa5620e`: `25.147874205955304s`
  - `34513330fcc1`: `21.819998442020733s`
- Stats/no-stats outputs matched, but the run still did not reach the protected
  deferred-lane verifier path. The second commit rejected earlier at the Git
  guard with `guardRejectDirectory=1`, `rejectedGuardClassifyRejectDirectory=1`,
  and `deferredLaneChecks=0`. This is the current sound `outline` directory
  reject, not the old v18g protected-lane recovery case.
- Causal conclusion: the counter-control fix is necessary and built, but the
  current `1cfd -> 345` focused window no longer directly exercises it because
  later directory-provenance hardening blocks first. A future reproducer must
  either use a pair where the Git guard accepts and the verifier reaches a
  protected deferred-lane reject, or construct a small fixture for that verifier
  branch.

Exact old reproducer env retry:

- Existing older artifacts with `deferredLaneRejectedProtected=1` were
  `cold-stats/90`, `cold-stats/91`, and `cold-stats/102` for the same two
  commits. `cold-stats/102` used the same strict flags as run `270` plus
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DEPENDENCY_OUTPUT_BOUNDARY_CURRENT_PACKAGE=1`.
- Run `271` reran that manifest environment through the devshell/Nix-package
  harness against the current binary.
- `cold/271` wall-only:
  - `1cfdcfa5620e`: `18.695473433006555s`
  - `34513330fcc1`: `15.314866114989854s`
- `cold-stats/271`:
  - `1cfdcfa5620e`: `25.85670939402189s`
  - `34513330fcc1`: `22.105754752003122s`
- Stats/no-stats outputs again matched.
- It still did not exercise the protected deferred-lane path:
  `guardRejected=1`, `guardRejectDirectory=1`,
  `rejectedGuardClassifyRejectDirectory=1`, `deferredLaneChecks=0`, and
  `deferredLaneRejectedProtected=0`.
- Causal read: the older protected-lane run is no longer reproducible on the
  current branch because the current publisher records a directory guard that
  rejects `pkgs/by-name/ou/outline` before certificate verification. This is a
  stricter soundness result, not a stats effect. The counter-control-flow fix
  remains needed for future accepted-guard/protected-verifier cases, but a new
  targeted reproducer is required to prove the specific branch with runtime
  artifacts.

Adversarial counter sweep:

- Searched for remaining `nr...load()` uses outside stats emission. The only
  non-`eval.cc` match is `nrOwnDepsMax.load()` in
  `dep-recording-context.hh`, used inside `if (Counter::enabled)` to update a
  diagnostic max counter.
- I did not find another proof/cache serving branch that depends on a stats
  counter value. That does not mean stats are free; it means the specific
  stats/no-stats control-flow hazard found in the output-boundary recovery path
  is not duplicated by another obvious counter-load branch.

Parallel review consolidation:

- Counter review agreed with the local sweep: after the local verifier
  diagnostic fix, remaining counter/stat effects are measurement, teardown, or
  telemetry. They still perturb timing, so stats runs stay non-headline, but I
  do not see another counter-read serving decision.
- Reproducer review found that `1cfd -> 345` is the wrong current-branch
  verifier reproducer. Current hardening rejects before verifier entry because
  deleted `pkgs/by-name/ou/outline` changes the directory entry kind under an
  unobserved by-name child. That is sound and blocks the old v18g path.
- Better focused candidates are single regular-file package modifications:
  - `1cfdcfa5620e13451b5c869742dd7cc00fb02e81 ->
    d92a66198f5846a229bf6828145a2848d35a1d06`
    (`pkgs/by-name/li/libvpx/package.nix` only),
  - `1cfdcfa5620e13451b5c869742dd7cc00fb02e81 ->
    ea3aaaa2908a8cf15bf497e92202a0da2e311340`
    (`pkgs/by-name/ne/newt/package.nix` only).
  I added tracked commit lists:
  `benchmarks/eval-trace-bench/commit-lists/protected-lane-libvpx.txt` and
  `benchmarks/eval-trace-bench/commit-lists/protected-lane-newt.txt`.
- Design review argued against more `.drvPath` proof work. The lower-bound
  probe already showed a single leaf `.drvPath` proof is broad and slow. The
  next defensible lead is phase-separated source-to-child-selection authority:
  prove "this command/source/route selected these child names" before child
  derivation/outPath forcing, then keep child-output DOI/witness facts separate.

Phase-separated selection diagnostic:

- Added diagnostic-only `selectedChildNames` and `selectionDeps` to
  `TraceSession::JsonOutputProjection`.
- In `tryPublishOutPathObject`, `selectionDeps` is the dependency vector after
  navigation and target attrset capture, before child derivation/outPath forcing
  appends child construction deps. This is the first direct measurement of the
  prospective source-to-child-selection authority surface.
- The by-name semantics diagnostic sidecar now emits a `selection` object:
  selected child names, selection dep count, selection by-name package-file
  deps, and selection derived-store-path deps. It is still
  `servingAuthority=false`; no verifier rule consumes it.
- Falsification criteria:
  - if `selectionDeps` is already near `.drvPath` proof scale, the idea is not
    compact enough;
  - if `selectionDeps` still contains `outline`/unobserved directory facts for
    the current target, selection authority cannot bypass the sound guard;
  - if child names/fragments change under a supposedly irrelevant by-name file
    modification, this cannot become serving authority.
- Rebuilt after adding the diagnostic:
  `/nix/store/x1bf2xjcd7plxd9afxk6j89d5cyjfdz9-nix-2.35.0pre20260515_dirty`,
  sha256 `f7008a2ab0ad19171b134be7780b793e38fc4fc42f54363933af38fa6852d53d`.

Harness correction follow-up:

- The first protected-lane candidate lists were anchored at `1cfdcfa5620e`
  instead of the direct parents of the candidate commits. That made the
  `1cfd -> d92a` diagnostic run compare across a large accumulated diff rather
  than the intended single-file parent transition, so it is not evidence for
  the protected-lane reproducer.
- Fixed the tracked commit lists to use the actual direct-parent pairs:
  - `a8243adb3c38046c7cd5f9513d41096f29a9df7e ->
    d92a66198f5846a229bf6828145a2848d35a1d06` for the one-file
    `pkgs/by-name/li/libvpx/package.nix` change.
  - `82398cb2f583a36283d99dd4d82f2598263f5897 ->
    ea3aaaa2908a8cf15bf497e92202a0da2e311340` for the one-file
    `pkgs/by-name/ne/newt/package.nix` change.
- The useful result from the bad run is limited to the diagnostic shape of the
  selection surface on that first commit: `selection.childNames` was
  `["aarch64-linux", "x86_64-linux"]`, `selection.depCount` was `5`, and the
  selection deps had no by-name package-file deps or derived store-path deps.
  That supports continuing the phase-separated selection probe, but it does not
  validate protected-lane recovery.

Corrected libvpx protected-lane diagnostic:

- Run `273` used the corrected direct-parent libvpx pair through the
  devshell/Nix-package harness, not the virtualenv:
  `a8243adb3c38046c7cd5f9513d41096f29a9df7e ->
  d92a66198f5846a229bf6828145a2848d35a1d06`.
- Stats run:
  - `cold-stats/273` first commit: `25.789631860970985s`.
  - `cold-stats/273` second commit: `8.161544290021993s`.
  - Second-commit counters:
    `hits=1`, `guardAccepted=1`, `guardRejected=0`,
    `deferredLaneChecks=1`, `deferredLaneRejectedProtected=1`,
    `outputBoundaryRecoveryAttempts=1`,
    `outputBoundaryRecoveryHits=1`, `load.totalUs=7682997`,
    `load.verifyUs=7543340`.
- Stats-off run with the same diagnostic flags:
  - `cold/273` first commit: `19.471360701019876s`.
  - `cold/273` second commit: `7.720706799998879s`.
  - `eval.json` SHA-256 matched stats-on and stats-off for both commits:
    `9949c7337b965ddbad7a7c0c492ec517d9ec29c737710329ce736e0b99d0bc77`.
- This finally reproduces the protected deferred-lane verifier branch on the
  current code. It also confirms the current recovery is semantically useful
  but too slow: the second commit is a proof hit only after about `7.5s` of
  verification/recovery work under stats.
- The by-name rejection diagnostic says the changed file is exactly
  `pkgs/by-name/li/libvpx/package.nix`; cached and current outputs are
  identical, but the shadow by-name package boundary would not serve because
  the aggregate proof has no accepted observation for that package file.
- The selection diagnostic is the promising contrast:
  `selection.childNames=["aarch64-linux","x86_64-linux"]`,
  `selection.depCount=5`, `selection.byNamePackageFileBytes=[]`, and
  `selection.derivedStorePathDeps=[]`, while the aggregate direct-output proof
  has `40203` deps, `20` by-name package-file deps, and `846` derived
  store-path deps. This is the strongest evidence so far that the expensive
  part is child/output forcing, not source-to-child selection.

Corrected newt protected-lane diagnostic:

- Run `274` repeated the same devshell/Nix-package diagnostic on the corrected
  direct-parent newt pair:
  `82398cb2f583a36283d99dd4d82f2598263f5897 ->
  ea3aaaa2908a8cf15bf497e92202a0da2e311340`.
- Stats run:
  - `cold-stats/274` first commit: `25.794167161977384s`.
  - `cold-stats/274` second commit: `8.200231149967294s`.
  - Second-commit counters:
    `hits=1`, `guardAccepted=1`, `guardRejected=0`,
    `deferredLaneChecks=1`, `deferredLaneRejectedProtected=1`,
    `outputBoundaryRecoveryAttempts=1`,
    `outputBoundaryRecoveryHits=1`, `load.totalUs=7666067`,
    `load.verifyUs=7541736`.
- Stats-off run with the same diagnostic flags:
  - `cold/274` first commit: `18.96634391101543s`.
  - `cold/274` second commit: `7.207563108007889s`.
  - `eval.json` SHA-256 matched stats-on and stats-off for both commits:
    `9949c7337b965ddbad7a7c0c492ec517d9ec29c737710329ce736e0b99d0bc77`.
- The rejection and selection diagnostics match the libvpx pattern:
  the changed file is `pkgs/by-name/ne/newt/package.nix`, output-boundary
  recovery proves the cached/current JSON are equal, but the shadow package
  boundary would not serve because there is no accepted package-file
  observation. Selection remains compact:
  `selection.depCount=5`, no by-name package-file deps, no derived store-path
  deps, and the same two selected systems.

Current causal model:

- Package-file changes like `libvpx` and `newt` are not part of source-to-child
  selection for this command. They enter only when forcing child derivation
  outputs.
- The current strict verifier is soundly conservative: it rejects the cached
  output because a protected deferred construction dependency changed.
- The output-boundary recovery then re-runs the expensive child/output path and
  discovers the top-level JSON/context did not change. That proves the
  opportunity but is not a usable fast path.
- A better implementation needs a first-class, compact authority for the
  phase boundary: "this source/root/attr route selects these child names and
  these cached child output identities are still available", without claiming
  unchanged package files are irrelevant to arbitrary child construction.

Existing trace-reuse recovery ablation:

- Run `275` added
  `NIX_EVAL_TRACE_JSON_ACTION_EXPERIMENT_OUTPUT_BOUNDARY_TRACE_REUSE=1` to the
  corrected libvpx wall-only diagnostic env.
- `cold/275` first commit: `18.86806005402468s`.
- `cold/275` second commit: `8.312950610998087s`.
- Outputs matched run `273`, but the rejection diagnostic still reported
  `accepted-by-deferred-lane-output-boundary-recovery`, not trace reuse.
- Conclusion: the existing trace-reuse knob is not the missing fast path for
  this case. It either cannot recover from the current cache state or costs at
  least as much as raw output-boundary recovery. Do not spend more time on that
  route unless a code read finds a simple wiring bug.

Unsafe current-package output-boundary lower bound:

- Runs `276`/`277` enabled the existing
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DEPENDENCY_OUTPUT_BOUNDARY_CURRENT_PACKAGE=1`
  path on the corrected direct-parent package-file pairs.
- Libvpx:
  - `cold-stats/276` second commit: `1.0857541029690765s`.
  - `cold/276` second commit: `0.9742513389792293s`.
  - Stats counters showed ordinary verifier acceptance, not recovery:
    `deferredLaneAccepted=1`, `deferredLaneAcceptedChanged=1`,
    `deferredLaneRejectedProtected=0`,
    `dependencyOutputBoundaryAttempts=1`,
    `dependencyOutputBoundaryHits=1`,
    `outputBoundaryRecoveryAttempts=0`.
- Newt:
  - `cold-stats/277` second commit: `1.013789609016385s`.
  - `cold/277` second commit: `0.9240994190331548s`.
- All eval JSON hashes matched the strict recovery runs:
  `9949c7337b965ddbad7a7c0c492ec517d9ec29c737710329ce736e0b99d0bc77`.
- Causal read: this is the current best lower bound for a fast package-file
  boundary proof. It is still intentionally unsafe as general serving authority:
  it checks changed package drv/output identity against old owner derivations,
  but it does not prove the command's current source-to-child route plus child
  identity boundary in a standalone, auditable way.

Cleanup pass:

- Removed the stray untracked `benchmarks/eval-trace-bench/uv.lock`. Benchmark
  evidence should continue to use the Nix devshell/Nix package harness.
- Refactored duplicated output-boundary recovery logic in
  `tryLoadProofJsonActionCache` into one local helper. This keeps the existing
  guard-reject and deferred-lane outcomes/counters but removes two copies of
  store-dependency checking, recovery timing, cached/current output comparison,
  diagnostic sidecar writing, and final clean-head serving checks.
- Added small helper predicates for repeated experiment/unsafe env gating:
  observed-key experiment gates, output-boundary experiment gates, and unsafe
  ablation gates.
- Verification after cleanup:
  - `git diff --check` passed.
  - `nix build -L --builders '' .#nix-cli` passed after both refactors.
  - Run `278` repeated the corrected libvpx strict recovery smoke after the
    recovery refactor: first commit `19.030771851015743s`, second commit
    `7.329309854016174s`, eval JSON hashes matched run `273`, and the
    diagnostic outcome remained
    `accepted-by-deferred-lane-output-boundary-recovery`.

Follow-up cleanup:

- Split proof JSON action cache flag/source-identity support out of
  `src/libcmd/installable-attr-path.cc` into
  `src/libcmd/proof-json-action-cache.{hh,cc}`. This removes a large block of
  environment-gating and authority-mode helpers from the installable traversal
  implementation without changing the flag names, cache-version selection, or
  authority-mode string.
- Added the new source to `src/libcmd/meson.build` and staged the new files so
  Nix's git-backed source filter includes them.
- Verification:
  - `git diff --check` passed.
  - `nix build -L --builders '' .#nix-cli` passed, producing
    `/nix/store/gj0sni1af53h42l1ff2lddq642div8dz-nix-2.35.0pre20260515_dirty`.

Additional deduplication pass:

- Factored store-path availability sidecar/text decoding into shared helpers:
  - top-level certificate availability refs/text now go through one path for
    both direct store-path-dep collection and full certificate loading;
  - accepted-result sidecar availability refs reuse the same sidecar ref
    decoder, while preserving the old load-counter behavior.
- Reused `gitHeadRevision()` in the clean-head recheck instead of maintaining a
  second copy of `git rev-parse --verify HEAD` parsing/trimming.
- Added one local hash-algorithm-tag helper and replaced repeated
  `evalTraceHashAlgorithmTag(getEvalTraceHashAlgorithm())` string construction
  in cache-key material, certificate headers, sidecars, and validators.
- Factored the duplicated by-name semantics diagnostic identity check used by
  classification and rejection diagnostics.
- Verification:
  - `git diff --check` passed.
  - `nix build -L --builders '' .#nix-cli` passed, producing
    `/nix/store/4mi8gz4pndaz9ii9f0iy006svbb26pjg-nix-2.35.0pre20260515_dirty`.

Private support-unit cleanup:

- Split proof JSON action cache key/path construction out of
  `src/libcmd/installable-attr-path.cc` into
  `src/libcmd/proof-json-action-cache-keys.{hh,cc}`. This moves the git repo
  discovery, clean/head revision checks, length-framed key material, child
  proof-key derivation, cache sidecar paths, certificate filename checks, and
  repo-relative path helper behind one private boundary.
- Split repeated diagnostic document primitives into
  `src/libcmd/proof-json-action-diagnostics.{hh,cc}`:
  byte/file/certificate refs, SHA-256 byte hashing, authority metadata, and
  command metadata are now built in one place.
- Collapsed duplicated cached by-name semantics sidecar attachment in
  classification and rejection diagnostics into a shared local helper. The
  helper still performs the same certificate/semantic sidecar match before
  adding the cached diagnostic reference and leaves each caller's verdict logic
  explicit.
- Verification:
  - `git diff --check` passed.
  - `nix build -L --builders '' .#nix-cli` passed, producing
    `/nix/store/hkmb4kr0wj61gp2618m5pfsvxv982gg6-nix-2.35.0pre20260515_dirty`.

Diff inventory and reduction checkpoint:

- Current `src` diff is much too large for review: `131` files,
  `39659` insertions, `1968` deletions. By area:
  - `libcmd`: `11166` insertions, `103` deletions.
  - `libexpr`: `24476` insertions, `1604` deletions.
  - `libexpr-tests`: `3897` insertions, `246` deletions.
  - other `src`: `120` insertions, `15` deletions.
- The largest single problem remains
  `src/libcmd/installable-attr-path.cc`: `HEAD` has `443` lines, current tree
  has `9866` lines, and the file accounts for `9502` added lines in the diff.
  It now hosts cache serving, git guards, scoped-source proof, by-name/output
  boundary recovery, store-path sidecars, exact-source proof capture, witness
  shadows, unsafe ablations, and diagnostics. It should be orchestration only.
- Spark subagent inventory had to be range-scoped; whole-file/document tasks
  exceeded context, and later Spark quota was exhausted. Useful completed
  reports:
  - libcmd inventory: keep the core JSON action cache load/store/verify/render
    path, but move guard logic, deferred-lane logic, witness shadow logic,
    proof-capture rendering, and strategy branching out of
    `installable-attr-path.cc`.
  - test inventory: most touched tests still cover active soundness boundaries,
    but helper and serialization tests should be split; there are still gaps
    around end-to-end missing store dependency fallback, serving-path proof
    descriptor verification, generation-store integration, and by-name/output
    boundary semantics.
- Local inventory found about `42` proof-action env knobs still present and a
  very large proof-action counter surface (`442` `nrProofJsonAction*` symbols
  across declarations/definitions). This is too much for a reviewable branch
  and can perturb benchmark runs when stats are enabled.

Relevance map:

- Keep as the evidence-backed path:
  - aggregate proof JSON action cache entries;
  - exact-head/cross-head alias routing as routing only, with original certs
    and sidecars as authority;
  - scoped-source proof serving only if it remains the selected authority for
    accepted-result sidecars;
  - store-path availability checks and warning/fallback behavior for missing
    required store dependencies;
  - proof descriptor/canonical hash/fail-closed decoding primitives that are
    used by exact persistent authority.
- Keep, but isolate before review:
  - git guard parsing/coverage and by-name path classification;
  - deferred dependency lane parsing and strict protected-lane rejection;
  - accepted-result sidecar encode/decode/verify;
  - direct render/proof-capture helpers;
  - exact replay/proof descriptor and generation file-format code if the branch
    is still trying to land immutable exact-session authority as part of this
    work.
- Needs a product decision:
  - generation-format/generation-store/git generation integration. These are
    plausible immutable authority infrastructure, but they are not necessary for
    the narrow JSON action-cache hot-path prototype. Keeping them means this is
    a broader exact-session persistence rewrite, not just an outPath cache
    prototype.
  - child-output witness shadow. Useful as telemetry for a future
    source-to-child-selection proof, but not serving authority and not justified
    as review-surface in the current prototype.
  - exact-source direct proof/proof-capture path. It may be the right direction
    for a compact proof, but previous `.drvPath` capture was too broad; keep
    only the minimum needed for the next selection-boundary experiment.
- Delete or quarantine before review:
  - unsafe by-name semantic recovery oracle;
  - unsafe current-package dependency-output-boundary serving ablation;
  - unsafe outPath publication skips (`trace activation`, `shape deps`,
    `disable trace ctx`);
  - stale output-boundary trace-reuse experiment, unless a fresh code read finds
    a concrete wiring bug;
  - child fragment publication by default; the ablation showed it was not causal
    for the current win and increased proof surface;
  - most proof-action micro-counters after their diagnostic purpose is recorded.

Reduction order:

1. Choose branch scope explicitly: narrow JSON action cache prototype, or broad
   immutable exact-session/generation rewrite. Do not keep both intertwined.
2. Remove/quarantine unsafe and falsified ablation paths first. This should
   reduce env gates, authority-mode variants, counters, and review ambiguity.
3. Split `installable-attr-path.cc` into private support units:
   guard/path proof, deferred lanes, store-path availability sidecars,
   accepted-result sidecars, proof capture rendering, and load strategy.
4. Shrink counters to a small stable set for default builds; keep detailed
   counters behind a debug-only or temporary local mechanism so benchmark
   numbers are not distorted.
5. Re-run build and then one wall-only smoke benchmark after each deletion
   batch. Only reintroduce a removed experiment if the benchmark falsifies the
   causal read.

5.5/xhigh deletion-oriented review:

- Spawned six targeted 5.5/xhigh read-only agents:
  - libcmd proof JSON action cache surface;
  - env gates and counters;
  - trace-session/projection/outPath/deps integration;
  - store/proof/generation infrastructure;
  - eval-trace tests;
  - adversarial architecture review.
- All six completed and agreed on the main architectural problem: the branch
  currently mixes a reviewable JSON action-cache prototype with a broad
  exact-session/generation storage rewrite and multiple lower-bound oracles.
  The cleanup should delete or quarantine experiments before doing more
  extraction.

Highest-confidence deletes/quarantines:

- Delete unsafe package-file drop oracles:
  `dropV11TailPackageFileBytesAblationPath`,
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DROP_V11_TAIL_PACKAGE_FILEBYTES`,
  and `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_DROP_BY_NAME_PACKAGE_FILEBYTES`.
  These were measurement oracles and explicitly not proof rules.
- Delete unsafe clean-status oracles:
  `proofJsonActionUnsafeUnkeyedSkipCleanKeyStatusOracleEnabled`,
  `NIX_EVAL_TRACE_JSON_ACTION_UNSAFE_UNKEYED_SKIP_CLEAN_KEY_STATUS`, and likely
  the unsafe keyed skip-clean-status ablation. The unkeyed version is especially
  bad because it changes key construction without being represented in
  `proofJsonActionAuthorityMode()`.
- Remove stale ignored env names that no longer drive behavior:
  `NIX_EVAL_TRACE_JSON_ACTION_CONTEXT_STORE_PROOF`,
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_OUTPATH_SKIP_TRACE_ACTIVATION`, and
  `NIX_EVAL_TRACE_JSON_ACTION_ABLATION_OUTPATH_SKIP_SHAPE_DEPS`.
- Delete unsafe outPath publication skips from `trace-session.cc`. Safe
  aggregate outPath publication still needs `TraceActivationScope` and shape
  deps; skip branches only complicate the production path.
- Delete `ChildOutputWitnessShadow` and the
  `.child-output-witness-shadow.json` sidecar unless actively consumed by an
  external workflow. It is diagnostic-only and has no serving reader.
- Delete unsafe by-name semantic recovery oracle. It is explicitly a lower-bound
  oracle and should not remain as a runtime serving switch.
- Delete unsafe deferred dependency lanes if the product path is safe JSON
  action cache. This removes `unsafeDeferredDependencyLanes`,
  `unsafe-serving-ablation-v1`, DOI facet proof serving exceptions, deferred
  missing-drv input-closure ablations, and current-package dependency-output
  serving.
- Delete exact-head fast memo. It bypasses the certificate-backed path and
  overlaps with normal proof cache/alias behavior.
- Delete or quarantine direct-output memo/render/proof-capture policy knobs
  that only steer benchmarks. If exact-source proof capture remains, keep one
  binary experiment gate, not min-candidates/min-rejects/refresh-stride knobs.
- Delete output-boundary trace-reuse unless a targeted code read finds a wiring
  bug. Run `275` did not improve the strict recovery case.
- Keep child fragment publication disabled by default, and remove it from the
  review branch if this is the minimal JSON action-cache path. The ablation
  showed it was not causal for the win and increased proof surface.
- If the branch scope is narrow JSON action cache, delete or quarantine
  `generation-store.{cc,hh}`, `generation-format.{cc,hh}`, git generation
  export/import, `PersistentExactGenerationHit`,
  `PersistentExactGenerationExport`, exact-hit sidecars/fixed-row indexes, and
  descriptor synthesis/export from `SqliteTraceStorage`. The store review found
  generation export still enabled despite local comments saying it is the wrong
  authority boundary, and general durable replay is not fully wired.

Simplify/generalize:

- Collapse the proof-action env matrix into one internal authority enum and one
  debug selector. Current code has about `41` proof-action env names in code
  and a feature-vector `proofJsonActionAuthorityMode()` that is easy to
  desynchronize. Suggested default surface:
  - no env needed for default behavior;
  - optional kill switch;
  - optional debug selector such as
    `NIX_EVAL_TRACE_JSON_ACTION_DEBUG=semantics,child-witness,source-shadow`.
- Replace the manual `proofJsonActionRoutingIgnoredEnvVar()` list with a single
  feature/env table. Behavior-changing envs must either be in the authority
  mode or not be ignored.
- Replace hundreds of micro-counters with structured enum telemetry:
  guard reject reason, certificate reject reason, source-proof outcome,
  deferred-lane outcome, plus a small stable set of attempts/hits/misses and
  coarse timings. The current `nrProofJsonAction*` surface is too broad for
  default stats and can perturb benchmark runs.
- Unify content refs for cert refs, byte refs, accepted-result refs,
  store-path sidecar refs, and diagnostic refs. The current bespoke JSON
  reference plumbing is still spread across multiple helpers.
- Unify store-path dependency validation. Cert verification, accepted-result
  sidecar verification, and output-context checks all do variants of
  collect-root/query-valid/missing-path logic.
- Unify observed-key guard acceptance. The verifier has a manual guard loop
  that overlaps `proofJsonActionNixObservedKeyGuardsAccept()`.
- Unify by-name package path parsing. `trace-session.cc` and
  `input-resolution.cc` have overlapping parsers with different strictness.
- Remove the unused single-step authorized projection route API if no external
  caller exists. Current in-tree callers use chain acceptance.
- Delete disabled child `drv-output` trace-row publication guarded by
  `constexpr false`; it is dead code unless old cache compatibility is a goal.

Reviewable split:

1. Minimal safe JSON action cache:
   aggregate certs, alias routing, source-cert-authorized accepted-result
   sidecars if kept, store-path availability checks, strict fallback, and small
   stable telemetry. This should leave `installable-attr-path.cc` as a thin
   call site.
2. Diagnostics-only branch:
   by-name classification/rejection diagnostics, selection diagnostics,
   child-output witness shadow, and boundary classifiers. No serving authority
   and no benchmark headline claims.
3. Phase-separated selection proof:
   source-to-child selection authority plus separate child drv/output identity
   authority. This is the plausible path from the `selectionDeps=5` evidence in
   runs `273`/`274`; do not reuse the unsafe current-package shortcut.
4. Exact-session/generation storage:
   proof descriptors, generation packs/indexes, exact replay, and persistent
   exact-session authority. This is a separate storage rewrite and should not
   ride along with the JSON action-cache prototype unless the branch scope is
   intentionally broadened.

Test cleanup after code decisions:

- If proof descriptors/generation export are removed or quarantined, delete or
  move `store/proof-descriptor.cc`, `store/generation-store.cc`,
  `store/candidate-descriptor-snapshot.cc`, and related test-only helpers.
- If exact-session preflight/probe serving is removed, delete the exact-session
  preflight tests and direct-serving result-codec tests tied to that path.
- If store-path availability remains, keep canonical key round-trip and exact
  proof coverage tests; otherwise delete the store-path availability suite and
  record/verify row.
- Merge duplicates:
  - fold `PrimaryCacheServedOnly_RejectsHistoryBootstrap` into the OR-5
    integration test;
  - delete `StorePathAvailability_RecordVerify_WarmHit` if the canonical and
    all-query-kind warm-hit tests remain;
  - keep either single-repo or multi-repo stale GitIdentity recovery unless
    multi-repo source identity is an explicit product branch.
- Parameterize repetitive malformed descriptor/generation/serialization tests
  and share descriptor fixture builders between proof-descriptor and
  generation-store tests if those files stay.

## 2026-05-27: `src` Diff Reduction Pass

Starting point after removing the inactive proof-descriptor/exact-replay layer:
staged `src` diff was `107 files changed, 11077 insertions(+), 2976
deletions(-)`.

Cleanup performed:

- Removed the duplicated full-capsule/deps-only parser bodies in
  `trace-capsule.cc`. Both public APIs now share one internal dependency
  decoder; full decode only asks it to materialize the result payload.
- Simplified result-payload parsing through one helper and trimmed stale
  capsule/header comments.
- Collapsed `Verifier::verifyAttrImpl` onto the store-owned
  `SqliteTraceStorage::verify` path. This removed the second single-attr
  verifier state machine; `verifyAttrNoRecovery` is preserved by adding an
  `allowRecovery` flag to the store verifier.
- Removed phase timing counters that only existed for the deleted async
  duplicate (`currentLookupUs`, `historyLookupUs`, `loadFullTraceUs`,
  `volatileScanUs`, `verifyTraceCallUs`, `recoveryCallUs`, `resultDecodeUs`).
  This also avoids reporting zero-valued stale stats after consolidation.
- Trimmed embedded research/reference comments in verifier, SQLite storage, and
  capsule headers down to local invariants.

Current staged `src` diff after this pass:
`107 files changed, 10350 insertions(+), 3250 deletions(-)`.

Verification:

- `nix develop .#native-clangStdenv -c meson compile -C build nixexpr
  nix-expr-tests` passed after the code refactors.
- `nix develop .#native-clangStdenv -c meson test -C build nix-expr-tests
  --print-errorlogs --timeout-multiplier 10` passed: `1872` passed, `3`
  skipped.

Why this is causal:

- The capsule parser reduction removes parallel implementations of the same
  wire walk, so future semantic changes have one parser authority instead of
  two.
- The verifier reduction removes duplicated control flow between the async
  single-attr path and the store-owned batch/sync path. That reduces code size
  and should reduce scheduler overhead, but it needs a hot/cold benchmark before
  claiming a runtime win.

Follow-up cleanup after reviewing the still-large `src` diff:

- Deleted the transitional `TraceBackendStorage` adapter. It only forwarded to
  `SqliteTraceStorage`, while the verifier still required the SQLite concrete
  type, so it was an abstraction without a backend boundary.
- Removed attr-key call-site telemetry (`std::map`, mutex, rendered source
  positions) and kept only aggregate counters. This avoids carrying a custom
  profiler in the shape-dep path.
- Removed exact-session git-recoverability rejection counters that were wired to
  optional parameters never passed by callers.
- Collapsed per-kind exact-session “uncovered” and attr-key by-reason counters
  into aggregates.
- Removed an unused `TraceSession::withActiveSession` helper and trimmed one
  stale root-load explanatory comment.

Current `src` diff after the follow-up cleanup:
`107 files changed, 9624 insertions(+), 3216 deletions(-)`.

Additional verification:

- `nix develop .#native-clangStdenv -c meson compile -C build nixexpr
  nix-expr-tests` passed after the follow-up cleanup.
- `nix develop .#native-clangStdenv -c meson test -C build nix-expr-tests
  --print-errorlogs --timeout-multiplier 10` passed: `1872` passed, `3`
  skipped.

Why this is causal:

- The adapter deletion reduces object surface without changing storage
  semantics.
- The counter cleanup removes diagnostics that were either dead or too detailed
  for normal benchmark runs. It preserves the aggregate signals needed to tell
  whether exact-session proof, preflight, trace loading, and materialization are
  moving, while reducing code and the risk of stats-enabled runs distorting the
  hot path.

## 2026-05-27 Origin Comparison After Cleanup

The branch was rebuilt and benchmarked again after the cleanup pass. This time
the comparison was against the actual upstream/origin branch, not the stale
dirty `result` symlink that was already present in the checkout.

Setup:

- Origin subject: `origin/vibe-coding/file-based-eval-cache` at
  `92a3df1ab706cf514357ae57b367050b373ff146`, built with
  `nix build -L --builders '' ".?rev=92a3df1ab706cf514357ae57b367050b373ff146"`.
- Origin run: `cold/1`, `hot/1`.
- Current subject: dirty current worktree, built with
  `nix build -L --builders '' .#nix-cli` after the broader `nix build .`
  produced the current executable but failed later in unrelated flakes
  functional tests.
- Current run: `cold/2`, `hot/2`.
- Commit window: same 10 nixpkgs commits as the existing reference manifest,
  anchored at `7d7616894ec49a9ef1959295dd0be90f46f2c5b6`.
- Measurement: no debug, no `NIX_SHOW_STATS`, empty benchmark environment.

Results from `eval-trace-bench runs --runs reference,cold/1,hot/1,cold/2,hot/2
--reference reference --allow-provenance-mismatch`:

- All outputs matched `reference`.
- Origin `cold/1`: mean wall `5.93s`.
- Current `cold/2`: mean wall `13.42s`.
- Origin `hot/1`: mean wall `0.89s`.
- Current `hot/2`: mean wall `1.69s`.

Pairwise result:

- `cold/2` was slower than `cold/1` on all 10 commits, total delta `+74.85s`.
  The most important signal is that origin has several cold commits around
  `0.89-0.90s`, while current turns those same commits into `12.9-13.7s`
  misses. This is a lost fast-path/recovery-precision issue, not noise.
- `hot/2` was slower than `hot/1` on all 10 commits, total delta `+7.99s`,
  mean ratio `1.899x`.

Interpretation:

- The cleanup did not make the branch competitive with origin. The current
  implementation is sound on this 10-commit window but materially slower.
- The current cold profile is especially bad because it loses origin's near-hit
  cold behavior for commits that should reuse earlier state. The next
  investigation should compare run-state artifacts/DB rows and verifier
  outcomes for a fast origin commit such as `53443ee9cdff...` or
  `fa143ab863d...` against the corresponding current miss.
- The broad `nix build .` failure should not be conflated with benchmark
  soundness or hot/cold performance: it happened after the current executable
  was built, in flakes functional tests (`check`, `show`, `flakes`). It still
  needs separate triage before this branch is ready.

Additional artifact inspection:

- Origin `cold/1` cache state is about `343M` total, with a `19M`
  `eval-trace-v25-blake3.sqlite` and a separate `attr-vocab.sqlite`.
- Current `cold/2` cache state is about `602M` total, with a `261M`
  `eval-trace-v53-blake3.sqlite`.
- Current `TraceCapsules` contains `61` rows totaling `259,410,106` payload
  bytes. The large rows are repeatedly attached to:
  `closures.gnome.{aarch64-linux,x86_64-linux}` and their `.outPath` leaves.
- Per path, current records `10` candidate events for each arch/leaf path, but
  only `3` distinct result hashes, `5` distinct dep-set hashes, and `10`
  distinct full/payload hashes. That means the current schema is preserving a
  much more source-specific full capsule than the workload needs for repeated
  result serving, and it is not translating the repeated result/dep-set facts
  into cold near-hits.
- Current recovery scopes do exist for the arch/leaf paths, but the candidate
  origins are still `fresh-evaluation` for those large payload rows. In other
  words, the data was recorded, but the cold run did not serve from it.

Updated causal hypothesis:

- Cold regression: recovery candidate routing/acceptance is too precise or too
  expensive in the v53 capsule model, so commits that origin serves as near-hits
  become fresh evaluations.
- Hot regression: exact-session hits still have to load/decode large capsule
  payloads for attr-path leaves. Origin's old path keeps hot around `0.89s`;
  current hot around `1.69s` is consistent with repeated large-payload work.
- This is a design problem, not just LOC. Simplifying the implementation should
  prioritize removing the large full-capsule requirement from common positive
  navigation/result-serving paths, or restoring a compact result/dependency
  authority that can serve the repeated `outPath` facts without inflating every
  source-specific capsule.
