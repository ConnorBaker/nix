# Follow-ups from May 2026 holistic-review pass

This file tracks the action items raised by the seven whole-branch
reviews launched after the GREEN+YELLOW batch landed. Each whole-branch
reviewer was asked to look beyond the recent diff and consider:
(1) cumulative cleanup unlocked by the full set of changes,
(2) candidate-reference correctness, (3) doc staleness.

The findings span correctness amends, doc-branch corrections, in-source
doc cleanups, missing release notes, new catalog candidates, and
implementation-tier compound wins. They are split into five batches
ordered by risk and by logical grouping, not by priority.

**Discipline rule:** items here do not move to "done" without the
fix landed and verified. Strike-through marks completion. New items
go at the bottom of their batch with a `(N)` tag.

---

## Batch A — branch-correctness amends

Commit-body and orphan-ref fixes on the seven cleanup branches. All
land via `git rebase -i` per shard. Self-contained; no behavioural
change to any code; safe to run independently.

- [x] ~~**libexpr** `437eea9d0` — orphan commit. Body has no
  `Refs candidate` line. Candidate #217 (in
  `21-eval-core-evalstate-value.md`) explicitly names the duplicate
  `friend struct ExprVar` decl this commit removes, and the catalog
  Branch line ascribes the work to `vibe-coding/cleanup/libexpr`.
  Amend the body to include `Refs candidate #217.`~~ Done; new SHA
  `bfc3c23a1c2` (libexpr tip `380b33252`).
- [x] ~~**libstore** `5f5b8151e` — orphan commit. Body claims
  to address #54+#56+#57+#60 per STATUS.md (the bidirectional
  audit), but `git log -1 --format=%B` shows no `Refs candidate`
  line in the actual commit message. STATUS asserts a prior amend
  pass fixed this; the fix appears lost across a later rebase.
  Amend with `Refs candidate #54 + #56 + #57 + #60.`~~ Done;
  new libstore tip `e2d92cbe0`.
- [x] ~~**libstore** `3a9406f58` body — references the
  `swallowMissingCaDerivations = true` parameter on
  `registerCopiedRealisations`. Pass-B dropped that parameter
  (the toggle was dead surface; no caller passed `false`) but the
  commit body wasn't reflowed. Amend the body to remove the
  parameter mention; the swallow is now unconditional.~~ Done.
- [x] ~~**nix-cli** `92df34fb2` body — says "prefetch.cc deferred to
  #95", but the diff actually converts `main_nix_prefetch_url`'s
  `MyArgs` to `LegacyEvalArgs`. Two consequences: the commit body
  factually misstates its own diff, and #95's remaining scope
  shrinks (only the `compatNixHash` / `parseCmdLine` shim remains).
  Amend body to read e.g. *"all four legacy entry points
  converted; #95's remaining scope is the `compatNixHash` /
  `parseCmdLine` shim only."*~~ Done; new nix-cli tip `2a34c3148`.
- [x] ~~**libutil** `e3c18fe5a` body — uses `kCompressionOutBufSize`,
  the constant name that existed mid-Pass-B before being renamed
  to `kBufferedStreamSize`. Same body says "Four sites" while the
  diff stat shows six. Re-flow body to use the post-rename name
  and the post-Pass-B count of six migrated sites.~~ Done; new
  SHA `03a269a0a`.
- [x] ~~**libutil** `d7e3fdc6d` body — says "Migrate the single
  existing call site in nar-listing.cc" but the diff migrates
  four (`nar-listing.cc`, `canon-path.cc::fromFilename`,
  `unix/file-system-at.cc`, `windows/file-system-at.cc`).
  Re-flow body to list all four sites and note that whole-tree
  `std::ranges::distance` over CanonPath now has zero hits.~~
  Done; new libutil tip `2c9005abe`.
- [x] ~~**libutil** `io-buffer-sizes.hh` docblock for
  `kDefaultIOBlockSize` lists 2 of 6 actual users (says
  "`Source::drainInto` and `tarfile.cc`'s default libarchive
  read/write buffer"; missing `file-system.cc::writeFile`,
  `file-descriptor.cc::drainFD`, `file-descriptor.cc::copyFdRange`,
  and the second `tarfile.cc` ctor). Amend the docblock; lands on
  the libutil cleanup branch as part of the same commit-body
  reflow above.~~ Done; folded into the same #167 amend.

---

## Batch B — doc-branch corrections

Edits to `doc/inventory/candidates/*.md` and `doc/inventory/STATUS.md`
on `vibe-coding/simplifications`. No cleanup-branch impact.

- [x] ~~**#159 Branch line** in `12-libutil-libstore-core-extras.md`
  says "`std::queue<ChildEvent>` plus `bool timedOut`; pop order
  preserved by FIFO insertion semantics; the old
  `assert(!childEOF)` defensive check was dropped". Actual
  implementation in `goal.{cc,hh}` uses `std::deque<ChildEvent>`,
  pop order preserved by an explicit `std::find_if` over the deque
  draining `ChildOutput` events ahead of markers, and a new
  `assert(events.size() <= childEventsHighWatermark)` (high-watermark
  16) replaces the dropped EOF assertion. Three points stale; fix
  Branch line to match shipped code.~~ Done. (Branch line lives
  in `16-libstore-build-audit.md`, not the file the reviewer
  named — found via grep.)
- [x] ~~**#226 candidate body** in
  `12-libutil-libstore-core-extras.md` cites only `printHashAlgo` /
  `printHashFormat` / `showCompressionAlgo`. Two sibling
  switch-based renderers exist in the same file
  (`file-content-address.cc:35-58`):
  `renderFileSerialisationMethod` and `renderFileIngestionMethod`.
  Both already have `EnumNames<E>` tables in the file (added by
  #97). Expand #226's body to cite all five sites and explicitly
  include the file-content-address pair as scope. The proposed
  `renderEnum<E>(E, EnumNames<E>) -> std::string_view` helper
  unifies all five.~~ Done.
- [x] ~~**#148 Branch line** in `07-dead-stale-code.md` (or wherever
  it lives — verify the file) names a "primary commit message"
  that doesn't exist as a singular concept. The actual
  attempt-and-revert is documented in `efd3e2613`'s third
  paragraph. Fix by naming the SHA `efd3e2613` directly.~~ Done;
  Branch line lives in `15-globals-settings.md`. Names commit
  `efd3e2613` and its third paragraph explicitly.
- [x] ~~**#128 Branch line** in `11-legacy-cli.md` flags the
  `#49+#128` bundling as "soft style issue", reading as
  if-not-fixed. The bundling shipped that way and #222/#223
  landed as separate commits with no further pushback. One-line
  nuance edit clarifying that the bundling was accepted, so
  future readers don't reopen the discussion.~~ Done. (Edit
  was in `14-vestigial-stdlib.md`, where #128's body actually
  lives — the candidate-body framing reads "History note"
  rather than "Note (per pass-2 finding)" now.)
- [x] ~~**#225 candidate body** (filed last batch) — add a note
  that any mtime-keyed cache helper introduced to fix
  `getUserRegistry`/`getSystemRegistry`/`getGlobalRegistry`
  belongs in libutil, not libfetchers, since it is a pure
  FS-stat-keyed memo of `T(Path)`. Keeps a future implementor
  from cross-locating the helper.~~ Done.
- [x] ~~**#89 Branch line** in `11-legacy-cli.md` — same mistake as
  the nix-cli commit body in Batch A: says "prefetch.cc deferred
  to #95" when prefetch.cc was actually migrated. Update Branch
  line to reflect that all four legacy entry points
  (`nix-build`, `nix-instantiate`, `nix-env`, `nix-prefetch-url`)
  were converted.~~ Done. Also added a propagation note to #95's
  Validation paragraph noting the scope shrunk (only the
  `compatNixHash` shim remains).

---

## Batch C — in-source doc staleness and missing rl-next entries

Source-side doc edits on the cleanup branches plus `rl-next/` entries
for user-visible behaviour changes that didn't get release notes.

### In-source comment fixes

- [x] ~~**libexpr** `lexer.l:105-107` says "If this regex changes,
  update `nix::isVarName` in src/libexpr/print.cc to match — it
  is the print-side mirror of this rule …". After #109, the
  canonical declaration with the lexer-mirror invariant lives in
  `src/libexpr/include/nix/expr/print.hh`. Retarget the pinning
  comment from `print.cc` to `print.hh`.~~ Done in `2064c3a35`.
- [x] ~~**libstore** `worker-protocol-connection.hh` — both
  `operator WorkerProto::ReadConn()` and
  `operator WorkerProto::WriteConn()` doxygen blocks read
  "easy to use the factored out **serve protocol** serializers
  with a `LegacySSHStore::Connection`". Wrong shard. Pre-existing
  drift (the branch did not modify this header) now visible
  because both protocols sit side-by-side after #185. Same
  Worker docstring also claims "The serve protocol connection
  types are unidirectional, unlike this type" — same problem.
  Replace "serve" → "worker" (and verify the unidirectional
  claim against the post-#185 code).~~ Done in `3123c3c9f`.
  The unidirectional-claim sentence was rewritten to refer to
  "this bidirectional connection type" since the prior framing
  was load-bearingly tied to the wrong-shard claim.
- [x] ~~**libstore** `common-protocol.hh` — `negotiateVersion`
  helper documents the `localMin` lower-bound but doesn't
  explain why the server-side handshakes
  (`worker-protocol-connection.cc:201`,
  `serve-protocol-connection.cc:33`) call bare `std::min`
  instead. Add one sentence explaining "servers accept any
  client version and don't need the lower-bound check".~~
  Done in `3123c3c9f` (same commit as the worker-coercion fix).
- [x] ~~**libfetchers** `github.cc:328-329` FIXME ("we may want
  to require a Git tree hash instead of a NAR hash") is mildly
  orphaned by #51's deletion of the `RefInfo::treeHash`
  scaffolding. Comment isn't wrong, but a reader walking via
  `git blame` finds no obvious starting point. If the FIXME
  region is touched in any future commit, append "(an earlier
  scaffolding for this was removed in #51 as it never wired
  up the upstream-tree-hash lookup)".~~ Deferred per the
  reviewer's own conditional framing ("if the FIXME region is
  touched"). Editing only the FIXME annotation would be the
  sole touch on libfetchers' source for this batch — borderline
  cost-benefit. The next code change in the region picks it up.

### rl-next entries

- [x] ~~**libmain** `--max-freed` user-visible behaviour change
  (#222): previously `--max-freed -1` silently disabled the cap;
  now errors at parse. Values above `INT64_MAX` previously
  silently disabled; now honoured up to `UINT64_MAX`. Affects
  both `nix-store --gc --max-freed` and `nix-collect-garbage
  --max-freed`. Add `doc/manual/rl-next/max-freed-clamp.md` (or
  similar) on the libmain cleanup branch.~~ Done in `0b66d94ce`
  as `doc/manual/rl-next/max-freed-clamp.md`.
- [x] ~~**libfetchers** `treeHash` attribute now rejected by
  `allowedAttrs` enforcement on github/gitlab/sourcehut inputs
  (#51). Previously silently accepted and ignored; now throws
  on parse. Extend the existing
  `doc/manual/rl-next/github-fetcher-param-validation.md` (which
  already documents `tag` rejection) with one line covering
  `treeHash`.~~ Done in `d1a856c3f`.
- [x] ~~**libexpr** diagnostic position + ANSI colouring shift
  across `prim_path` (#220) and the four fetcher primops
  migrated under #79. Diagnostics previously anchored at the
  call site; now anchor at the offending attribute. Primop name
  ANSI rendering also shifted from baked-into-format-string to
  `'%s'` interpolation. Add an rl-next entry covering the four
  primops and the diagnostic-position promise.~~ Done in
  `2064c3a35` as
  `doc/manual/rl-next/primop-diagnostic-position.md`.

---

## Batch D — new catalog candidates from review findings

Each follow-up below becomes a real numbered catalog entry on the
doc branch, continuing from #226. Filing them makes the work
visible in the "Queued / blocked" surface so it doesn't get
forgotten.

- [x] ~~**#227 — `EvalProfiler`'s NVI cache + default-no-op virtuals
  are dead post-#204.** With `profilerHooks` snapshotted into
  `EvalState` once at construction, the
  `private std::optional<Hooks> neededHooks` cache, the
  `getNeededHooksImpl` indirection, and the no-op default
  `pre/postFunctionCallHook` bodies in `eval-profiler.cc` are
  all unreachable. Both subclasses (`FunctionCallTrace`,
  `SampleStack`) override the hooks. Drop the cache, collapse
  the NVI to one direct virtual, mark hook virtuals pure.
  `eval-profiler.hh` ships via `install_headers`, so this is a
  public-API tightening — coordinate with rl-next. Effort:
  small. **See also:** #204.~~ Filed in
  `20-eval-core-cache-attrset-profiler.md`.
- [x] ~~**#228 — `bitAnd`/`bitOr`/`bitXor` are an unfinished twin
  family adjacent to `primNumeric<NumOp>`.** Three lines below
  the new template, `prim_bitAnd` / `prim_bitOr` / `prim_bitXor`
  (`primops.cc:4587-4635`) sit as byte-identical free functions
  differing only by `&`/`|`/`^`. Each forces both args via
  `forceInt`, applies the op, emits an int. Extend the `NumOp`
  enum with `BitAnd`/`BitOr`/`BitXor`, give each row a
  `firstIntCtx`/`secondIntCtx`, let `primNumeric<Op>` handle
  int-only as a third arm. Effort: small. **See also:** #67.~~
  Filed in `08-duplicated-parsers.md`. (Line numbers replaced
  with "adjacent to" framing per the no-line-numbers rule.)
- [x] ~~**#229 — `SeenSet = set<const void *>` widening
  type-erases at the boundary.** Promoted in #110 to handle
  `printAmbiguous` and `getDerivations::done`'s parallel
  visited sets. The `void *` widening means anyone adding a
  fourth caller can insert any pointer kind without compile
  enforcement. Promote to a parameterised
  `template<class T> using SeenSet = std::set<const T *>;`
  and instantiate per call site
  (`SeenSet<Bindings>`, `SeenSet<Value>`). Preserves the
  consolidation while keeping each caller's element type
  pinned. **See also:** #110, the analogous pointer-keyed
  visited sets in libstore/libutil if any.~~ Filed in
  `13-libexpr-extras.md`.
- [x] ~~**#230 — GitArchive subclass DRY-ing post-#51 trim.**
  After #51 trimmed `RefInfo` to a single `Hash` field, the
  duplication across `GitHubInputScheme`, `GitLabInputScheme`,
  `SourceHutInputScheme` becomes legible: host default
  repeated 7×, `clone()` near-clone across all three (differs
  only in host default and `.git` suffix), `getOwner`/`getRepo`
  helpers exist only on GitHub but the others inline the same
  `getStrAttr(input.attrs, "owner"|"repo")` 4-6×, `getDownloadUrl`
  shape uniform with only the format string varying. Promote
  to a base virtual with per-scheme overrides for the
  load-bearing differences. Effort: small. **See also:** #51.~~
  Filed in `03-repeated-boilerplate.md`.
- [x] ~~**#231 — `nix-env --priority` open-codes the
  `getArg`+`string2Int` pattern.** `src/nix/nix-env/nix-env.cc::
  opSetFlag` does manual end-check + throw +
  `string2Int<int>(*i++)`. After libmain #223 turned `getIntArg`
  into a one-line wrapper around `getArg`, this is the only
  in-tree site still hand-rolling the pattern. Cannot use
  `getIntArg` directly (different `string2Int` vs
  `string2IntWithUnitPrefix`, and `nullopt` handling) but is a
  one-liner via `getArg(arg, i, opFlags.end())` +
  `string2Int<int>`. Effort: trivial. **See also:** #128, #223.~~
  Filed in `11-legacy-cli.md`.
- [x] ~~**#232 — `ignoreExceptionInDestructor` non-destructor
  audit.** ~14 invocation sites in libstore alone (counted via
  `grep -rn` from libstore reviewer). Most are genuinely in
  destructors; some are not. The libutil branch shipped #58
  converting one libutil non-destructor site (`getMaxCPU`) to
  `ignoreExceptionExceptInterrupt`. Walk all remaining sites
  in libstore and reclassify any that aren't destructors.
  Effort: small. **See also:** #58.~~ Filed in
  `07-dead-stale-code.md`.
- [x] ~~**#233 — Schema-migration-style scaffolding repeats
  thrice (cross-shard).** `LocalStore::upgradeDBSchema`'s
  `SchemaMigrations` table + `doUpgrade` closure pattern,
  `nar-info-disk-cache.cc::LastPurge` table-keyed periodic
  refresh, and `eval-cache.cc::_state`'s mutex-guarded
  "have we initialised this row" check are all variants of
  "named-once-and-marked-done bookkeeping". After
  `LocalStore::upgradeDBSchema` collapsed to one migration
  (post-#55), the scaffolding's value-density is borderline.
  Lift into a shared helper, or note explicitly that the
  three are deliberately separate. **See also:** N13
  (cache-base / doSQLite consistency), #168 (cache schema
  versions). Effort: medium.~~ Filed in `17-cross-cutting.md`.

(Optional for a future filing pass — not blocking on this round:)

- [ ] (N) **DarwinDerivationBuilder** thin enough to fold
  post-#153 — only two override methods + one bool.
  Platform-specific bodies are the genuine reason to keep the
  subclass; possibly absorb the bool into `LocalSettings`. File
  if/when the build-platform dispatch story is revisited.
- [ ] (N) **Worker `BasicConnection` operator-coercion
  comments** flag the asymmetry that makes a
  `BasicConnection<Proto>` template ill-advised. Annotate the
  catalog entry that lists this as a future template
  consolidation (any candidate that proposes the template
  should know two reasons to decline, not one — the `Version`
  storage shape *and* the server-side scaffolding asymmetry).
- [ ] (N) **`forEachConfiguredSigner` cannot cleanly absorb
  `keys.cc::getDefaultPublicKeys`** despite shape match —
  exception narrative differs. File a "considered, declined"
  note next to #100's Branch line so a future investigator
  doesn't repeat the analysis.
- [ ] (N) **`UnkeyedValidPathInfo` is not lift-eligible for
  `common-protocol-impl.hh`** despite shape match — wire
  format genuinely differs (Worker omits "downloadSize",
  encodes `narHash` differently). Flag in the candidate body
  for #2 so the next reader doesn't try to extend the lift.

---

## Batch E — compound-win cleanups (code changes)

Implementation-tier work. Each lands on its shard branch as a new
commit with full A/B/C/D review chain. Deferred until A-D are
clean and the relevant new candidates (#227, #228, #229) have
catalog entries.

- [x] ~~**#227 implementation** — drop the dead `EvalProfiler`
  NVI cache infrastructure (libexpr).~~ Landed as
  `c199262e4` on `vibe-coding/cleanup/libexpr`. Drops the
  cache field, renames `getNeededHooksImpl` → `getNeededHooks`,
  marks all three hooks pure. Public-API tightening; rl-next
  entry already covers it under #204.
- [x] ~~**#228 implementation** — extend `NumOp` enum to bit ops,
  collapse `prim_bitAnd`/`prim_bitOr`/`prim_bitXor` (libexpr).~~
  Landed as `8ffe04cd1`. `NumOp` extended with
  `BitAnd`/`BitOr`/`BitXor`; `primNumeric` gets an `isIntOnly<Op>`
  arm. Three free functions deleted; their `RegisterPrimOp`
  blocks now point at `primNumeric<NumOp::Bit*>`. Error wording
  preserved byte-for-byte.
- [x] ~~**#229 implementation** — parameterise `SeenSet` template
  alias (libexpr).~~ Landed as `59cae92f8`. Added
  `template<class T> using TypedSeenSet = std::set<const T *>;`
  alongside the existing `SeenSet` (now an alias for
  `TypedSeenSet<void>`). `EvalState::forceValueDeep` migrated
  to `TypedSeenSet<Value>`; `getDerivations`'s `Done` migrated
  to `TypedSeenSet<Bindings>`. The mixed-pointer-kind printer
  consumers stay on the void-typed alias since they
  legitimately mix `Bindings *` and `Value *`. Templated
  `dedupe` overload added; void-typed compatibility overload
  preserved.
- [ ] **Cross-shard #167 extension** — migrate
  `local-store.cc::addToStoreFromDump`'s chunk size and
  `derivations.cc::Derivation::unparse`'s string reservation
  to use `kDefaultIOBlockSize` from libutil's
  `io-buffer-sizes.hh`. **Blocked on libutil landing upstream.**
  Implementation attempted in this batch and reverted: the
  libstore cleanup branch builds against upstream master's
  installed nix-util headers, where `io-buffer-sizes.hh` does
  not yet exist. Two paths to unblock:
  (a) wait for the libutil cleanup branch (tip `2c9005abe`)
  to land upstream, then implement this on libstore;
  (b) rebase the libstore cleanup branch onto a worktree
  that has libutil's changes available — invasive and changes
  the merge story. (a) is the documented pattern. **See also:**
  #167.
- [ ] **Cross-shard #124 extension** — migrate `daemon.cc`'s two
  read-only `tunnelLogger->state_.lock()->canSendStderr` sites
  from `lock()` to `readLock()` to express read-only intent
  via the new `Sync<T>::ConstLock` type. **Blocked on libutil
  landing upstream** for the same reason as the #167 extension:
  the libstore cleanup branch builds against upstream master's
  `sync.hh`, which doesn't yet have `ConstLock`. Land after
  libutil merges. While doing this, also survey other libstore
  `lock()` call sites for similar read-only opportunities.
  **See also:** #124.

---

## Batch F — clang-tidy regression check + boost-flat container audit

Added after Batch E's libexpr commits to verify no new clang-tidy
errors in changed files and to audit data-structure choices.
Sequential per shard (clang-tidy on parallel worktrees overloads the
host).

- [x] ~~**Run clang-tidy sequentially on every shard with new commits**
  via `nix develop <worktree> --command ninja -C build clang-tidy`.
  Compare against the post-Batch-A baseline (5 known pre-existing
  master errors: 4× rapidcheck/Gen.hpp, 1× libcmd/network-proxy.cc).
  Any new error in changed files needs to be fixed before landing.
  Shards to check: libexpr (3 new Batch-E commits), libstore (1
  new Batch-C commit), libutil (1 Batch-A docblock amend),
  libmain (1 new Batch-C commit), libfetchers (1 new Batch-C
  commit). libflake and nix-cli had only Batch-A rebases, no
  new commits, so the prior clean clang-tidy still holds.~~ Done.
  All seven shards exhibit exactly five errors — the same five
  pre-existing master errors. **No clang-tidy regressions across
  Batches A through E.**
- [x] ~~**Audit data-structure choices in changed files.** Strong
  project preference: `boost::unordered_flat_map`/`unordered_flat_set`
  (and the flat-tree containers) over STL `std::map`/`std::set`/
  `std::unordered_map`/`std::unordered_set`. Walk every Batch-E
  diff (in particular #229's `TypedSeenSet = std::set<...>`
  alias and #228's `NumOpDiag` table — though `NumOpDiag` is
  not a container) and flag any STL container that should
  switch. Note: `std::set` for a "seen pointers" cycle-break
  set has different cost characteristics from `boost::unordered_flat_set`
  (deterministic iteration order vs. faster lookup); call out
  the trade-off rather than blindly switching.~~ Done.
  **#229's `TypedSeenSet` migrated to
  `boost::unordered_flat_set<const T *>`** in the same commit
  (now `76b6808f0`). Every in-tree consumer uses the set only as
  a membership probe via `dedupe`, so the deterministic-iteration
  property of `std::set` was unused; the flat-set form gives
  amortised-O(1) lookup with locality-friendly memory layout for
  the two hot recursive walkers (`getDerivations`'s `Done` and
  `forceValueDeep`'s `seen`). **Other newly-added STL containers
  audited and kept**: libstore wire-format `std::set<StoreReference>`
  / `std::map<OutputName, UnkeyedRealisation>` / `std::set<Signature>`
  are wire-format types (deterministic iteration is load-bearing
  for stable rendering). libflake's `std::set<NodeRefT>` was
  introduced by the prior #78 helper, predates this batch, and
  is appropriately tagged as DFS-visited; can swap in a future
  pass but not in scope for Batch F. Pre-existing libexpr
  `std::map<FrameStack, uint32_t>` (eval-profiler.cc) and
  `std::map<const Hash, ref<EvalCache>>` (eval.hh) were not
  modified by Batch E and are out of scope.

## Batch G — adversarial value-review gaps (May 2026)

Spawned in parallel after Batches A-F closed; one reviewer per shard
asked: "Does each commit meaningfully help/simplify/improve/correct
the code, or is it effectively churn? Does it unlock further
simplifications? Are semantics preserved? Why?". Aggregate verdict
across the seven shards: **substantive value, not churn**, with a
small set of disclosure / under-documentation gaps and one actual
churn case. The four gaps below close those out.

- [x] ~~**libutil #163 — revert the `Pipe::create(PipeOptions)`
  overload.** Reviewer flagged this as actual churn: zero in-tree
  consumers of either the new overload or the legacy
  `pipe.create(true)` form. The Unix `nonBlocking` parameter was
  already dead. The overload was speculative public-header surface
  for hypothetical future code, not a fix for any present-day call
  site. Drop the commit (`e3f228714`) via `git rebase -i master`.
  Update #163's Branch line on the doc branch to record
  "landed and reverted" rather than "additive overload available".
  The design (`PipeOptions{.nonBlocking = ...}` ignored on Windows
  or implemented via `SetNamedPipeHandleState`) is still right;
  re-file when an actual consumer needs the cross-platform shape.~~
  Done. Commit dropped via non-interactive rebase
  (`GIT_SEQUENCE_EDITOR=sed -i.bak '/e3f228714/s/^pick/drop/'`).
  New libutil tip `7399593ce`. clang-tidy + `nix build -L .` post-
  revert green (5 known pre-existing errors only). #163 Branch
  line updated on `vibe-coding/simplifications`.
- [x] ~~**libstore #80 — missing rl-next entry for the Windows
  `log-fd=N` behavioural change.** `legacy-ssh://` URL parameter
  was silently dropped on Windows pre-#80 because
  `LegacySSHStoreConfig::logFD` was a plain `Descriptor` field
  with no parser; #80 promoted it to a `Setting<Descriptor>` so
  the URL parameter is now parsed via `toDescriptor`. This is a
  user-visible behaviour change on Windows: configurations that
  previously set `log-fd=N` expecting it to be ignored will now
  honour it. Add `doc/manual/rl-next/legacy-ssh-store-windows-logfd.md`
  on the libstore cleanup branch.~~ Done. Committed as
  `a3ed63c01` on `vibe-coding/cleanup/libstore`. clang-tidy +
  `nix build -L .` green.
- [x] ~~**libmain — missing rl-next entry for the `getIntArg<N>`
  4-argument signature break.** `src/libmain/include/nix/main/shared.hh`
  is shipped via `install_headers`. The previous signature took a
  trailing `bool allowUnit` parameter; the migration dropped it
  outright (the body always parses unit suffixes; both in-tree
  callers passed `true`, so the parameter was dead surface).
  Out-of-tree consumers — Hydra, Lix, plugins — that pass a fourth
  argument will fail to compile until they drop it. Add
  `doc/manual/rl-next/getIntArg-allowunit-removed.md` on the libmain
  cleanup branch. Same rl-next entry should also note the related
  `nix::blockInt` extern declaration removal (never defined
  anywhere).~~ Done. Committed as `b1a14158f` on
  `vibe-coding/cleanup/libmain`. `nix build -L .` green.
- [x] ~~**libutil #97 — capture two latent-bug fixes folded in via
  the migration.** Adversarial review of the `parseEnum` migration
  found two pre-existing latent bugs that were silently fixed by
  regenerating the error-message tail from the source-of-truth
  `EnumNames<E>` table: (a) `parseHashFormat`'s pre-batch error
  tail listed `'base16', 'base32', 'base64', or 'sri'` but the
  actual accepted set also included `'nix32'` — the new generated
  tail enumerates the table directly, so `'nix32'` is now correctly
  listed; (b) `parseFileSerialisationMethod`'s pre-batch error
  message had a typo (`"file serialiation method"`) that the
  migration silently corrected to `"file serialisation method"`.
  Neither was the candidate's stated goal; both are genuine bug
  fixes that emerged from the unification. Update #97's Branch
  line on the doc branch to call them out so future readers know
  the migration carried bug-fix freight beyond the consolidation.~~
  Done. #97 Branch line on `vibe-coding/simplifications`
  expanded with both bug-fix bullets.

### Batch G follow-ups (post-audit)

A bidirectional traceability audit run after Batch G's commit
landed surfaced two more items, both addressed.

- [x] ~~**Phantom claim in STATUS.md cleanup-branches table.**
  `#221` was credited to the libutil row, but the actual commit
  (`ae7167e8c` "libutil/configuration: lift parseSettingTokens /
  renderSettingTokens (#221)") lives on the libstore branch — it
  edits a shipped libutil header but the consumer it unblocks is
  libstore's `globals.cc`. Re-shelve from libutil row to libstore
  row with cross-shard notes on each side.~~ Done in `0bdf836ea`.
- [x] ~~**Missing rl-next entry for `BaseSetting<T>::overrideIfSet`.**
  Commit `aa00e072e` on `vibe-coding/cleanup/libstore` added a new
  public member function on `BaseSetting<T>` in the shipped
  `configuration.hh` header. The addition is purely additive (no
  breakage for out-of-tree consumers) but matches the disclosure
  standard applied to other shipped-header changes in this batch.
  Add `doc/manual/rl-next/baseSetting-overrideIfSet.md` on the
  libstore cleanup branch.~~ Done. Committed as `b9a750f00` on
  `vibe-coding/cleanup/libstore`. `nix build -L .` green.
- [x] ~~**Stylistic non-canonical commit-body form on three libutil
  commits.** Bodies on `4212ac8c7` (#29), `7bfd6d15b` (#99), and
  `fe96867de` (#117) used `Candidate #NN in doc/inventory/candidates/.`
  instead of the canonical `Refs candidate #NN.`. Reword via
  non-interactive `git rebase -i master` with sed-based
  GIT_SEQUENCE_EDITOR + GIT_EDITOR.~~ Done. New libutil tip
  `da1e0d7d9` (no tree change; metadata only). clang-tidy and
  `nix build -L .` post-reword green. Three rewritten SHAs:
  `34b02c3f7` (was `4212ac8c7`, #29), `309faf880` (was `7bfd6d15b`,
  #99), `b0574ddc5` (was `fe96867de`, #117).

### Batch G follow-ups (adversarial-audit second pass)

The user asked for an inventory of unfiled opportunities and asked
the auditor to scrutinise everything I claimed was untracked. The
auditor confirmed three items genuinely lacked candidate numbers
and named two more whose framing/SHA had errors but were also
untracked. All five filed as #234-#238 on the doc branch.

- [x] ~~**#234 — `Store::signPathInfo` / `Store::signRealisation`
  re-read every secret-key file from disk on every signing call;
  #100's helper-only landing intentionally deferred the cache.**
  FIXME comment in `store-api.cc` is the canonical pointer; #100's
  body explicitly chose "helper extraction only" as the landing
  scope. Filed in `12-libutil-libstore-core-extras.md`.~~
- [x] ~~**#235 — `addPermRoot` GC-root-naming has three behavioural
  divergences across the two legacy CLI sites that #91's helper
  extraction deliberately left unresolved.** Counter-increment
  timing, `absPath` timing, and `printGCWarning` gating each
  diverge between `nix-store` and `nix-instantiate`. Filed in
  `11-legacy-cli.md`.~~
- [x] ~~**#236 — `CmdHash` is the only `getCommandsFor`-eligible
  `NixMultiCommand` that can't migrate without a registry-with-args
  mechanism.** #93's Branch line names this as deferred but no
  separate candidate tracked it. Filed in `11-legacy-cli.md`.~~
- [x] ~~**#237 — Two libstore call sites still use bare `65536`
  literals for I/O buffer sizing where #167's `kDefaultIOBlockSize`
  is the canonical name.** Cross-shard, blocked on libutil
  upstream merge per Batch-E ledger. Now has a tracking number
  beyond the FOLLOWUPS-ledger entry. Filed in
  `17-cross-cutting.md`.~~
- [x] ~~**#238 — `GitArchiveInputScheme::isLocked` carries an
  in-source FIXME proposing Git tree hashes as the locking
  primitive instead of NAR hashes.** Predates #51 and is
  independent of it (different function, different scope).
  Now has a tracking number rather than living only in the
  source FIXME. Filed in `19-eval-core-fetcher-lookup-json.md`.~~

(Optional, deferred — the reviewers also surfaced these but they
are non-blocking and either moot or low-value:)

- [ ] (N) **libexpr #79 exception-type cacheability shift** — a
  Pass-A finding noted that #79's migration to `EvalErrorBuilder`
  shifted the cached error type (subtle effect on `eval-cache`'s
  error-replay round-trip). Pass-A landed an amend; subsequent
  follow-ups on libexpr have not regressed. End-state moot but
  worth noting in the candidate body if a future reader encounters
  the cache-replay machinery.

---

## Batch tracking / sequencing

- **A → B → C in any order.** Independent fixes; no behavioural
  cross-dependencies. Recommend A first (commit history is the
  most fragile to context loss) then B (catalog corrections,
  doc-branch only) then C (in-source comments + rl-next).
- **D after A-C.** Filing the new candidates touches the same
  doc-branch files as B, so easier to do after B's edits land.
- **E after A-D.** Implementation work needs the catalog
  entries (#227-#233) in place so commits can `Refs candidate`
  them.
- **F after E.** Verification of Batch-E correctness.

Push policy unchanged: explicit-only, per-commit. None of the
batches push automatically.

---

## Source of these findings

The seven whole-branch reviewers ran on these tip SHAs:

- libexpr `adc7301d1`
- libfetchers `f39f6486e`
- libflake `fd6749635`
- libmain `8f2494f61`
- libstore `075a0479a`
- libutil `d7e3fdc6d`
- nix-cli `ef3236827`

Their full transcripts live in the conversation log; the
condensed findings are above. If any item below is unclear,
the source-of-truth is the relevant reviewer's full report,
which can be regenerated by re-running the holistic review
prompt from the conversation that produced this document.
