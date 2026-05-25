# Inventory work — running status

This file is the project ledger. It records what's queued, in flight,
blocked, and pending across the catalog work. Update it whenever a
phase advances, a verification lands, or a follow-up is queued.

The other entry points:
- [`HANDOFF.md`](HANDOFF.md) — single entry point for an agent
  picking up the work without prior context (build invocation,
  worktree-source gotcha, push policy, common pitfalls).
- [`INVENTORY.md`](INVENTORY.md) — codebase navigation map.
- [`FOLLOWUPS.md`](FOLLOWUPS.md) — current-round follow-up
  ledger. Active item-by-item checklist of correctness amends,
  doc-branch corrections, in-source doc fixes, missing
  release-note entries, new catalog candidates, and
  compound-win cleanups raised by the holistic-review pass.
  Strike items off as they land.
- [`candidates/README.md`](candidates/README.md) — catalog scope,
  eight-debt-shape framing, evidentiary standard.
- [`review/00-INDEX.md`](review/00-INDEX.md) — index of every review
  report.
- [`review/AGENT-CHARTER.md`](review/AGENT-CHARTER.md) — operational
  rules for any agent working on the catalog.

## Currently in flight

- **Holistic-review follow-up batches** — seven batches of work
  raised by the May 2026 whole-branch reviewer passes; full
  checklist in [`FOLLOWUPS.md`](FOLLOWUPS.md). Sequence: A
  (correctness amends) → B (doc-branch corrections) → C
  (in-source docs + rl-next) → D (file new catalog
  candidates #227-#233) → E (implementation) → F (clang-tidy +
  boost-flat audit) → G (adversarial value-review gaps; revert
  libutil #163, add libstore #80 + libmain `getIntArg` rl-next
  entries, capture #97 latent-bug fixes). All seven batches
  closed.

## Queued / blocked

- **#138 implementation** — relocate `EvalSettings::readOnlyMode` to
  `Store`/`StoreConfig`. V1's release-note caveat documented in #138's
  body: deleting the dead `nix::ref<bool> readOnlyMode` field in
  `nix_eval_state_builder` is a layout change to the publicly-installed
  `nix_api_expr_internal.h`, must be called out in release notes. The
  fix folds in the F1+V3 confirmed UAF repair (the dangling pointer in
  `EvalSettings::readOnlyMode` after `nix_eval_state_builder_free` goes
  away when the field is removed entirely).
- **#218 implementation** — `Hash::dummy`/`StorePath::dummy` placeholder
  pattern (new candidate added during integration). Two halves:
  placeholder-then-overwrite sites (6, in `local-store.cc`,
  `derivation-trampoline-goal.cc`, `make-content-addressed.cc`,
  `unix/build/derivation-builder.cc`, two FIXME-tagged sites in
  `nar-info.cc`) → migrate to `std::optional<Hash>` /
  `std::optional<StorePath>`; wire-marker sites (3, in `worker-protocol.cc`,
  `serve-protocol.cc`, `legacy-ssh-store.cc`) → rename sentinel and
  coordinate the protocol header. Aggregate effort medium because the
  wire-marker half touches a wire-format-adjacent header.
- **E1 implementation** — `impureOutputHash` deletion. Lands as an
  additional commit on the long-lived `vibe-coding/cleanup/libstore`
  branch. E1 cleared the prerequisites: internal linkage (verified via `nm -m`
  on the built dylib); zero in-tree readers; zero references in lix
  or Hydra; commit `50912d02e` (2022-03-31) intended full removal but
  missed the namespace-scope definition in `derivations.cc`. Pure
  deletion as a standalone PR — no deprecation cycle, no downstream
  coordination.
- **E5 verifiable-counts manifest** — implement `doc/inventory/counts.toml`
  + `verify-catalog-counts.sh` per E5's specification. Wire to weekly
  cron + manual pre-PR run. Open questions in E5 (manifest location,
  new-claim onboarding policy, file-rename handling) need a decision
  before scripting begins.
- **Cluster-G uplift series U10 (`boost::describe`)** — flagged as the
  highest-leverage middle-of-graph step in `candidates/README.md`'s
  uplift series. Not yet scheduled. Unblocks N16, N32, N35, N38, N39,
  plus the existing #9, #176, #182.
- **E3 implementation** — write the `Goal::Co` property test pinning
  O(1) frame depth, before any partial #160 (`boost::asio::awaitable`)
  migration. Specification ready in `review/E3-goal-co-property-test.md`.

## Recently completed

(Most recent first; truncate after a dozen entries.)

- **Adversarial whole-branch value review + Batch G gap-closure** (May 2026)
  — ran a parallel adversarial value-review (one reviewer per shard,
  seven total) asking whether each commit meaningfully helps or is
  effectively churn, whether semantics are preserved, and whether
  follow-on simplifications are unlocked. Aggregate verdict across
  the seven shards was **substantive value, not churn**, with one
  actual churn case (libutil #163's speculative `Pipe::create(PipeOptions)`
  overload — zero in-tree consumers; the legacy `nonBlocking` flag
  was already dead) plus three disclosure gaps. **Batch G** closed
  all four: (1) reverted libutil #163's overload commit
  (`e3f228714` dropped via non-interactive rebase; new libutil tip
  `7399593ce`); (2) added libstore rl-next entry
  `legacy-ssh-store-windows-logfd.md` documenting the user-visible
  Windows `log-fd=N` behaviour change from #80 (committed as
  `a3ed63c01`); (3) added libmain rl-next entry
  `getIntArg-allowunit-removed.md` documenting the public-header
  4-arg signature break for out-of-tree consumers (Hydra, Lix,
  plugins; committed as `b1a14158f`); (4) updated #97's Branch line
  on the doc branch to capture two latent-bug fixes folded in via
  the `parseEnum` migration (`parseHashFormat` was missing
  `'nix32'` from its accepted-values error tail; `parseFileSerialisationMethod`
  had a `"serialiation"` typo). clang-tidy + `nix build -L .`
  green on libutil/libstore/libmain post-Batch-G. Final tip SHAs
  after Batch G: libexpr `76b6808f0` (unchanged), libfetchers
  `d1a856c3f` (unchanged), libflake `fd6749635` (unchanged),
  libmain `b1a14158f`, libstore `a3ed63c01`, libutil `7399593ce`,
  nix-cli `2a34c3148` (unchanged). **Post-Batch-G audit follow-up:**
  a bidirectional traceability audit (independent agent) surfaced
  one phantom claim — `#221` was shelved under STATUS' libutil row
  but the commit lives on libstore — re-shelved with cross-shard
  notes on both rows; one missing rl-next disclosure for the
  additive `BaseSetting<T>::overrideIfSet` member added in commit
  `aa00e072e` (#119) on a shipped header (libstore tip moves to
  `b9a750f00`); and one stylistic gap — three libutil commits used
  the non-canonical `Candidate #NN in doc/inventory/candidates/.`
  form rather than `Refs candidate #NN.` — reworded via
  metadata-only `git rebase -i master` (libutil tip moves to
  `da1e0d7d9`; trees unchanged). clang-tidy + `nix build -L .`
  re-verified post-rebase; 5 known pre-existing master errors
  only. Final tip SHAs after follow-up: libstore `b9a750f00`,
  libutil `da1e0d7d9`; other five branches unchanged.
- **20 GREEN+YELLOW candidates (18 commits) landed + 4-pass review + Pass-A/B/C amends + new candidates #225/#226** (May 2026, this batch)
  — bucket sweep over the catalog: triaged 28 small-effort candidates
  into GREEN (15)/YELLOW (5)/RED (8). Landed all GREEN+YELLOW: libstore
  +8 (#1+#3 BuildResult/DrvOutput/Realisation templates with
  `readBuildResult`/`writeBuildResult` cpu-timing-as-callback;
  #7 `negotiateVersion`; #11 `tryUpperFallLower` returning
  `std::pair<R, bool>`; #55 stale migration drop; #59 `useBuildUsers`
  static drop; #101 `partitionRealisedPaths` + `registerCopiedRealisations`;
  #153 `DerivationBuilderImpl` composition; #185 `ServeProto::BasicConnection`
  + `protoVersion` rename); libexpr +3 covering 4 candidates (#192
  fetchClosure-mode collapse via `bool expectInputAddressed`;
  #202+#203 drop failure-cache machinery, bump v6→v7, wrap `getAttr`
  in `doSQLite`; #204 drop `MultiEvalProfiler`, two named optional
  members, ctor-frozen `profilerHooks` cache); libutil +4 (#97 new
  `parse-enum.hh` migrating `parseHashAlgo`/`parseHashFormat`/
  `parseCompressionAlgo`/`parseFileSerialisationMethod`/
  `parseFileIngestionMethod`; #124 `Sync<T>::ConstLock` + drop
  `SyncBase`; #167 `io-buffer-sizes.hh` with `kDefaultIOBlockSize` +
  `kBufferedStreamSize`; #224 `CanonPath::numSegments`); libfetchers
  +1 (#53 latent-bug fix in `getCustomRegistry`); libflake +1 (#133
  rename `configureEvalSettings` → `populateExtraPrimOps`); libmain
  +1 (#223 `getIntArg` wrapper around `getArg`). Skipped #76 (libc++
  in dev shell lacks `<generator>`) and several others as RED.
  **Pass A** (adversarial, parallel, 6 reviewers) found 2 BLOCKERS
  (libstore #153 over-broad rename corrupted the `external-builders`
  JSON wire-key from `"inputPaths"` to `"params.inputPaths"`; libutil
  #97 dropped the inline allowed-name list breaking
  `eval-fail-hashString-2.err.exp`) and ~12 minor items. Both
  blockers fixed via amend; `parseEnumOrThrow` extended to render
  `'a', 'b', or 'c'` with Oxford comma + best-match suggestions.
  **Pass B** (`/code-review` skill, sequential, 18 reviewers) found
  no blockers; substantive concerns amended: libexpr `callFunction`
  hot-path now reads a ctor-frozen `profilerHooks` snapshot (drops
  per-call virtual `getNeededHooks()` dispatches) and the
  `runFetchClosureChecked` local-shadow `bool isContentAddressed`
  renamed to `contentAddressed`; libstore `tryUpperFallLower`
  returns `std::pair<R, bool>` instead of threading a flag through
  a captured lambda, dead `swallowMissingCaDerivations` parameter
  dropped, narrative comments stripped from
  `derivation-builder.cc`/`serve-protocol-connection.hh`/
  `local-overlay-store.cc`; libutil completed three migration gaps
  (file-content-address parsers via `parseEnumOrThrow`; 5 dead
  `assert(s)` lines on `Sync<T>::Lock::wait*`; rename
  `kCompressionOutBufSize` → `kBufferedStreamSize` and 4 more sites
  adopt the constants; 3 more `numSegments` adopters); libflake
  added a docstring on `populateExtraPrimOps` documenting the
  lifetime contract. **Pass C** (clang-tidy, sequential) found one
  real error in libutil — `parse-enum.hh` missing
  `nix/util/configuration.hh` include for
  `ExperimentalFeatureSettings`. Amended into #97. Other shards
  showed only the 5 known pre-existing master errors.
  **Pass D** (`nix build -L .`, sequential, full flake build + test
  suite) green on all 6 shards. **New catalog candidates from
  Pass-B review:** #225 (libfetchers `getUserRegistry`/
  `getSystemRegistry`/`getGlobalRegistry` share #53's first-call-wins
  shape but on the hot path), #226 (libutil `showCompressionAlgo`
  vs `printHashAlgo` enum-rendering inconsistency — the inverse
  direction wasn't migrated to a table-driven helper). Final tip
  SHAs: libexpr `adc7301d1`, libfetchers `f39f6486e`, libflake
  `fd6749635`, libmain `8f2494f61`, libstore `075a0479a`, libutil
  `d7e3fdc6d`. All seven branches Local-only past their pushed tips.
- **20 trivial candidates landed + 4-pass review + Pass-B-follow-up amends + new candidates #220/#221** (May 2026)
  — landed batch (above) plus full Pass A (adversarial, parallel),
  Pass B (`/code-review` skill, sequential), Pass C (clang-tidy,
  sequential), Pass D (`nix build -L .` flake build + full test
  suite, sequential) on every shard. **Pass A** found 13 follow-ups
  (4 real concerns on libstore [#80 round-trip comment, #80 ABI
  break, #80 Unix `documentDefault`, #135 ABI break], 1 on libexpr
  [#79 `EvalError` cacheability shift]; 8 minor/cleanup); all amended
  via `git rebase -i`. **Pass B** found 5 minor + 9 follow-ups;
  fixed in-batch: libflake `isUnlocked` redundant-set regression
  introduced by the #78 helper, libexpr `// Refs candidate #67`
  inventory leak in source, libstore four narrative-history
  comments across `#26`/`#100`/`#135`. **Pass C** found 3 in changed
  files (1 libflake `forEachReachableNode` forwarding-ref
  unused, 1 libexpr `mkFailedFromCurrentException` forwarding-ref
  unused, 1 libexpr `BinOp` CRTP ctor-accessibility); first two
  fixed by accepting `const &`; CRTP fixed via `NOLINTNEXTLINE`
  with documented rationale (the lint's recommended fix breaks
  `polymorphic_allocator::construct`). **Pass D** green on all six
  shards. **Pass-B-follow-up amends** (categorised after the user
  asked which follow-ups were caused by our changes versus
  pre-existing): six commits amended in place — libfetchers #51
  unwrapping single-field RefInfo to `Hash` (caused by #51's trim);
  libexpr #67 transposing the four parallel constexpr ladders into
  a single per-`Op` `NumOpDiag` struct (surfaced by #67's collapse);
  libutil #37/#163 docstring polish (lifetime contract + Windows
  ignore note + class-doc rewrite); libstore #100 templating
  `forEachConfiguredSigner` (caused by #100's helper extraction);
  libstore #135 bundling `startId`/`uidCount` into a `UidRange`
  struct (caused by #135's flatten); nix-cli #74 templating
  `runWholeStoreGC`'s printer (caused by #74's helper extraction).
  Two new catalog candidates filed and landed: **#220** (libexpr,
  extending the renamed `iterateNamedAttrs` helper to `prim_path`)
  and **#221** (libstore→libutil, lifting
  `parseSettingTokens`/`renderSettingTokens` into shipped
  `configuration.hh` so future `BaseSetting<C<T>>` specialisations
  can reuse the helper). **Final adversarial review pass** over the
  six amends + two new commits: libfetchers/libstore/nix-cli clean;
  libutil 1 minor (docstring overclaimed `std::map::insert`
  invalidation, fixed); libexpr 2 minor (#220 commit body factual
  error "six-way" → "five-way", + ANSI-coloring drift acknowledgement
  for primop name now interpolated as `'%s'` instead of being baked
  into the format string; both fixed via amend). **Bidirectional
  traceability audit + orphan-commit cleanup** (final): a
  general-purpose audit walked every commit on every cleanup branch
  against every catalog Branch line. Findings: zero phantom Branch
  lines, zero wrong-fix commits; 7 orphan commits across 5 shards
  (commit bodies missing the `Refs candidate #NN` line) and 4 stale
  Branch line texts where Pass-B amends shifted the implementation
  but the catalog text didn't update. **All 7 orphans fixed** via
  per-shard rebase amends adding the missing references: `437eea9d0`
  → #217 (libexpr), `74e512d11` → #52 (libfetchers), `513b47628` →
  #48 (libflake), `35ab0faaf` → #49 + #128 (libmain), `5f5b8151e` →
  #54 + #56 + #57 + #60 (libstore, four bundled), `52be6d116` → #58
  + #130 + the documented #148 attempt-and-revert (libutil),
  `784a4f4b4` → catalog cleanup tied to #148 (libutil),
  `48524e038` → #127 + #142 (nix-cli). All 4 stale Branch lines
  re-synced (#100 templated, #74 templated, #135 `UidRange` struct,
  #51 `RefInfo` unwrap). **`/code-review` skill brought to parity**
  on libmain (the one shard skipped earlier; commit `35ab0faaf`
  ships clean per the three-agent fan-out). **Post-audit follow-up
  sweep**: filed #222 (libmain `--max-freed` clamp oddity), #223
  (libmain `getIntArg<N>` inline opportunity), #224 (libutil
  `NarIndexer::createMember` path-depth idiom). Implemented #222 on
  libmain (`a7bb49f91`); #223 and #224 filed but deferred per their
  trade-off discussions. Plus four code/doc amends folded into
  existing commits: libexpr #214 instantiation note, libstore #135
  `UidRange` nested in `AutoUserLock`, libstore #221 doxygen scope
  expansion, libutil #163 `PipeOptions` calling-style ambiguity
  note. **Worktree-source-drift discovered on libmain**: build dir
  was configured against master rather than the worktree (the
  HANDOFF.md gotcha) — reconfigured with explicit source dir; the
  earlier libmain `/code-review` reviewers happened to read post-
  rebase source via `git show` so their conclusions still hold,
  but no actual ninja compile of the worktree tree had occurred
  until this fix. Final tip SHAs: libexpr `c9401c3c4`, libfetchers
  `dc5dc001d`, libflake `fd75c930f`, libmain `a7bb49f91`, libstore
  `e5651495a`, libutil `e3f228714`, nix-cli `ef3236827`. All seven
  branches Local-only past their pushed tips; push policy is
  explicit-only and per-commit.
- **20 trivial candidates landed across 6 shard branches + Pass A review** (May 2026)
  — libexpr +4 (#67, #79, #105, #214; 3 skipped: #63 Rule 6, #194/#197 Rule 2 install_headers; doxygen `MakeBinOp` follow-up landed after Pass A), libstore +7 (#26, #46, #80, #83, #85, #100, #135; 1 skipped: #84 prescription doesn't shrink either body), libfetchers +1 (#51 expanded; #70 closed as resolved-upstream by `de6b5f60c`), libflake +1 (#78), libutil +2 (#37, #163), nix-cli +5 (#72, #74, #75, #93, #141; 1 skipped: #25 preprocessor cannot embed `#include` in macro args; `removeOldGenerations` `static`-ify follow-up landed after Pass A). Pass A adversarial review found 13 follow-ups across the six shards (no blockers): 4 real concerns on libstore (#80 false round-trip comment, #80 ABI break in shipped `legacy-ssh-store.hh`, #80 Unix `documentDefault` regression, #135 ABI break in shipped `local-settings.hh` for `GCSettings`/`AutoAllocateUidSettings` removal), 1 real concern on libexpr (#79 exception-type change shifts cacheability), and 8 minor items; all amended via `git rebase -i` in their respective shard branches. Build verified green (`ninja`) on every shard after each amend. Pass B (`/code-review` skill, sequential per shard), Pass C (clang-tidy, sequential), Pass D (`nix build -L .`, sequential) still pending.
- **30 trivial candidates landed across 4 shard branches** (May 2026)
  — libexpr +11 (#64, #66, #68, #106, #107, #109, #110, #112, #196,
  #199, plus a forceValueDeep-to-SeenSet follow-up), libstore +14
  (#4, #5, #6 hoist-then-delete, #14, #47, #82, #114, #115, #119,
  #157, #159, #165, plus a macro→template follow-up and a
  GET_PROTOCOL_*-deletion follow-up), libutil +3 (#29, #99, #117),
  nix-cli +5 (#24, #77, #89, #91, #187). Two skipped on rule
  grounds: #194 (search-path.hh ships via install_headers, not
  internal as the candidate framing claimed) and #197 (lookupPath
  is private). Each shard went through 4 adversarial review agents
  + 4 `/code-review` skill passes (12 agent runs total); 20
  nits/deferrals were captured and addressed in a final polish
  pass. New candidate #219 (post-build-hook timeout silently
  dropped) added to the catalog from post-cleanup review.
- **Integration of 19 review reports** (HEAD) — folded findings from
  E1-E8, F1-F3, B1-B3, C1, V1-V5 into the candidate bodies. Notable
  edits: #134 expanded with C-API caveat per V1; #135 corrected per
  B1 (does NOT depend on #136); #138 expanded with F1+V3 UAF repair
  + V1 release-note caveat; #65 corrected per B2 (79 canonical sites
  + ~135 bespoke); #211 corrected per B2 (7 structurally-shared
  bodies, not 10); #18 corrected per B3 (17 hand-written derivations,
  not 14; 18a is not a refactor target); #213 expanded with E8+V4
  Direction A recommendation; #166 expanded with E7's three-header
  recommendation; #125 expanded with E4+V2+F2's wire-crossing
  reachability + two text-grep contracts + phasing constraint;
  #128 noted commit-bundling per CLAUDE.md; #140 expanded with E1
  verification details. New candidate #218 added (`Hash::dummy`/
  `StorePath::dummy` placeholder pattern). N41 corrected per E6+V5
  (28 fixtures, 19 fit cleanly with subdir-only ctor, 27/28 with
  `(subdir, suffix)` ctor pair, 1 structural hazard).
- **Evidentiary standard added** (`7cb0eca34`) — eight rules in
  `candidates/README.md` plus operational form in
  `review/AGENT-CHARTER.md`. Derived from failure modes seen across
  the review passes.
- **18 review reports landed in-tree** (`7cb0eca34`) — E1-E8,
  F1-F3, B1-B3, C1, V1, V3, V4, V5.
- **Four-pass review consolidation** (`f3bc2c222`) — folded the
  ~5,300-line review output into the catalog. 89 findings applied,
  13 deferred (the E/F/B/C/V queue), 4 rejected.
- **Two adversarial passes + two pattern-discovery passes** —
  reports under `review/01-…md` through `review/05-…md`.
- **Catalog scope rewrite** — perf and modernisation are in scope on
  their own merits.
- **Catalog split into 25 sections** — original monolithic
  `CANDIDATES.md` deleted; 217 + 44 candidates organised by topic.
  (Now 238 + 44 after #218–#238 — #220–#221 added when Pass B
  reuse review surfaced catalog gaps while landing #79 / #85;
  #222–#224 surfaced by the post-audit follow-up sweep around #128;
  #225–#226 surfaced by the May 2026 Pass-B sweep on #53 and #97
  respectively; #227–#233 filed as Batch D from the May 2026
  whole-branch holistic-review pass; #234–#238 filed by the
  post-Batch-G adversarial-audit pass after the user asked for an
  inventory of unfiled opportunities — auditor confirmed three
  items genuinely lacked tracking numbers and named two more
  whose framing/SHA had errors but were also untracked.)
- **Seven cleanup PRs pushed** — `vibe-coding/cleanup/*` shard branches
  on origin, ready for upstream review.

## Cleanup branches (live)

These are pushed to `origin` (`ConnorBaker/nix`) and ready for PR
creation against upstream. Each shard branch is **long-lived**: as
more candidates land in the same shard, they are appended as
additional commits on the same branch (one PR per shard, multi-commit
review). Each candidate addressed by a branch carries a `**Branch:**`
line in its body.

| Branch | Shard | Addresses | Status |
| ------ | ----- | --------- | ------ |
| `vibe-coding/cleanup/libexpr` | libexpr | #217, #64, #66, #68, #106, #107, #109, #110 (extended to forceValueDeep), #112, #196, #199, #67, #79 (3 of 4 sites; `prim_fetchTree` excluded as misclassified — its loop dispatches on value type, not name), #105 (with doxygen `EXPAND_AS_DEFINED` follow-up), #214, #220, #192, #202+#203 (folded — drop failure-cache machinery, bump v6→v7, wrap `getAttr` in `doSQLite`, rl-next entry), #204 (drop `MultiEvalProfiler`; ctor-frozen `profilerHooks` cache); plus Batch-C doc fixes (lexer.l ID retarget, primop diagnostic rl-next); plus Batch-E compound-win cleanups #227 (drop dead `EvalProfiler` NVI cache + default no-op virtuals; rl-next entry), #228 (extend `NumOp` to `BitAnd`/`BitOr`/`BitXor`), #229 (parameterise `SeenSet` template + migrate underlying container to `boost::unordered_flat_set`) | Local-only past `76b6808f0` |
| `vibe-coding/cleanup/libfetchers` | libfetchers | #52, #51 (delete-and-trim, scope expanded past the two `#if 0` blocks), #53 (latent-bug fix in `getCustomRegistry`); plus Batch-C rl-next extension noting treeHash rejection | Local-only past `d1a856c3f` |
| `vibe-coding/cleanup/libflake` | libflake | #48, #78 (helper covers two of three DFS sites; `doFind` excluded as structurally different), #133 (rename only; deviation from prescribed end state documented) | Local-only past `fd6749635` |
| `vibe-coding/cleanup/libmain` | libmain | #49, #128, #222 (`--max-freed` clamp), #223 (`getIntArg` wrapper around `getArg`); plus Batch-C max-freed-clamp rl-next entry; plus Batch-G `getIntArg` 4-arg-signature-break rl-next entry | Local-only past `b1a14158f` |
| `vibe-coding/cleanup/libstore` | libstore | #54, #56, #57, #60, #4 (extended to template form), #5 (extended to template form), #6 (hoist-then-delete), #14, #47, #82, #114, #115, #119 (helper extracted as `BaseSetting<T>::overrideIfSet` on a shipped header — Batch-G follow-up rl-next), #157, #159, #165, #26 (Linux only — Windows untested under current CI), #46 (buildenv + unpack-channel; `fetchurl` excluded — different lookup shape), #80 (Windows behaviour change: `log-fd` now respected), #83, #85, #100 (helper-only scope), #135 (option (a) flatten), #1+#3 (BuildResult/DrvOutput/Realisation templates with cpu-timing-as-callback), #7 (`negotiateVersion` client-side helper), #11 (`tryUpperFallLower` returning `std::pair<R, bool>`), #55 (stale migration drop), #59 (`useBuildUsers` static drop), #101 (`partitionRealisedPaths` + `registerCopiedRealisations`), #153 (`DerivationBuilderImpl` composition), #185 (`ServeProto::BasicConnection` + `protoVersion` rename), #221 (cross-shard: edits a shipped libutil header — `configuration.hh`'s `parseSettingTokens` / `renderSettingTokens` — but lives on this branch because the consumer is libstore's `globals.cc`); plus Batch-C doc fixes (worker-protocol-connection wrong-shard wording, negotiateVersion docstring); plus Batch-G #80 Windows-`log-fd` rl-next entry; plus Batch-G follow-up #119 `overrideIfSet` rl-next entry | Local-only past `b9a750f00` |
| `vibe-coding/cleanup/libutil` | libutil | #58, #130, #29, #99, #117, #37 (helper covers one of three NAR walks; other two structurally distinct), #97 (new `parse-enum.hh`; migrated 5 parsers including file-content-address pair from Pass-B sweep; folded in two latent-bug fixes — `parseHashFormat` `nix32` omission and `parseFileSerialisationMethod` typo), #124 (drop `SyncBase`; `Sync<T>::ConstLock` + `mutable mutex`), #167 (`io-buffer-sizes.hh` + 6 site migrations), #224 (`CanonPath::numSegments` + 4 migrations); Batch-G dropped #163 `PipeOptions` overload (zero in-tree consumers); Batch-G follow-up reworded #29/#99/#117 commit bodies to canonical `Refs candidate` form. Note: `#221` ships on the libstore branch even though it edits this shard's shipped `configuration.hh` — see the libstore row | Local-only past `da1e0d7d9` |
| `vibe-coding/cleanup/nix-cli` | `src/nix/` (modern + legacy CLI) | #127, #142, #24, #77, #89, #91, #187, #74, #75, #72 (free helpers, not mixin — diamond inheritance), #93 (3 of 4 parents; `CmdHash` deferred), #141 (per-command `static`-ify subset only — extended to `removeOldGenerations` after Pass A) | Local-only past `2a34c3148` |

The branches whose Status reads "Pushed" are on `origin`
(`ConnorBaker/nix`) and ready for upstream PR creation. The four
Status: Local-only branches each have additional commits beyond
the pushed tip and have not been pushed yet — push policy is
explicit-only.

## Worktree layout

The seven shard branches are checked out as worktrees under
`/Users/cbaker2/ext-sources/nix-worktrees/cleanup-<shard>/`. Future
agents should use `git worktree list` to see what's checked out. New
work targeting an existing shard should reuse that shard's worktree
and append a commit; new shards get a new branch and a new worktree.

## How to pick up this work

Read in order:

1. `INVENTORY.md` — what the codebase looks like.
2. `candidates/README.md` — what the catalog tracks and how.
3. `STATUS.md` (this file) — what's pending.
4. `review/00-INDEX.md` — what investigations have been done.
5. `review/AGENT-CHARTER.md` — how to do investigations correctly.

Then pick a queued item from the "Queued / blocked" list above (or
propose a new one). Spawn an agent for it with `AGENT-CHARTER.md` as
"Read first". Update this file when the work lands or changes state.

## Conventions

- Reports go under `doc/inventory/review/`. Naming: `<E|F|B|C|V|N><N>-<topic>.md`.
  - **E** = eight follow-up agents queued from the consolidation
    pass.
  - **F** = "follow-up" findings flagged during E1-E8 that needed
    their own investigation.
  - **B** = "category B" structural decisions deferred from the
    consolidation pass.
  - **C** = "category C" marginal observations deferred from the
    consolidation pass.
  - **V** = "verification" pass — pure source-evidence verification
    of weak/mixed-evidence claims from the prior layers.
  - **N** = new candidate (the cross-shard pattern-discovery passes
    used N1-N44).
- Cleanup branches: `vibe-coding/cleanup/<shard>` — one branch per
  shard, long-lived, accumulating commits as more candidates land in
  that shard. One worktree per branch off master, named
  `nix-worktrees/cleanup-<shard>/`.
- Candidate cross-references: bare `#NN` for 1-217, `NN` (no hash)
  for cross-shard `N1-N44`, `EN` / `FN` / `BN` / `CN` / `VN` for
  reports.
