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

- [ ] **libexpr** `lexer.l:105-107` says "If this regex changes,
  update `nix::isVarName` in src/libexpr/print.cc to match — it
  is the print-side mirror of this rule …". After #109, the
  canonical declaration with the lexer-mirror invariant lives in
  `src/libexpr/include/nix/expr/print.hh`. Retarget the pinning
  comment from `print.cc` to `print.hh`.
- [ ] **libstore** `worker-protocol-connection.hh` — both
  `operator WorkerProto::ReadConn()` and
  `operator WorkerProto::WriteConn()` doxygen blocks read
  "easy to use the factored out **serve protocol** serializers
  with a `LegacySSHStore::Connection`". Wrong shard. Pre-existing
  drift (the branch did not modify this header) now visible
  because both protocols sit side-by-side after #185. Same
  Worker docstring also claims "The serve protocol connection
  types are unidirectional, unlike this type" — same problem.
  Replace "serve" → "worker" (and verify the unidirectional
  claim against the post-#185 code).
- [ ] **libstore** `common-protocol.hh` — `negotiateVersion`
  helper documents the `localMin` lower-bound but doesn't
  explain why the server-side handshakes
  (`worker-protocol-connection.cc:201`,
  `serve-protocol-connection.cc:33`) call bare `std::min`
  instead. Add one sentence explaining "servers accept any
  client version and don't need the lower-bound check".
- [ ] **libfetchers** `github.cc:328-329` FIXME ("we may want
  to require a Git tree hash instead of a NAR hash") is mildly
  orphaned by #51's deletion of the `RefInfo::treeHash`
  scaffolding. Comment isn't wrong, but a reader walking via
  `git blame` finds no obvious starting point. If the FIXME
  region is touched in any future commit, append "(an earlier
  scaffolding for this was removed in #51 as it never wired
  up the upstream-tree-hash lookup)".

### rl-next entries

- [ ] **libmain** `--max-freed` user-visible behaviour change
  (#222): previously `--max-freed -1` silently disabled the cap;
  now errors at parse. Values above `INT64_MAX` previously
  silently disabled; now honoured up to `UINT64_MAX`. Affects
  both `nix-store --gc --max-freed` and `nix-collect-garbage
  --max-freed`. Add `doc/manual/rl-next/max-freed-clamp.md` (or
  similar) on the libmain cleanup branch.
- [ ] **libfetchers** `treeHash` attribute now rejected by
  `allowedAttrs` enforcement on github/gitlab/sourcehut inputs
  (#51). Previously silently accepted and ignored; now throws
  on parse. Extend the existing
  `doc/manual/rl-next/github-fetcher-param-validation.md` (which
  already documents `tag` rejection) with one line covering
  `treeHash`.
- [ ] **libexpr** diagnostic position + ANSI colouring shift
  across `prim_path` (#220) and the four fetcher primops
  migrated under #79. Diagnostics previously anchored at the
  call site; now anchor at the offending attribute. Primop name
  ANSI rendering also shifted from baked-into-format-string to
  `'%s'` interpolation. Add an rl-next entry covering the four
  primops and the diagnostic-position promise.

---

## Batch D — new catalog candidates from review findings

Each follow-up below becomes a real numbered catalog entry on the
doc branch, continuing from #226. Filing them makes the work
visible in the "Queued / blocked" surface so it doesn't get
forgotten.

- [ ] **#227 — `EvalProfiler`'s NVI cache + default-no-op virtuals
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
  small. **See also:** #204.
- [ ] **#228 — `bitAnd`/`bitOr`/`bitXor` are an unfinished twin
  family adjacent to `primNumeric<NumOp>`.** Three lines below
  the new template, `prim_bitAnd` / `prim_bitOr` / `prim_bitXor`
  (`primops.cc:4587-4635`) sit as byte-identical free functions
  differing only by `&`/`|`/`^`. Each forces both args via
  `forceInt`, applies the op, emits an int. Extend the `NumOp`
  enum with `BitAnd`/`BitOr`/`BitXor`, give each row a
  `firstIntCtx`/`secondIntCtx`, let `primNumeric<Op>` handle
  int-only as a third arm. Effort: small. **See also:** #67.
- [ ] **#229 — `SeenSet = set<const void *>` widening
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
  visited sets in libstore/libutil if any.
- [ ] **#230 — GitArchive subclass DRY-ing post-#51 trim.**
  After #51 trimmed `RefInfo` to a single `Hash` field, the
  duplication across `GitHubInputScheme`, `GitLabInputScheme`,
  `SourceHutInputScheme` becomes legible: host default
  repeated 7×, `clone()` near-clone across all three (differs
  only in host default and `.git` suffix), `getOwner`/`getRepo`
  helpers exist only on GitHub but the others inline the same
  `getStrAttr(input.attrs, "owner"|"repo")` 4-6×, `getDownloadUrl`
  shape uniform with only the format string varying. Promote
  to a base virtual with per-scheme overrides for the
  load-bearing differences. Effort: small. **See also:** #51.
- [ ] **#231 — `nix-env --priority` open-codes the
  `getArg`+`string2Int` pattern.** `src/nix/nix-env/nix-env.cc::
  opSetFlag` does manual end-check + throw +
  `string2Int<int>(*i++)`. After libmain #223 turned `getIntArg`
  into a one-line wrapper around `getArg`, this is the only
  in-tree site still hand-rolling the pattern. Cannot use
  `getIntArg` directly (different `string2Int` vs
  `string2IntWithUnitPrefix`, and `nullopt` handling) but is a
  one-liner via `getArg(arg, i, opFlags.end())` +
  `string2Int<int>`. Effort: trivial. **See also:** #128, #223.
- [ ] **#232 — `ignoreExceptionInDestructor` non-destructor
  audit.** ~14 invocation sites in libstore alone (counted via
  `grep -rn` from libstore reviewer). Most are genuinely in
  destructors; some are not. The libutil branch shipped #58
  converting one libutil non-destructor site (`getMaxCPU`) to
  `ignoreExceptionExceptInterrupt`. Walk all remaining sites
  in libstore and reclassify any that aren't destructors.
  Effort: small. **See also:** #58.
- [ ] **#233 — Schema-migration-style scaffolding repeats
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
  versions). Effort: medium.

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

- [ ] **#227 implementation** — drop the dead `EvalProfiler`
  NVI cache infrastructure (libexpr).
- [ ] **#228 implementation** — extend `NumOp` enum to bit ops,
  collapse `prim_bitAnd`/`prim_bitOr`/`prim_bitXor` (libexpr).
- [ ] **#229 implementation** — parameterise `SeenSet` template
  alias (libexpr).
- [ ] **Cross-shard #167 extension** — migrate
  `local-store.cc:1173` (`addToStoreFromDump` chunk) and
  `derivations.cc:643` (`Derivation::unparse` reservation) to
  use `kDefaultIOBlockSize` from libutil's
  `io-buffer-sizes.hh`. Lands on the libstore cleanup branch
  as a new commit. **See also:** #167.
- [ ] **Cross-shard #124 extension** — migrate `daemon.cc:1110`
  (`tunnelLogger->state_.lock()->canSendStderr` — pure read)
  and `daemon.cc:1122` (`assert(!…canSendStderr)` — pure read)
  from `lock()` to `readLock()` to express read-only intent
  via the new `Sync<T>::ConstLock` type. Lands on the libstore
  cleanup branch. Survey other libstore `lock()` sites for
  similar read-only opportunities while there. **See also:**
  #124.

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
