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

- **Single-shard foundation batch 2 + scope triage (2026-05-29)** —
  worked the "green tier" (candidates scoped as small/contained). The
  honest outcome: only **#216** landed; the tier was smaller than first
  estimated. **Landed:** #216 (libexpr `c3820c35d`) — namespace-scope
  `static_assert`s pinning `sizeof(Value) == 2*sizeof(void*)`, a
  bit-packed-guarded `alignof(Value) == 16`, and the relocated
  `sizeof(Env) <= 16`; the round-trip-test half is deferred medium.
  Also #248 (libexpr `e247a4cb3`, from earlier in this session) —
  `Expr::registerDebugEnv` helper replacing the 19-copy bindVars
  debug-env idiom + binop registration; its build hit a **transient
  `new-build-cmd` flake** (sandbox-FIFO test) that reproduced on the
  unchanged base tip and cleared on re-run, confirmed not caused by the
  change. **Declined after attempt:** #255 (why-depends `resolveToStorePath`)
  — the audit's "four sites" was two, and a `StorePath`-returning helper
  discards the `Installable` objects reused later for `->what()` error
  messages; caught at build time, reverted. **Reclassified small→medium
  after per-site reading** (NOT done as quick batch): #232
  (`ignoreExceptionInDestructor` — several sites are genuinely
  non-destructor, e.g. `S3BinaryCacheStore::abortMultipartUpload`,
  `PathSubstitutionGoal::cleanup`; per-site interrupt-semantics audit
  needed), #95 (legacy-CLI shim with a cleanup-hook pitfall), #45
  (NAR FSO visitor), #201 (eval-cache schema version — touches cache
  invalidation), #208 (installed-header factory), #60-stretch. **Process
  note:** a `nix build` returned exit 1 and was nearly misread green off
  a filtered error-grep — always read the literal "build exit: N" line.
  **Large items #90 + #141 (legacy-CLI structural) deferred** per user
  decision: interlocking multi-file refactors (LegacySubcommandTable +
  handler-context threading), to be their own dedicated pass; left as
  open single-shard-structural candidates. ~20 single-shard candidates
  (mostly medium) remain as foundation work.

- **Single-shard foundation batch 1 (2026-05-29)** — first tranche of
  the per-shard foundation work the value-review inventory surfaced,
  landed as one commit per candidate. **libutil +3:** #259 (`LocalSigner`
  moves the `SecretKey` instead of copying; `publicKey` derived from the
  member to avoid use-of-moved-from without a member reorder), #243
  (latent bug — `git::parseBlob`/`parseTree` size parse via a checked
  `getSizeUntil`/`string2Int<uint64_t>` throwing `SerialisationError`
  instead of `std::stoi`'s `std::logic_error`; octal mode via
  `std::from_chars`), #244 (`MemorySink::openForCreate` prelude extracted
  from the three `create*` methods, both diagnostic nouns preserved
  byte-for-byte). **libexpr +1:** #252 (`setTagged` collapses the three
  tag-only `AttrDb` setters). **libflake +1:** #258 (`applyLockMode` +
  mode table for the three `set_mode_*` C entry points, ABI-preserving;
  fixed the stale `nix_flake_lock_flags_new` "== write_as_needed"
  comments — the fields actually differ on `allowUnlocked`). **Deferred:**
  #251 (`queryMetaTyped`) — re-judged marginal against the churn
  standard (two callables threaded per site ≈ as much plumbing as the
  three short bodies, legacy path, no extra caller); left inline with a
  note. All changed shards `nix build -L .` green (full flake + test
  suite). Tips: libutil `a16c0a75a`, libexpr `411cc11ec`, libflake
  `b29de9420`. New tips are fast-forward over the pushed libutil/libexpr
  origin tips; libflake needs force-push (history rewritten earlier in
  the value pass). ~31 single-shard-actionable foundation candidates
  remain after this batch.
- **Per-commit value/churn review + remaining-work inventory (2026-05-29)**
  — independent per-shard pass (one agent per branch) over all 116
  landed commits, re-deriving value verdicts rather than inheriting the
  earlier whole-branch value review. **Verdict: substantively valuable,
  not churn — 87 substantive / 19 marginal / 4 churn / 3 concern
  (1 genuine code-level).** The flagged seven were all fixable in place:
  **(churn)** libflake #133 `configureEvalSettings`→`populateExtraPrimOps`
  was a pure public-header rename (source-compat break, zero payoff) —
  **reverted to the original name, docstring kept**, #133 reframed
  resolved-by-design; libmain `0b66d94ce` was the known-wrong
  `--max-freed` rl-next, wholly superseded — **squashed** into its
  rewrite (`998a763cc`); libutil #119/#221 header-only commits have zero
  in-tree callers on-branch (same shape as the reverted #163) — **kept**
  by decision, since the consumers are filed/tracked (#239/#240) and
  dropping would override the deliberate cross-shard-cleanup split
  (rationale recorded on #239). **(concern)** nix-cli `ffb5ea72b` had
  shipped the `--priority` regression then-fixed two commits later —
  **squashed** so the broken state never appears in history (`9f70a07f1`
  now carries the correct `needArg` form); libexpr #87's
  `get_list_byidx_impl` comment falsely claimed identical behaviour
  across both symbols — **corrected** via a follow-up commit
  (`b9095c56b`) documenting the lazy-path null-guard change. All four
  rewritten/extended branches rebuilt green via `nix build -L .`; the
  libflake/libmain/nix-cli rewrites preserve a byte-identical final tree
  vs their pre-rewrite backups (verified). **Remaining-work inventory:**
  the catalog is NOT down to only cross-shard work — **~36
  single-shard-actionable candidates remain as the foundation** (libexpr
  ~15, libstore ~7, libutil ~5, nix-cli ~5, libfetchers/libflake ~2
  each, libmain 0), to land before the separately-tracked cross-shard /
  blocked-on-merge / needs-decision work. Highest-value foundation items
  are largely the #241-#259 audit candidates plus pre-existing ones
  (#211, #198, #190 libexpr; #245/#247/#186 libstore; #243/#244/#242
  libutil; #90/#141 nix-cli).

- **Whole-branch adversarial audit + fresh-eyes catalog audit (2026-05-29)**
  — independent multi-agent pass (22 discovery agents → dedup →
  per-finding adversarial verify): re-verified the 104 landed commits
  across all seven cleanup branches for semantics-preserving
  regressions, re-derived load-bearing catalog claims/counts against
  master, and fresh-audited each shard for new opportunities. 70
  findings; **63 confirmed, 7 refuted/downgraded.** **Three blockers
  fixed inline** (uncommitted, pending review): **F001** — a real
  regression on `vibe-coding/cleanup/nix-cli` `ffb5ea72b`: `opInstall`'s
  `--priority` arm used `getArg` (pre-increments) inside an
  `auto arg = *i++` loop, so every `nix-env -i --priority N pkg` threw
  "requires an argument" (test-covered by `user-envs-test-case.sh`);
  fixed to `needArg`; catalog #231 corrected (it had recommended the
  buggy `getArg` form + an `opSetFlag` misnomer). **F002** —
  `--max-freed` rl-next/catalog prose was wrong and dangerous: old
  `-1` was a no-op GC (not "disabled the cap"), `> INT64_MAX` already
  errored at parse (boost throws, doesn't wrap), and the "drop the flag"
  migration advice would trigger an *unbounded* GC; rl-next rewritten,
  #222/FOLLOWUPS/STATUS corrected; the #222 *code* was always fine.
  **F003** — pre-existing master bug: `nix_get_external` used
  `check_value_out` (throws when valid) then asserted validity →
  always failed for legit input; public C-API, no in-tree caller so CI
  never caught it; fixed to `check_value_in`. **Two more latent master
  bugs fixed inline:** **F025** (`destroyCgroup` threw interpolating an
  *uninitialised* `pid` — UB; → `pid_s`) and **F052**
  (`nix_store_path_hash` missing the `last_err_code = NIX_OK` entry
  preamble every sibling has). Plus five sub-trivial behaviour-
  preserving cleanups fixed inline (no candidate number): **F065**
  (`printCodeLines` indent loop → `std::string(n, ' ')`), **F066**
  (shebang parser redundant `make_shared<T>(T(…))` temporaries, 5
  sites), **F067** (`ExperimentalFeatureSettings::isEnabled` linear
  `std::find` over a `std::set` → `.contains()`), **F069**
  (`search.cc::wrap` accidental external linkage → `static`), **F070**
  (`nix_value_call` deprecated `Value` alias → `nix_value *`). All four
  changed shards rebuilt green via `nix build -L .` (full flake +
  test suite: libutil, libexpr, libstore exit 0; libmain doc-only).
  **Catalog corrections folded into the existing sections:** #65 grep
  was malformed and returned 2 not 79 (F014, unanchored form restored);
  #18 count was 17, actual 23 hand-written (F016); #169/#217/N33 friend
  count was "10 lines/9 distinct", actual 11/10 (F013); #103/N6
  `RewritingSink` "tracks match positions" was stale dead code (F019);
  #138 read-site named a nonexistent `addToStore` (F006, → `copyPathToStore`);
  #218's placeholder inventory undercounted (6 → 7-8) and mischaracterised
  the wire-marker as a `DrvOutput` serialisation (it's the `BuildResult`
  compat StringMap) and misfiled `nix-store.cc` as a "legitimate
  constant" (F007-F011); U7b/U7c counts and exclusion reasons (F017/F018).
  **19 new candidates filed** (#241-#259), distributed into their
  *topical* sections (per the catalog's group-by-symptom principle, not
  a provenance-grouped file): 03 (#244), 07 (#246, fixed inline), 08
  (#243), 10 (#253, #255), 12 (#241, #242, #259), 13 (#248, #249, #251),
  16 (#245, #247), 17 (#254, #256; cross-shard ones tagged deferred), 20
  (#250, #252), 23 (#257, #258). **7 refuted/downgraded** (the audit's
  own false positives, kept out of the catalog): F015 ("count is 87" was
  itself a methodology artifact), F046/F047 (#97 wording "undisclosed"
  but actually cataloged), F048 (#119/#221 speculative-API already
  tracked as #239/#240), F053 (`sizeof(Goal)` change real but no
  actionable gap), F054 (#26 Windows `.lock` literal narrowing verified
  harmless), and **F062 — which *confirmed* the #122/U5
  `stackOverflowHandler` symbol-mangle caveat is correct as written**
  (no change needed). Also fixed: the in-tree docs cited macOS
  `/Users/cbaker2/` paths throughout HANDOFF.md/STATUS.md — this machine
  is Linux `/home/cbaker2/`; all references converted. **Code fixes
  committed** per-shard on their cleanup branches (libexpr `b3b02e9a9`,
  libmain `9495181a4`, libstore `410107979`, libutil `b96eda936`,
  nix-cli `fd60a040b`); push policy remains explicit-only, none pushed.
  **Build re-verification (`nix build -L .` on the committed tips):** all
  five green — but the first nix-cli rebuild **failed** on a unity-build
  ODR clash (`makeEdge` redefined across `dotgraph.cc`/`graphml.cc`),
  a latent break introduced by the Sprint-1 #76 landing that per-file
  `ninja` never exercised; fixed by renaming the graphml helpers
  (`fd60a040b`) and re-verified green. Lesson: re-build *every* changed
  shard through the flake unity build, not just per-file `ninja`.
- **Sprint 1 — single-shard cleanup pass (May 2026)**
  — five-candidate sweep filtered to single-shard, no-blocker work
  per the user's "we must defer cross-shard work" rule. Three
  shipped: **#87** (libexpr-c eager/lazy accessor pair dedup via
  private impl helpers preserving error wording byte-for-byte;
  libexpr commit `374e9db5e`); **#111** (`withRegex<Body>` template
  helper extracting `prim_match`/`prim_split` regex-fetch + try/catch
  scaffold; `RegexCache &` parameter sidesteps the existing private-
  member friend cluster, no friend additions, deferring the cleaner
  `EvalState::compileRegex` accessor to whenever #169 lands; libexpr
  commit `418272080`); **#76** (`walkClosure` template helper
  deduplicating the dotgraph/graphml BFS scaffold; helper kept in a
  new file-local `nix-store/closure-walk.hh` rather than promoted to
  `libstore/include/nix/store/store-api.hh` per the validation's
  original sketch, because the latter would be a cross-shard libstore
  change; nix-cli commit `705114e2c`). One **already shipped** in
  Batch E but missing a Branch line: #229 (parameterised `SeenSet<T>`
  on libexpr; commit `76b6808f0` ships the implementation; Branch
  line added to candidate body in this round). One **declined**:
  #156 (fan-out idiom dedup) — re-reading the two sites with fresh
  eyes shows the "shared" block reduces to ~6 lines before the two
  callers diverge into different `doneFailure` overload shapes;
  extraction is cosmetic, doesn't unlock anything. Re-evaluate when
  the surrounding `doneFailure` overloads unify or a third caller
  adopts the same shape. New tip SHAs: libexpr `418272080`, nix-cli
  `705114e2c`. `nix build -L .` green on both branches; libstore /
  libutil / libfetchers / libmain / libflake unchanged.
- **Post-batch-H Pass-4 adversarial review — file missing rl-next for #185 + record three coverage-gap findings** (May 2026)
  — fourth adversarial-review pass biased toward areas no prior
  reviewer had touched. **BLOCKER caught:** #185's `d313a74c6`
  commit (`libstore/serve-protocol: extract BasicConnection base,
  rename remoteVersion`) explicitly named the field rename
  `remoteVersion`→`protoVersion` as a "deliberate public-API break
  shipping in the install-headers serve-protocol-connection.hh
  header" requiring downstream-consumer (Hydra, Lix, plugin)
  update on rebuild. No rl-next disclosure shipped at the time.
  Same shape as the Batch-G systematic disclosure-fix pass (which
  closed #80 / #128 / #119) — fell through. **Fixed:** added
  `doc/manual/rl-next/serve-protocol-protoversion-rename.md` on
  `cleanup-libstore` documenting the rename + the semantic note
  that the field always held the negotiated version. New libstore
  tip `09abb8158`. **Three coverage-gap findings recorded** but
  not actioned this round (any new tests would mean new commits
  on already-pushed branches; defer until the next batch lands
  alongside): (1) #80 Windows `log-fd=N` URL parameter
  honour-on-Windows behaviour change has zero test coverage; (2)
  #222 `--max-freed -1`/overflow error-instead-of-disable change
  has zero negative-path test coverage; (3) #235 `nix-store
  --realise` `--add-root`-missing warning emission for
  non-`LocalFSStore` has zero functional-test coverage — the
  rl-next prose pins the user-visible behaviour but no test
  asserts the warning text. **Push-state finding:** three of
  seven origin tips diverge from local because of the cross-shard
  cleanup + #224-reword cascade + Batch H — libstore (origin
  `b9a750f00` is now-dropped), libutil (origin `da1e0d7d9` is
  pre-rebase), nix-cli (origin `2a34c3148` lacks Batch H). All
  three need force-push when published. (Catalog body finding,
  recorded for future implementer of a queued candidate but no
  action this round: #218 wire-marker inventory cites
  `worker-protocol.cc` as a wire-marker site; post-#1+#3
  template-lift the marker now lives in `common-protocol-impl.hh`.
  #218 is queued/unimplemented; verify the body before the
  implementation lands.) **[Corrected by the 2026-05 audit, F007/F010:**
  on master the marker is in `worker-protocol.cc` inside the
  `Serialise<BuildResult>::write` `common` lambda; the cleanup-branch
  template-lift moves it into `writeBuildResult<Proto>` in
  `common-protocol-impl.hh` — the `BuildResult` serialiser, NOT
  `Serialise<DrvOutput>`. The #218 body now carries the corrected
  inventory.**]
- **Post-batch-H Pass-3 adversarial review — refresh stale SHAs after #224-reword cascade + document #234 snapshot-iterate discipline** (May 2026)
  — third adversarial-review pass after the user said "another
  adversarial review" caught a second-order-staleness pattern.
  The Pass-2 #224 commit-message reword was a `git rebase -i
  master` with `reword`, which rewrote the metadata of #224 and
  every commit past it on the libutil branch — bumping five SHAs
  including #119's API addition (`f2ead5163` → `2bc3e21e0`),
  #221's API lift (`1ac18fe9a` → `3f893878b`), and #119's
  rl-next entry (`fdb46ab33` → `3b6cdf3d1`).
  The Pass-2 fixes had cited the *pre-reword-cascade* SHAs in
  six places, leaving them dangling-in-the-object-store with no
  branch reachability. **Six citations refreshed** in this round:
  #239 body and validation (3 citations), #240 body (1 citation),
  FOLLOWUPS Batch-G #221 phantom-claim post-cleanup correction,
  FOLLOWUPS Batch-G #119 missing-rl-next post-cleanup correction.
  All now point at current libutil-branch SHAs, with a note that
  the SHAs may move again before the libutil branch merges
  upstream.
  **Doc gap closed:** #234 Branch line documented pointer-stability
  via `std::unique_ptr` but not the snapshot-iterate discipline
  added by the Pass-2 iteration-safety fix. Branch line now
  describes both safety properties.
  **Bonus narrative refresh:** STATUS.md's Pass-2 entry self-
  description ("#239 added the libutil SHA citation `f2ead5163`")
  reworded to avoid pinning to specific SHAs that have since
  moved.
  No code changes this round. Tip SHAs unchanged: libstore
  `6022c2925`, libutil `834a52639`, others unchanged.
- **Post-batch-H Pass-2 adversarial review — fix #234 iteration-safety + #224 commit body + 4 doc gaps** (May 2026)
  — second adversarial-review pass after the user said "dig
  deeper" caught a real iteration-safety regression and three
  doc gaps that the first pass missed.
  **Real bug:** #234's `forEachConfiguredSigner` removed the
  pre-existing `auto secretKeyFiles = settings.secretKeyFiles;`
  snapshot copy that #100 had, replacing it with a direct
  `settings.secretKeyFiles.get()` iteration that holds a `const
  std::list<std::string> &` to live `Setting<T>` state.
  `BaseSetting<T>::get()` returns `const T &` with no
  synchronisation, and a trusted daemon client can mutate
  `secret-key-files` mid-session via `ClientSettings::apply`.
  Iterating a live setting reference would race the
  `std::list` re-assignment. Fixed by restoring the snapshot
  copy before the for-range. Amended in place; new libstore tip
  `6022c2925`.
  **Stale commit-message body:** #224's commit message claimed
  "Migrate the single existing call site in nar-listing.cc"
  while the diff actually migrated four sites
  (`NarIndexer::createMember`, `CanonPath::fromFilename`,
  `unix/file-system-at.cc::openFileEnsureBeneathNoSymlinksIterative`,
  the equivalent on Windows) plus a unit test. FOLLOWUPS Batch A
  recorded this as fixed but the fix had only been a metadata
  rebase that bumped the SHA without touching the message body.
  Reworded via non-interactive `GIT_SEQUENCE_EDITOR` + sed-based
  `GIT_EDITOR`; new libutil tip `834a52639`. The reword cascade
  bumped all five libutil commits past #224.
  **Doc gaps closed:** #97 Branch line had `parseCompressionAlgo`
  in `compression-settings.cc` (actual: `compression-algo.cc`);
  fixed. #226 Branch line gained a latent-fragility note —
  `renderEnum`'s first-match correctness depends on alias rows
  following canonical rows in the table, with no compile-time
  enforcement; flagged for future hardening (`static_assert` or
  alias-tag pattern). #239 / #240 gained their libutil SHA
  citations for parity (the SHAs themselves were rewritten by the
  same #224-reword cascade and were re-refreshed in pass-3; see
  the next entry). STATUS.md gained a "Current tip SHAs (as of
  last edit)" block at the document tail to disambiguate against
  the chronological log's intermediate-tip claims. clang-tidy and
  `nix build -L .` green on libutil and libstore post-rebase +
  post-amend.
- **Post-batch-H adversarial review — fix #234 thread-safety bug + doc gaps** (May 2026)
  — adversarial review across the doc-branch and seven cleanup
  branches caught one real bug and several documentation gaps.
  **Real bug:** #234's signers cache returned a `LocalSigner &`
  to a `boost::unordered_flat_map` value held outside the lock;
  flat-map references are invalidated on rehash, so a concurrent
  first-time sign on another worker thread could dangle the
  reference. Fixed by switching the map values to
  `std::unique_ptr<LocalSigner>` so the pointer returned from
  `getCachedSigner` stays valid across rehash; lock scope stays
  at map-mutation only (holding the lock through signing would
  serialise every worker-thread sign through one mutex).
  Amended in place; new libstore tip `689effee2`.
  **Doc gaps closed:** added missing `**Branch:**` lines to six
  shipped candidates (#183, #226, #230, #231, #234, #235) so
  catalog readers can find the implementation from the candidate
  body alone; documented the speculative-API trade-off on #119
  and #221 — the libutil header halves currently have zero
  in-tree consumers (waiting for #239/#240 to land after libutil
  merges), structurally similar to the libutil #163 revert but
  differentiated by the consumers being filed and tracked.
  **Minor info loss restored:** the `// FIXME: get username
  somewhere` comment on GitLab clone, originally inlined and
  dropped during #230's base-class promotion, restored as a
  class-level comment on `GitLabInputScheme`. Amended in place;
  new libfetchers tip `572e8fde8`. clang-tidy and
  `nix build -L .` re-verified green on both branches post-fix.
  Final tip SHAs after this round: libstore `689effee2`
  (was `80fd74bca`), libfetchers `572e8fde8` (was `12a17bb65`);
  other five branches unchanged.
- **Trivial+small batch H — six candidates landed across four shards, no cross-shard violations** (May 2026)
  — bucket of post-review-verdict candidates landed in
  single-shard commits. Each fanned out independently across
  branches; none introduced a cross-shard dependency. Trivials:
  **#183** drops the dead `WorkerProto::Op::QueryDeriver` arm in
  `daemon.cc::performOp` (validation confirmed
  `MINIMUM_PROTOCOL_VERSION=1.18` already excludes any client that
  would emit it), libstore tip `14bb9fd31`. **#231** replaces the
  hand-rolled iterator-advance + end-check + parse pattern in
  `nix-env --priority` with a single `getArg(...)` call
  (conservative form only — no speculative `getIntArgNoUnit<N>`
  helper), nix-cli tip `ffb5ea72b`. **#235** consolidates
  `printGCWarning` gating across `nix-store --realise` and
  `nix-instantiate` after #91's `GcRootNamer` shipped; warning
  now fires regardless of store type, matching the
  nix-instantiate behaviour. User-visible behaviour change for
  non-`LocalFSStore` users without `--add-root`; rl-next entry
  `nix-store-add-root-warning.md` ships on the same branch
  (`f4f95a3c9` + `24ebbcc35`). Smalls: **#226** adds
  `renderEnum<E>(E, EnumNames<E>) → std::string_view` to libutil
  `parse-enum.hh` and migrates all five enum→string renderers
  (`printHashAlgo`, `printHashFormat`, `showCompressionAlgo`,
  `renderFileSerialisationMethod`, `renderFileIngestionMethod`)
  off hand-written switches and the linear-scan loop, closing
  the parse/show symmetry from #97; libutil tip `190b08372`.
  **#230** promotes `getHost`/`getOwner`/`getRepo` and
  `clone()` to `GitArchiveInputScheme`, with per-subclass
  `defaultHost()` and a defaulted `cloneUrlSuffix()` virtual
  (SourceHut overrides to `""`). 70 insertions, 56 deletions
  across the three subclasses; libfetchers tip `12a17bb65`.
  **#234** caches parsed secret keys per file path in
  `Store::signPathInfo`/`signRealisation`, closing a years-old
  FIXME. Cache is mutation-safe wrt the `secret-key-files`
  setting because keys are indexed by file path, not by setting
  state; uses `Sync<boost::unordered_flat_map<...>>` per the
  project's data-structure preference. libstore tip
  `80fd74bca`. Three rejected by adversarial review and not
  shipped: #168 (cosmetic colocation; superseded by N31), #219
  (dead-code defence; `respectTimeouts=false` blocks the path),
  #236 (declined; per-format subclass split was more code than
  it removed). One deferred to a separate two-step ship: #225
  (registry first-call-wins memo; needs the libutil
  mtime-keyed-memo helper as a prerequisite, which would create
  a third cross-shard violation if bundled with the libfetchers
  consumer migration).
- **Cross-shard cleanup — revoke libstore commits that touched libutil headers** (May 2026)
  — user-mandated removal of two cross-shard violations on the
  libstore cleanup branch. Both commits combined a libutil
  header addition with a libstore consumer migration in a single
  commit, which would have prevented the two branches from
  landing independently upstream. Three commits dropped via
  non-interactive `git rebase -i master` on cleanup-libstore:
  `aa00e072e` (#119 `BaseSetting<T>::overrideIfSet` libutil
  header + `HttpBinaryCacheStore` consumer), `ae7167e8c` (#221
  `parseSettingTokens`/`renderSettingTokens` libutil header lift
  + `globals.cc` consumer), and `b9a750f00` (#119 rl-next entry,
  which lives where the API change lives). The libutil header
  halves were replayed as two single-shard commits on
  cleanup-libutil (`f2ead5163` and `1ac18fe9a`) plus the rl-next
  entry (`fdb46ab33`); the libstore consumer halves were dropped
  outright and re-filed as queued candidates **#239** (libstore
  consumer for #119) and **#240** (libstore consumer for #221),
  both blocked on the libutil cleanup branch landing upstream
  before they can re-land. Same shape as the existing Batch-E
  cross-shard extensions (#167, #124). New tip SHAs:
  cleanup-libstore `ec50b5ba4` (was `b9a750f00`; 31 commits past
  master, was 34); cleanup-libutil `fdb46ab33` (was `da1e0d7d9`;
  13 commits past master, was 10). `nix build -L .` post-rebase
  green on both branches.
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
  one phantom claim (`#221` shelved under STATUS' libutil row
  while the commit lived on libstore — re-shelved at the time
  with cross-shard notes; **superseded** by the later cross-shard
  cleanup entry below, which dropped the libstore commit
  entirely); one missing rl-next disclosure for the additive
  `BaseSetting<T>::overrideIfSet` member added in commit
  `aa00e072e` (#119) on a shipped header (libstore tip moves to
  `b9a750f00` — **superseded** by cross-shard cleanup; this
  commit was dropped); and one stylistic gap — three libutil
  commits used the non-canonical `Candidate #NN in doc/inventory/
  candidates/.` form rather than `Refs candidate #NN.` — reworded
  via metadata-only `git rebase -i master` (libutil tip moves to
  `da1e0d7d9`; trees unchanged). clang-tidy + `nix build -L .`
  re-verified post-rebase; 5 known pre-existing master errors
  only. Final tip SHAs after follow-up: libstore `b9a750f00`,
  libutil `da1e0d7d9`; other five branches unchanged. (Tip SHAs
  in this paragraph are stale post-cross-shard-cleanup; see the
  next entry for current tips.)
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
  (Now 240 + 44 after #218–#240 — #220–#221 added when Pass B
  reuse review surfaced catalog gaps while landing #79 / #85;
  #222–#224 surfaced by the post-audit follow-up sweep around #128;
  #225–#226 surfaced by the May 2026 Pass-B sweep on #53 and #97
  respectively; #227–#233 filed as Batch D from the May 2026
  whole-branch holistic-review pass; #234–#238 filed by the
  post-Batch-G adversarial-audit pass after the user asked for an
  inventory of unfiled opportunities — auditor confirmed three
  items genuinely lacked tracking numbers and named two more
  whose framing/SHA had errors but were also untracked. #239–#240
  filed during the May 2026 cross-shard cleanup that revoked two
  cross-shard commits on the libstore branch — the libutil header
  halves moved to the libutil branch and the libstore consumer
  halves are now tracked as queued work blocked on libutil
  upstream merge.)
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
| `vibe-coding/cleanup/libexpr` | libexpr | #217, #64, #66, #68, #106, #107, #109, #110 (extended to forceValueDeep), #112, #196, #199, #67, #79 (3 of 4 sites; `prim_fetchTree` excluded as misclassified — its loop dispatches on value type, not name), #105 (with doxygen `EXPAND_AS_DEFINED` follow-up), #214, #220, #192, #202+#203 (folded — drop failure-cache machinery, bump v6→v7, wrap `getAttr` in `doSQLite`, rl-next entry), #204 (drop `MultiEvalProfiler`; ctor-frozen `profilerHooks` cache); plus Batch-C doc fixes (lexer.l ID retarget, primop diagnostic rl-next); plus Batch-E compound-win cleanups #227 (drop dead `EvalProfiler` NVI cache + default no-op virtuals; rl-next entry), #228 (extend `NumOp` to `BitAnd`/`BitOr`/`BitXor`), #229 (parameterise `SeenSet` template + migrate underlying container to `boost::unordered_flat_set`); plus Sprint-1 cleanups: #87 (libexpr-c eager/lazy accessor pair dedup via private impls preserving error wording), #111 (`withRegex<Body>` template helper extracting the prim_match/prim_split regex-fetch + try/catch scaffold; `RegexCache &` parameter sidesteps the existing friend-cluster, no friend additions); plus 2026-05 audit fixes: F003 (`nix_get_external` used `check_value_out`, rejecting every valid external value — fixed to `check_value_in`) and F070 (`nix_value_call` deprecated `Value` alias → `nix_value *`) | Pushed to origin; in sync at `c3820c35d` |
| `vibe-coding/cleanup/libfetchers` | libfetchers | #52, #51 (delete-and-trim, scope expanded past the two `#if 0` blocks), #53 (latent-bug fix in `getCustomRegistry`), #230 (GitArchive subclass DRY: getHost/getOwner/getRepo + clone() promoted to base; defaultHost() + cloneUrlSuffix() per subclass; GitLab `// FIXME: get username somewhere` preserved as a class-level comment); plus Batch-C rl-next extension noting treeHash rejection | Pushed to origin; in sync at `572e8fde8` |
| `vibe-coding/cleanup/libflake` | libflake | #48, #78 (helper covers two of three DFS sites; `doFind` excluded as structurally different), #133 (rename only; deviation from prescribed end state documented) | Pushed to origin; in sync at `b29de9420` |
| `vibe-coding/cleanup/libmain` | libmain | #49, #128, #222 (`--max-freed` clamp), #223 (`getIntArg` wrapper around `getArg`); plus Batch-C max-freed-clamp rl-next entry; plus Batch-G `getIntArg` 4-arg-signature-break rl-next entry; plus 2026-05 audit fix F002 (rewrote the `max-freed-clamp.md` rl-next prose — the old `-1`-disables-cap / over-INT64_MAX-overflow description was wrong on every point and the migration advice was dangerous; code unchanged) | Pushed to origin; in sync at `998a763cc` |
| `vibe-coding/cleanup/libstore` | libstore | #54, #56, #57, #60, #4 (extended to template form), #5 (extended to template form), #6 (hoist-then-delete), #14, #47, #82, #114, #115, #157, #159, #165, #26 (Linux only — Windows untested under current CI), #46 (buildenv + unpack-channel; `fetchurl` excluded — different lookup shape), #80 (Windows behaviour change: `log-fd` now respected), #83, #85, #100 (helper-only scope), #135 (option (a) flatten), #1+#3 (BuildResult/DrvOutput/Realisation templates with cpu-timing-as-callback), #7 (`negotiateVersion` client-side helper), #11 (`tryUpperFallLower` returning `std::pair<R, bool>`), #55 (stale migration drop), #59 (`useBuildUsers` static drop), #101 (`partitionRealisedPaths` + `registerCopiedRealisations`), #153 (`DerivationBuilderImpl` composition), #185 (`ServeProto::BasicConnection` + `protoVersion` rename), #183 (drop dead `QueryDeriver` opcode arm in daemon.cc::performOp), #234 (cache parsed secret keys per-file in store-api.cc; mutation-safe wrt `secret-key-files` setting; pointer-stable via `std::unique_ptr` values to keep the lock scope at map-mutation only); plus Batch-C doc fixes (worker-protocol-connection wrong-shard wording, negotiateVersion docstring); plus Batch-G #80 Windows-`log-fd` rl-next entry. **Cross-shard cleanup** (May 2026): two libutil-touching commits dropped from this branch so it can land independently — #119's `BaseSetting<T>::overrideIfSet` libutil header addition + the `HttpBinaryCacheStore` consumer (former commit `aa00e072e`); #221's `parseSettingTokens`/`renderSettingTokens` libutil header lift + the `globals.cc` consumer (former commit `ae7167e8c`); plus the dropped #119 rl-next entry. The libutil header halves moved to the libutil branch; the libstore consumer halves were dropped and re-filed as queued candidates #239 (#119's consumer) and #240 (#221's consumer), both blocked on libutil upstream merge. **Pass-4 review** (May 2026): added `serve-protocol-protoversion-rename.md` rl-next entry disclosing #185's `remoteVersion`→`protoVersion` field rename in shipped `serve-protocol-connection.hh` (a public-API break for Hydra/Lix/plugin consumers; the gap was the same shape as Batch-G's systematic disclosure-fix pass and fell through); plus 2026-05 audit fixes: F052 (`nix_store_path_hash` missing the `last_err_code = NIX_OK` entry preamble every sibling has) and #246/F027 (drop dead `PathSubstitutionGoal::gotInfo`/`finished` declarations) | Pushed to origin; in sync at `410107979` |
| `vibe-coding/cleanup/libutil` | libutil | #58, #130, #29, #99, #117, #37 (helper covers one of three NAR walks; other two structurally distinct), #97 (new `parse-enum.hh`; migrated 5 parsers including file-content-address pair from Pass-B sweep; folded in two latent-bug fixes — `parseHashFormat` `nix32` omission and `parseFileSerialisationMethod` typo), #124 (drop `SyncBase`; `Sync<T>::ConstLock` + `mutable mutex`), #167 (`io-buffer-sizes.hh` + 6 site migrations), #224 (`CanonPath::numSegments` + 4 migrations), #119 (`BaseSetting<T>::overrideIfSet` libutil header addition; consumer migration deferred to libstore as queued #239), #221 (`parseSettingTokens`/`renderSettingTokens` libutil header lift; consumer migration deferred to libstore as queued #240), #226 (`renderEnum<E>` helper in `parse-enum.hh` + 5 site migrations completing the parse/show symmetry from #97); plus #119 rl-next entry. Batch-G dropped #163 `PipeOptions` overload (zero in-tree consumers); Batch-G follow-up reworded #29/#99/#117 commit bodies to canonical `Refs candidate` form. **Cross-shard cleanup** (May 2026): #119 and #221 moved from libstore to here (header-only halves); the libstore consumer halves are queued as #239/#240 blocked on libutil upstream merge. **Post-Pass-2 review** (May 2026): #224 commit-message body reworded — claimed "Migrate the single existing call site in nar-listing.cc" but the diff actually migrated four sites (this had been recorded as fixed in FOLLOWUPS Batch A but a metadata-only rebase had bumped the SHA without touching the message body). All five upstream commit SHAs moved by one rebase pass. **2026-05 audit fixes:** F025 (`destroyCgroup` threw interpolating an uninitialised `pid` — UB; → `pid_s`), F065 (`printCodeLines` indent loop → `std::string` fill), F066 (shebang parser redundant `make_shared<T>(T(…))` temporaries), F067 (`ExperimentalFeatureSettings::isEnabled` linear `std::find` → `std::set::contains`) | Pushed to origin; in sync at `a16c0a75a` |
| `vibe-coding/cleanup/nix-cli` | `src/nix/` (modern + legacy CLI) | #127, #142, #24, #77, #89, #91, #187, #74, #75, #72 (free helpers, not mixin — diamond inheritance), #93 (3 of 4 parents; `CmdHash` retained inline deliberately — see #236 declined), #141 (per-command `static`-ify subset only — extended to `removeOldGenerations` after Pass A), #231 (`nix-env --priority` `getArg` substitution at `opInstall`), #235 (consolidate `printGCWarning` gating in `nix-store` and `nix-instantiate`; rl-next disclosure for the user-visible warning-on-non-LocalFSStore behaviour change), #76 (Sprint-1: `walkClosure` template helper in new file-local `nix-store/closure-walk.hh` deduplicating the dotgraph/graphml BFS scaffold; helper kept in `nix-store/` rather than promoted to `store-api.hh` to keep single-shard); plus 2026-05 audit fixes: F001 (the #231 `--priority` landing used `getArg`, which pre-increments inside the `*i++` loop and threw on every `nix-env -i --priority N pkg` — fixed to `needArg`, restoring the test-covered behaviour), F069 (`search.cc::wrap` → `static`), and a unity-build fix for #76 (`graphml.cc`'s `makeEdge`/`makeNode` collided with `dotgraph.cc`'s under the flake unity build — renamed to `makeGraphMLEdge`/`makeGraphMLNode`) | Pushed to origin; in sync at `2cbb7af04` |

As of the 2026-05-29 session, all seven cleanup branches plus the
`vibe-coding/simplifications` doc branch are **pushed to `origin`
(`ConnorBaker/nix`) and in sync** at the tips shown in the Status column
(and re-listed in the "Current tip SHAs" block below) — ready for
upstream PR creation. Three of them (libflake, libmain, nix-cli) had
their history rewritten during the value/churn pass, so any future
re-publish of those needs a force-push; the rest fast-forward. Push
policy remains explicit-only: the working trees are clean and nothing is
unpushed, but no further push happens without a fresh instruction.

## Worktree layout

The seven shard branches are checked out as worktrees under
`/home/cbaker2/ext-sources/nix-worktrees/cleanup-<shard>/`. Future
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

## Current tip SHAs (as of last edit)

| Branch | Tip |
| ------ | --- |
| `vibe-coding/cleanup/libexpr` | `c3820c35d` |
| `vibe-coding/cleanup/libfetchers` | `572e8fde8` |
| `vibe-coding/cleanup/libflake` | `b29de9420` |
| `vibe-coding/cleanup/libmain` | `998a763cc` |
| `vibe-coding/cleanup/libstore` | `410107979` |
| `vibe-coding/cleanup/libutil` | `a16c0a75a` |
| `vibe-coding/cleanup/nix-cli` | `2cbb7af04` |

(Tips advanced through the 2026-05-29 audit, the follow-on value/churn
pass, and the first single-shard foundation batch. The value pass
rewrote history on three branches: libflake (reverted the #133 rename,
kept the docstring), libmain (squashed the known-wrong `--max-freed`
rl-next into its rewrite), nix-cli (squashed the `--priority` regression
into its fix). The foundation batch then appended new commits: libutil
+3 (#259, #243, #244), libexpr +2 (#87 comment, #252), libflake +1
(#258). All `nix build -L .` green on their current tips. PUSH MODES when
published: libflake/libmain/nix-cli need a **FORCE-push** (history
rewritten); libutil/libexpr/libstore are **fast-forward**;
libfetchers unchanged.)

The chronological narrative above carries intermediate tip claims
("Final tip SHAs after Batch G", etc.) that are no longer current.
This block is the single source of truth; verify against
`git rev-parse HEAD` on the corresponding worktree before acting on
any tip-SHA claim elsewhere in this file.
