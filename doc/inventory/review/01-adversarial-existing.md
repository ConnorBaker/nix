# Adversarial review: existing catalog

## Methodology

I read every candidate in `doc/inventory/candidates/` (sections 01-21,
217 entries) end-to-end and cross-checked every load-bearing claim against
current source under `src/`. For each entry I:

- Verified cited identifiers and file paths against the working tree
  (e.g. `grep -rn` against `src/`, `Read` of named files end-to-end where
  the mechanism claim warranted it).
- Compared the verdict against the catalog criteria in
  `candidates/README.md` ("What this catalog targets" / "What the catalog
  does **not** target") and against the verified shards under
  `doc/inventory/verified/`.
- For trivial-effort entries, traced the call graph by greps to detect
  hidden cascade (test callers, `meson.build` references, header-only
  uses, ODR pitfalls).
- Noted entries whose framing is technically accurate but mis-categorised
  (e.g. ODR-fix dressed as "stylistic relocation"), and entries whose
  category overlaps several headings.
- Cross-referenced verified-shard observations the catalog did not
  translate into a candidate; in-scope drift is reported below, and
  net-new patterns are routed to the parallel agent in the handoff
  section.

Source greps used `grep -rn` against `src/`; in cases where the catalog
claimed a count or a specific file, I re-derived the count and recorded
the discrepancy.

(This file is being streamed: sections below are appended as the review
progresses.)

## Reclassification recommendations

The catalog already does heavy lifting on verdicts. The cases below are
where I believe a verdict still needs to move.

### #50 (current verdict: VALID — Effort: trivial / "Resolved-by-design") → no change, but the row in the verdict table is misleading.

**Evidence:** The `**Validation:**` paragraph correctly reclassifies #50
as resolved-by-design ("Effort: none."). The verdict-table row at the
top of `07-dead-stale-code.md` still reads `VALID | trivial`. A reader
glancing at the table will think #50 is a trivial cleanup.

**Reasoning:** The body and the table disagree. The table should be
amended to say `VALID (resolved-by-design) | none` (or the entry
demoted to OBSOLETE / out-of-scope). Same applies to #148 (table says
`VALID | trivial`, body says `INVALID — closed by adversarial review`).

### #148 (current verdict: VALID — Effort: trivial / body says INVALID) → INVALID.

**Evidence:** `07-dead-stale-code.md`'s row-table for #148 doesn't exist
(only the body), but `15-globals-settings.md`'s table row for #148 says
`VALID | trivial` and the body says `**INVALID — closed by adversarial
review.**`. Internal contradiction.

**Reasoning:** The verdict line in the table needs to match the body.
The `~logger` UAM hazard analysis (logger destroyed before
function-local `instance` mutex) is the load-bearing finding; this is
actually one of the strongest examples in the catalog of why
"VALID — trivial" entries deserve adversarial review.

### #176 (current verdict: VALID — body says "31 live" cases) → VALID, body needs correction.

**Evidence:** `grep -cn "case WorkerProto::Op::"
src/libstore/daemon.cc` returns **37** case labels. The validation
paragraph says "confirmed 37 case labels (35 distinct bodies after
collapsing the merged Referrers/Derivers/Outputs arm)" — agrees with
source. But the body opens with "is a single ~700-line free-function
`switch` over **31 live `WorkerProto::Op` cases**" which is wrong (it
references `Op` count rather than `case` count, and even so excludes
several live cases).

**Reasoning:** Body and validation paragraph disagree. Body should say
"37 case labels (35 distinct bodies after collapsing
QueryReferrers/QueryValidDerivers/QueryDerivationOutputs and
OptimiseStore/VerifyStore)".

### #94 (current verdict: PARTIALLY VALID — Effort: medium) → split into two: VALID-trivial deletion + PARTIALLY-VALID-medium template factoring.

**Evidence:** The validation paragraph correctly notes that
`BuiltPath::parse` and `BuiltPath::to_string` are dead surface (declared
in `built-path.hh` with no implementation and no caller). The remaining
template factoring is medium.

**Reasoning:** The dead-surface deletion belongs in the trivial dead-
code section (where #48-60 sit), not buried inside a partially-valid
medium-effort entry. Splitting makes the trivial half land easily and
keeps the still-open template work distinct.

### #50, #148: row-table verdicts vs body verdicts

These are the two cases where the verdict table contradicts the
validation paragraph. Reconciling them is a paper edit, but it directly
affects how someone reading the catalog plans work.

## Effort-class corrections

For these candidates, the effort class as written is at odds with the
call-graph evidence. Each line: candidate # → current effort →
proposed → one-line justification.

- **#22 (Async/sync pair pattern, current: trivial → proposed: small).**
  The validation paragraph correctly identifies that
  `Callback::rethrow` semantics must be preserved for `noexcept`
  correctness. That is non-trivial reasoning across each call site;
  trivial undersells it.
- **#39 (`MemorySink`/`RestoreSink` parallel impls, current: small →
  proposed: medium).** Each `CreateRegularFileSink` subclass is a
  bytes-callback wrapper threading state through the sink. Genuine
  factoring requires a virtual-or-callback design choice that affects
  every NAR consumer.
- **#46 (`builtinBuilders` family, current: trivial → proposed: small).**
  Claim is "shared registration pattern". Verified the `getAttr` lambda
  is genuinely repeated, but each builtin's body has irreducibly per-
  builtin behaviour; the trivial label assumes the helper extraction is
  one-line per site, which is true but the helper itself needs design.
- **#65 (primop arg-validation boilerplate, current: medium → proposed:
  large or structural).** The validation paragraph says "compounds with
  #211" (the `force*` family). Both together is a structural change to
  every primop; "medium" is the helper-only scope and undersells the
  call-graph effect.
- **#83 (GC roots three layers, current: small → proposed: small —
  validation matches).** No change; flagged for crispness.
- **#86 (`OnStartup` registration pattern, current: medium → proposed:
  medium or structural).** Each registry has different invariants —
  `RegisterPrimOp` mutates a vector at static-init that is later
  consumed in `EvalState::createBaseEnv`; `RegisterStoreImplementation`
  registers into a `Implementations::registered()` Meyers singleton;
  `OnStartup` for input schemes pushes into a vector; `RegisterCommand`
  does the same for commands; `RegisterBuiltinBuilder` for builders.
  Unifying these across libutil/libstore/libfetchers/libcmd/libexpr is
  the same SIOF surgery as #137 — structural by the same logic the
  catalog applies to #137.
- **#100 (`signPathInfo`/`signRealisation`, current: trivial → proposed:
  small).** The FIXME-tracked "cache keys in memory" extension lives
  inside the validation paragraph as a cross-reference; if the
  refactor stops at `forEachConfiguredSigner` it is trivial, but the
  way the paragraph is written invites doing the cache too. Catalog
  should pick one scope and own the effort label.
- **#142 (`chrootHelperName`, current: trivial → proposed: trivial —
  matches branch).** Already landed on `vibe-coding/cleanup/nix-run`.
- **#172 (`unreachable()` audit, current: small → proposed: small per
  site / medium total).** Validation paragraph correctly elevates the
  count from 25+ to 64, but keeps "small per site". 64 sites × small =
  medium aggregate; the catalog should make that explicit so the
  consolidation isn't undersold.
- **#173 (`#pragma GCC diagnostic`, current: small → proposed: small).**
  The 10-vs-8 correction matches my recount; effort fits.
- **#211 (`force*` family, current: medium → proposed: medium —
  matches).** Note that the catalog says "actual count is 10 force*
  overloads, not 16. Pattern duplication count is ~7." I confirm this
  but the exact body shape (`try { forceValue; check type;
  return accessor } catch { addTrace }`) recurs with a uniform shape;
  medium is right.

## Identifier / file-path corrections

These are the drift sites I found beyond what the validation
paragraphs already note.

- **#137 register-site count, body says "13 TUs (16 production register
  sites)" with correction "the production count is **15**"; my recount
  with `grep -rEn "GlobalConfig::Register " src/` returns 16 lines
  total: 15 production + 1 test (`libutil-tests/nix_api_util.cc`).
  Validation paragraph is correct (15 production); the *original
  candidate body* says "16 production". Body should be edited to match.
- **#166 forward-decl counts.** Validation says "9 Source/Sink, 19
  Store, 12 EvalState"; my recount: 9 / 21 / 12. The Store count is
  off by two (the catalog appears to have missed
  `derivation-env-desugar.hh` and one of the libfetchers headers).
  Mechanism still stands.
- **#169 friend declarations on `EvalState`.** Body says "13"; the
  validation correction says "actual count is 17 friend declarations
  (with one duplicate `friend struct ExprVar`)". My recount of
  `eval.hh`: 16 friend declarations exactly (lines per `grep -nE
  "friend " eval.hh`: 908, 909, 910, 1132, 1133, 1134, 1135, 1136,
  1137, 1138, 1139, 1140, 1141, 1142, 1144, 1145). The "17" in the
  validation paragraph is wrong; correct count is 16 with one
  duplicate `ExprVar` (so 15 distinct names).
- **#217 friend declarations.** Body says "13 friend declarations of
  which 10 are `Expr*` AST nodes"; validation correction says
  "`friend struct ExprVar;` is declared **twice**", which I verified —
  but doesn't restate the total count. Per the recount above, the
  friend cluster has 10 `Expr*` lines (one duplicate), 3 `prim_*`,
  `Value`, `ListBuilder`. The "10 are `Expr*` AST nodes" is correct
  (`ExprVar` ×2, `ExprAttrs`, `ExprLet`, `ExprOpUpdate`,
  `ExprOpConcatLists`, `ExprString`, `ExprInt`, `ExprFloat`, `ExprPath`,
  `ExprSelect` = 10 lines / 9 distinct).
- **#173 pragma sites.** Validation says "10 total pragma sites (8
  `-Wswitch-enum`, 1 `-Woverloaded-virtual`, 1
  `-Wimplicit-fallthrough`); `lexer.l` has 1 not 2." My recount: 10
  total, 8 `-Wswitch-enum`, 1 `-Woverloaded-virtual`, 1
  `-Wimplicit-fallthrough`. Matches. Files: `flake.cc`, `command.hh`,
  `lexer.l`, `eval.cc` (×2), `parser.y`, `primops.cc` (×2),
  `filetransfer.cc`, `build-result.cc`. The body claim "twice in
  `lexer.l`, including a `-Wimplicit-fallthrough`" is wrong; it's a
  single pragma in `lexer.l` and that's the `-Wimplicit-fallthrough`
  one.
- **#172 unreachable() count.** Validation says "actual count is **64
  sites**". My recount with `grep -rEn "\bunreachable\(\)" src/`
  returns 65; close enough. `nix::unreachable()` is the helper,
  `std::unreachable()` is the C++23 replacement target. Body still
  says "25+"; correction paragraph is right but body still drifts.
- **#146 `_NIX_TEST_*` count.** Body says "at least 15 distinct
  names". My recount with `grep -rEnh "_NIX_(TEST|FORCE)_[A-Z0-9_]+"
  src/` returns **14** distinct names; including `tests/` brings the
  total to **20** (`_NIX_TEST_BUILD_DIR`,
  `_NIX_TEST_CLIENT_VERSION`, `_NIX_TEST_DAEMON_PACKAGE`,
  `_NIX_TEST_DAEMON_PID`, `_NIX_TEST_SHARED`, `_NIX_TEST_SOURCE_DIR`
  appear only in `tests/` shell). The catalog text only counts `src/`
  references, which is a defensible scope (consolidation only in
  C++) but worth stating.
- **#167 buffer-size literals.** Body says "12 scattered literals" —
  my `grep` of `libutil/` alone returns 4 distinct sites
  (`tarfile.cc` defaultBufferSize, `serialise.cc` 65536 buf,
  `file-system.cc` 64*1024 buf, `file-descriptor.cc` 64*1024 ×2
  sites). The 12 figure is reasonable across the whole tree but worth
  re-deriving as the body suggests.
- **#178 trust-check sites.** Validation says "actual count is **11
  trust-check sites**, not 9". My recount of `daemon.cc::performOp`:
  AddMultipleToStore (1), BuildPaths (1), BuildPathsWithResults (1),
  BuildDerivation (2 — drvType.isCA()/trusted check + drvPath
  recompute), AddPermRoot (1), FindRoots (1: `!trusted` as censor
  arg), VerifyStore (1), AddToStoreNar (2: dontCheckSigs clamp +
  ultimate clamp), AddBuildLog (1), CollectGarbage `ignoreLiveness`
  (1, unconditional). Total: 12. Validation's 11 omits the FindRoots
  censor (which is technically a clamp, not a throw, but the
  candidate frames it as a trust check elsewhere). 12 is the most
  generous count.
- **#176 `performOp` case count.** Already covered above.
- **#137 `LENGTH_PREFIXED_PROTO_HELPER_X` claim.** #57 mentions this in
  passing; the macro is in
  `length-prefixed-protocol-helper.hh` and #57's branch already
  normalises it. Worth a cross-reference back from #137 since the
  consolidation is in the same area.

## Catalog-criteria violations

The README's "What this catalog targets" excludes "cosmetic changes
(formatting, naming preferences), feature additions, performance work
without duplication or complexity to fix, or 'modernise for its own
sake' if the existing code is clear and the proposed replacement
isn't simpler". The candidates below straddle that line:

- **#56 (`BaseSetting<PathsInChroot>::trait` placement).** The body
  says "Inconsistent placement; one of them should move." That alone
  is a cosmetic preference. The validation paragraph rescues it: "a
  real ODR fix (different TUs previously saw different definitions of
  the explicit specialisation; the values coincidentally matched
  `appendable=false` from the primary template, so behaviour was
  preserved but the program was technically ill-formed)." The cosmetic
  framing in the body undersells; the entry is genuinely under "Latent
  bugs hiding inside duplication" / "ODR fix", not just style. **Move
  the categorisation hint up to the body.** (See "Mis-categorised
  candidates" below.)
- **#75 (Three NAR streaming entry points).** "Trivial (just sharing
  the TTY-check helper) / small (full `runNarDump(SourceLike)`
  helper)." The TTY-check sharing is a one-line consolidation. There
  is no duplicated logic that hides a bug; this is consolidation for
  its own sake. The catalog accepts it because it sits in "duplication
  of logic" but the duplication is one boolean call. **Marginal — keep
  but flag as cosmetic-adjacent.**
- **#82 (`AutoUserLock`/`SimpleUserLock` `acquire` skeleton).** The
  validation paragraph says "Extract `tryAcquireSlotLock(path) ->
  std::optional<AutoCloseFD>`; both call sites become a one-liner
  preceded by per-implementation prelude." Two call sites with one
  shared body each ~5 lines. Catalog accepts as "code duplication" but
  the duplication is small. **Keep but accept that the win is small.**
- **#99 (Brotli sink `finish` shape).** "Trivial." Two call sites with
  identical `flush(); writeInternal({});` bodies. Validation paragraph
  acknowledges the shared scaffold is Brotli-specific. **Marginal —
  honest "small win" rather than a debt entry.**
- **#107 (`shouldPrettyPrintAttrs/List` accessor difference).** The
  body's evidence: "byte-for-byte equivalent modulo the element
  accessor". Validation: "Effort: trivial." Two methods with a single
  accessor difference. This is the smallest possible "duplicated
  code" cluster; it borders on cosmetic refactor. **Keep but accept it
  is borderline.**
- **#121 (`operator<=>` libc++16 workarounds).** "Stdlib uplift made
  possible by C++23." Squarely in scope. No criteria violation; flagged
  to confirm. Validation says "17 sites" — confirmed by `grep -rn
  "TODO libc++ 16" src/` returning 17.
- **#125 (`boost::format`/`HintFmt` → `std::format`).** Body classifies
  as "Stdlib uplift". The validation paragraph correctly notes "every
  `%s`/`%d`/`%1%` format string needs translation to `{}`/`{:d}`. Test
  coverage in `libutil-tests/hilite.cc` and many functional tests
  assert exact error message text — must preserve." This is a
  multi-month migration with no duplication or complexity removal —
  the existing layer is clear. The README's "modernise for its own
  sake if the existing code is clear and the proposed replacement
  isn't simpler" exclusion arguably applies. **Keep on the list (one
  Boost dep removed) but the body should explicitly justify why this
  doesn't fall foul of the exclusion.**
- **#171 (`Strings = std::list<std::string>` → vector).**
  "Performance work without duplication" exclusion arguably applies.
  Validation paragraph confirms "splice is used twice in `ssh.cc`,
  plus three other splice sites" — meaning the migration has a real
  semantic cost (vector has no splice). The "wrong container in the
  hot CLI/settings path" framing is performance-flavoured; the
  refactor doesn't deduplicate anything. **On the line; keep but
  acknowledge it is performance-driven, not duplication-driven.**
- **#174 (`HAVE_*` autoconf macros).** "Audit the actual support
  matrix per probe." This is feature-flag deprecation, not
  duplication. Borderline; keep under "Dead code" if any branches are
  proven dead, otherwise drop.
- **#205 (`Counter` cache-line alignment).** "Performance work without
  duplication." The validation paragraph admits "the alignment-drop
  route needs benchmarking". This is genuinely a perf entry rather
  than a debt entry. **Marginal; keep but flag as benchmark-gated.**

## Mis-categorised candidates

Candidates whose body framing names a different debt class than what
the underlying issue actually is.

- **#56** is filed under "Dead or stale code" but is a real ODR fix.
  The validation paragraph already says so explicitly. The body should
  open with the ODR framing — "Latent bugs hiding inside duplication"
  is the right README category, not "Dead code". Same effort either
  way; the categorisation matters for prioritisation.
- **#80 (`logFD` Setting<int> Unix vs `Descriptor logFD` Windows).**
  Filed under "Other distinct candidates". Body finishes with "Latent
  bug: the Windows side cannot be set at all from user config (only
  mutated programmatically in tests)". This is squarely in "Latent
  bugs hiding inside duplication". **Recategorise to dead/stale or
  globals.**
- **#83 (GC roots three layers).** Filed under "Other distinct
  candidates". The deduplication is genuine but the underlying point
  is "three layers do meaningfully different work" — i.e., this fits
  "Multi-implementation patterns that could share a base" better than
  "Other".
- **#94 (`BuiltPath`/`SingleBuiltPath`).** PARTIALLY VALID with a
  dead-surface finding. The dead surface (`BuiltPath::parse`,
  `BuiltPath::to_string` declared without bodies) is a "Dead or
  stale code" candidate; the template factoring is a "Repeated
  boilerplate" candidate. Two different debt classes are conflated.
- **#119 (`HttpBinaryCacheStore::makeRequest` propagate lambda).**
  Filed under "libexpr extras" — wrong section; this is a libstore
  finding. The cross-references are right but the section header is
  misleading. **Section misfile.**
- **#136 (`Setting<T>{this, ...}` self-registration).** Body framed as
  "Globals, settings architecture". The deeper issue named in the
  validation paragraph is the SIOF + non-relocatable Config issue,
  which crosses into "Latent bugs hiding inside duplication" and into
  the README's "Hand-rolled abstractions where Boost or stdlib
  already has it" if a boost.PFR-style descriptor table were used.
  Multi-class; the body picks one.
- **#140 (`impureOutputHash` SIOF).** Filed under "Globals, settings
  architecture". This is squarely "Latent bugs hiding inside
  duplication" — SIOF is a latent crash class. Trivial effort, but the
  category should be "Dead/stale" or "Latent bugs", not "Globals".
- **#142 (`chrootHelperName` non-const global with hard-coded
  sentinel).** Filed under "Globals, settings architecture". Real
  category is "Dead or stale code" (effectively dead-write — never
  reassigned) and "Latent bugs" (the brace-init issue called out in
  the implementation note). It is *also* a global. The branch fix
  reflects all three.
- **#147 (`getFileTransfer()` leaked singleton).** Filed under
  "Globals". Also fits "Hand-rolled abstractions where Boost or
  stdlib already has it" because the singleton-with-restart-on-quit
  pattern recurs across libraries; a per-Store transfer pool is the
  refactor target. Cross-class.
- **#160 (`Goal::Co` coroutine machinery).** Filed under "libstore
  build deep audit". Body framing is "structurally a stackless task
  that `boost::asio::awaitable` already provides" — squarely
  "Hand-rolled abstractions where Boost or stdlib already has it".
  Validation paragraph (correctly) declines the full migration but
  keeps the partial. Effort and category framing should be aligned.
- **#216 (`Value` storage layout asserts).** Filed under "EvalState
  and Value". The lack of `static_assert(sizeof(Value)==16)` /
  `alignof(Value)==16)` is a "Latent bugs hiding inside duplication"
  finding (the duplication is between the two layout specialisations;
  one has `alignas(16)` declared, one doesn't, but neither is
  asserted at namespace scope so silent layout drift is possible).

## Branch cross-references missing or wrong

The brief listed 14 candidates that should carry `**Branch:**` refs
(#48-50, 52, 54, 56-58, 60, 127-128, 130, 142, 217). I verified each:

| Candidate | Branch | Notes |
| --- | --- | --- |
| #48 | `vibe-coding/cleanup/libflake-lockfile` | Present, matches branch (`513b47628 libflake: remove stray unused free-function forward declaration of check()`) |
| #49 | `vibe-coding/cleanup/libmain-shared` | Present; branch covers blockInt + getIntArg (`35ab0faaf`) |
| #50 | none — resolved-by-design | Present; correct |
| #52 | `vibe-coding/cleanup/libfetchers-curl-stub` | Present, matches (`74e512d11`) |
| #54 | `vibe-coding/cleanup/libstore-dead-decls` | Present, fix verified in branch diff (`useWAL = true;`) |
| #56 | `vibe-coding/cleanup/libstore-dead-decls` | Present, fix verified (trait moved to header) |
| #57 | `vibe-coding/cleanup/libstore-dead-decls` | Present, COMMA_ macros renamed/scoped per branch diff |
| #58 | `vibe-coding/cleanup/libutil-misc` | Present, fix verified (`ignoreExceptionExceptInterrupt`) |
| #60 | `vibe-coding/cleanup/libstore-dead-decls` | Present, deletion verified |
| #127 | `vibe-coding/cleanup/nix-run` | Present, fix verified (#ifdef cleanup) |
| #128 | `vibe-coding/cleanup/libmain-shared` | Present, fix verified (`allowUnit` removed) |
| #130 | `vibe-coding/cleanup/libutil-misc` | Present, fix verified (`append` shim deleted) |
| #142 | `vibe-coding/cleanup/nix-run` | Present, fix verified |
| #217 | `vibe-coding/cleanup/libexpr-friend-dup` | Present, fix verified (duplicate `friend struct ExprVar` removed) |

All 14 of the brief's listed candidates carry correct branch refs.

**Additional candidates that may also be on a branch but the catalog
doesn't say so:**

- **#148 attempt-and-revert.** The candidate carries a `**Branch:**
  none — attempt-and-revert documented in the second paragraph of
  `cleanup/libutil-misc`'s primary commit message.` That is consistent
  with the branch's `784a4f4b4 libutil/unix/signals: clarify why
  getInterruptCallbacks leak is load-bearing` commit, which expands a
  comment rather than removing the leak. **No correction needed**, but
  worth noting that the cross-reference is to a *commit message* of an
  *attempted-and-reverted* fix, which is unusual in the catalog and
  worth scanning other entries for similar shapes.
- **No other candidate appears to have landed work** on the seven
  cleanup branches that isn't already mentioned in the catalog. The
  branches and their candidate-coverage:
  - `libfetchers-curl-stub` → #52 only.
  - `libflake-lockfile` → #48 only.
  - `libmain-shared` → #49, #128.
  - `libstore-dead-decls` → #54, #56, #57, #60.
  - `libutil-misc` → #58, #130, plus `getInterruptCallbacks` comment
    expansion (cross-references #148).
  - `nix-run` → #127, #142.
  - `libexpr-friend-dup` → #217.

## Overlooked verified-shard observations

These are observations the verified shards documented as patterns or
invariants but the candidate catalog didn't translate into a separate
candidate. (Net-new candidates the *shards didn't* document go to the
overlooked-patterns agent; this section is about the gap between
verified-shard-as-source-of-truth and the catalog.)

- **`SourceAccessor::operator==`/`<=>` derived from a process-global
  atomic counter (`number`).** `INVENTORY.md` (Key Invariants) and
  `verified/01-libutil-io.md` document this. The catalog has #40
  (wrapping source accessors clearing `displayPrefix`) and #117
  (`narMaxDepth`) but no candidate addresses the `number`-based
  comparison invariant — which is unusual enough to deserve a candidate
  if any caller assumes pointer-equality. Worth a line.
- **`Bindings::iterator` k-way merge across layers (capped at
  `maxLayers = 8`).** `INVENTORY.md` Key Invariants; #206 covers
  `maxLayers` itself but doesn't say the k-way merge ordering is the
  invariant. Already covered, just worth noting #206 should cite the
  invariant.
- **`Counter` enabled gating via `NIX_SHOW_STATS`.** `INVENTORY.md`
  Key Invariants. #205 covers `Counter` alignment but the body says
  "the `enabled` gate is a runtime branch, not a compile-time switch"
  while the verified shard says "every increment/decrement returns 0
  without touching the atomic" — same fact, but #205's framing
  underplays that the existing design *already* short-circuits.
- **`SymbolTable` append-only and concurrent.** `INVENTORY.md` Key
  Invariants. #207 covers the chunked-vector layout but doesn't cite
  the append-only invariant.
- **`buildResultStatusTable` wire-tag table** noted in
  `INVENTORY.md` cross-shard topic index (Wire protocols) but not
  individually a candidate. This is a spot where wire-format
  specialisation lives (one of the few `CommonProto` Serialise sites)
  — relevant to #1 / #3 framing but not separately catalogued.
- **NAR magic constant `narVersionMagic1 = "nix-archive-1"` and
  export magic `NIXE`.** `INVENTORY.md` cross-shard topic index. No
  candidate addresses these as scattered constants, while #168
  (cache-schema versions) does the same job for SQLite version
  numbers. Worth aligning.
- **Hash algorithm enumeration.** `INVENTORY.md` Key Invariants
  documents `BLAKE3/MD5/SHA1/SHA256/SHA512` and the
  `regularHashSize` switch. The catalog touches this only obliquely
  via #8 (the `<algo>:<base>` shape) and #97 (`parseHashFormat` /
  `parseHashAlgo`). The hash enum's per-format dispatch (e.g. SRI
  prefix, `parseAny` fallback by length) is itself a recurring pattern
  that the catalog catalogues by symptom (#8) rather than by mechanism.
- **`StorePath::dummy` and `Hash::dummy` constants.** `INVENTORY.md`
  Key Invariants. The catalog has #149 / #168 / #142 around magic
  globals but no entry on the "dummy/sentinel" pattern that recurs
  across `StorePath` / `Hash` / `MissingName`. Marginal.

## Summary

- **Reclassifications proposed:** 4. #50 and #148 verdict-table rows
  contradict their bodies; #176 body count contradicts validation;
  #94 deserves splitting into two distinct candidates.
- **Effort-class corrections proposed:** ~10. Most are bumps from
  trivial → small or small → medium driven by call-graph cascade or
  cross-class compounding.
- **Catalog-criteria violations or borderline:** ~9 (#56's body
  framing, #75, #82, #99, #107, #125, #171, #174, #205). Most are
  marginal; #125 and #171 are the strongest "modernise / perf for its
  own sake" candidates and deserve explicit justification in the body.
- **Mis-categorised candidates:** 11 (#56, #80, #83, #94, #119, #136,
  #140, #142, #147, #160, #216). Most fit "Latent bugs hiding inside
  duplication" or "Hand-rolled abstractions where Boost/stdlib has
  it" but are filed elsewhere.
- **Identifier/path drift:** 7 sites (#137 body register-site count,
  #166 Store fwd-decl count, #169 friend count overstated, #173
  lexer.l count, #176 case count, #211 force* family count drift in
  body but corrected in validation, #172 unreachable count drift in
  body but corrected in validation).
- **Branch cross-references:** all 14 of the brief's listed
  candidates carry correct branch refs; no additional candidate has
  unflagged branch work.
- **Verified-shard observations the catalog didn't translate:** ~7
  (`SourceAccessor::number` invariant, `Bindings` k-way merge,
  `Counter` enabled gate, `SymbolTable` append-only, NAR/export magic
  constants, hash-enum dispatch, `dummy`/sentinel pattern). All
  marginal; flagged for completeness.

## Handoff to overlooked-patterns agent

Out-of-scope items I noticed while reviewing the existing catalog —
these are net-new patterns not currently catalogued, routed to the
parallel agent for net-new candidate selection:

- **`MakeError` + `BaseError`-derived chain in `daemon.cc::performOp`
  catches.** Each `case` in `performOp` has a different exception-
  rewriting pattern; some convert to `RemoteStore`-side errors, some
  rethrow, some swallow. Not currently a candidate; sits adjacent to
  #176/#177/#178.
- **`Sync<T>::Lock::wait(condvar)` wired to `std::condition_variable`
  but `SharedSync<T>` cannot legitimately call `wait`.** #124's
  validation paragraph names this but doesn't lift it as its own
  candidate. The mechanism mismatch (CV doesn't bind to
  `std::shared_lock`) is a real footgun.
- **Test-only `static GlobalConfig::Register rs(&mySettings);` in
  `libutil-tests/nix_api_util.cc`.** Is the test relying on the
  global registry, or simulating it? Not catalogued.
- **`Store::optimiseStore` and `Store::registerValidPath` virtual
  default-throws (`unsupported(...)`).** #23 covers `unsupported(...)`
  generically; the per-store overrides for `RestrictedStore` and
  `LegacySSHStore` are the largest concentration and might deserve a
  per-store breakdown rather than one general entry.
- **`MaintainCount<uint64_t>` member-vs-stack-vs-unique_ptr split.**
  #129 and #47 touch this but the unique_ptr-as-member shape
  (`mcExpectedBuilds = std::make_unique<MaintainCount<uint64_t>>(...)`
  vs `mcExpectedBuilds.reset()`) is a third pattern; the catalog
  documents the first two.
- **`isReservedKeyword`/`isVarName`/`printIdentifier` triple.** #109
  covers `isVarName`/`printIdentifier` but `isReservedKeyword` is a
  separate predicate that #109 mentions in passing. Whether
  `printIdentifier` should also reject reserved keywords (currently
  doesn't — `state.symbols["with"]` would render unquoted) is a real
  question the catalog doesn't ask.
- **`Activity`/`PushActivity` usage and `JSONLogger` activity-id
  sequencing.** #143 covers logging globals; the per-activity
  protocol isn't separately catalogued and may have its own
  duplication / globals (per `INVENTORY.md`'s "Error and logging"
  cross-shard topic index).

(End of file.)
