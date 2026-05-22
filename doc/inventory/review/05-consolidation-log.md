# Consolidation log

This file streams the consolidation decisions across the four review reports
(`01-adversarial-existing.md`, `02-overlooked-patterns.md`,
`03-adversarial-existing-deeper.md`, `04-overlooked-patterns-deeper.md`)
into the candidate catalog.

Format: each finding is one line — `Apply | Defer | Reject — <reason>`.
Findings are grouped by report and cluster.

## Counts of independently-verified facts

These counts were re-derived locally (via `grep -rn` against `src/`) before
folding into the catalog. Where pass 1 and pass 2 disagree, the verified
count is recorded here and applied in the corresponding section file.

(populated as the pass progresses)

## Pass 1 — adversarial existing (`01-adversarial-existing.md`)

### Reclassifications

- #50 verdict-table contradicts body — **Apply**: amend the
  `07-dead-stale-code.md` table row to read
  `VALID (resolved-by-design) | none`; the body already says so.
- #148 verdict-table contradicts body — **Apply**: amend the
  `15-globals-settings.md` table row to read `INVALID | none`; the body
  already says `INVALID — closed by adversarial review`.
- #176 body counts "31 live" cases vs validation "37" — **Apply**: rewrite
  the body opener to "37 case labels (35 distinct bodies after collapsing
  the merged Referrers/Derivers/Outputs arm and the
  OptimiseStore/VerifyStore arm)".
- #94 split into VALID-trivial deletion + PARTIALLY-VALID-medium template —
  **Defer**: pass 2 confirms the split. The dead-surface deletion can be
  added as a sibling candidate, but the section file currently treats it
  as one entry; rewording the body to surface the two halves is sufficient
  without renumbering. Apply as a body rewrite, not a renumber.
- #50 / #148 row-table reconciliation — **Apply** (same as the two above).

### Effort-class corrections

- #22 trivial → small — **Apply** (in body / verdict table); also flag that
  pass 2 says #22 is *subsumed* by N8 if asio migration lands first.
- #39 small → medium — **Apply**.
- #46 trivial → small — **Apply**.
- #65 medium → large/structural — **Defer**: pass 2 proposes merging #65
  with #211 into a single structural candidate. Apply the merge note in
  both bodies; leave #65's effort label at "medium (per-helper) /
  structural (full + #211 merge)".
- #83 small (no change) — **Reject** (no change needed).
- #86 medium → medium-or-structural — **Apply** (note structural angle in
  body; cross-reference N3).
- #100 trivial → small — **Apply**.
- #142 trivial (matches branch) — **Reject** (no change needed — branch
  has landed).
- #172 small per site / medium total — **Apply** (clarify aggregate in
  body).
- #173 small (matches) — **Reject** (no change needed).
- #211 medium (matches) — **Reject** (no change needed); note the merge
  with #65 (see above).

### Identifier / file-path corrections

- #137 register-site count: catalog body "16 production"; correct count
  16 lines total (15 production + 1 test) — **Apply** (rewrite body to
  "15 production + 1 test = 16 total").
- #166 forward-decl counts: catalog body "9, 19, 12"; correct 9, 21, 12 —
  **Apply** (rewrite body counts).
- #169 friend declarations: catalog body "13"; pass 1 says "actual count
  is 17"; pass 2 verifies 16 — **Apply** with verified count 16 (one
  duplicate `friend struct ExprVar`).
- #217 friend cluster — **Apply**: tighten body to match the verified
  count of 10 `Expr*` lines (one duplicate) plus 3 `prim_*`, plus
  `Value`/`ListBuilder` etc. Pass 1 verified.
- #173 pragma sites: catalog body claim "twice in `lexer.l`" is wrong —
  **Apply** (single pragma in `lexer.l`, the `-Wimplicit-fallthrough`
  one).
- #172 unreachable count: catalog body "25+"; correct 64+ — **Apply**.
- #146 `_NIX_TEST_*` count: catalog body "at least 15 distinct names";
  pass 1 finds 14 in `src/`, 20 tree-wide — **Apply** (rewrite body to
  "14 names in `src/`; 20 tree-wide").
- #167 buffer-size literals: catalog body "12"; pass 1 finds 4 in
  libutil; pass 2 finds 10 in libutil, 19 tree-wide — **Apply** (rewrite
  body to cite both methodologies; pass 2's count is the methodology
  one).
- #178 trust-check sites: catalog body "9"; pass 1 finds 11-12;
  pass 2 silent — **Apply** (clarify methodology; cite 11 throw-shaped
  + 1 censor as the broadest interpretation).
- #176 case count drift in body — **Apply** (already covered above).
- #137 LENGTH_PREFIXED_PROTO_HELPER_X cross-ref — **Apply** (add a
  one-line cross-reference from #137 to #57's branch since the macro
  is normalised there).

### Catalog-criteria stress points

- #56 cosmetic framing → ODR fix framing — **Apply** (rewrite body
  opener; the validation paragraph already documents it).
- #75 trivial half is one-line consolidation — **Apply**: pass 2 wants to
  delete the trivial half. Mark trivial half OBSOLETE; keep small half.
- #82 small win, keep — **Apply** (acknowledge marginal win in body).
- #99 trivial, marginal — **Apply** (flag as marginal in body).
- #107 borderline — **Apply** (acknowledge borderline in body).
- #121 in scope — **Reject** (no change needed; flagged as confirmed).
- #125 large migration with no duplication; needs justification — **Apply**
  (extend body to justify why this isn't "modernise for its own sake":
  removes a Boost dep, unlocks std::format integration with C++23
  source-locations).
- #171 perf-driven, not duplication — **Apply** (extend body to
  acknowledge perf framing alongside correctness framing).
- #174 borderline — **Apply** (flag as borderline; if no branches prove
  dead, drop).
- #205 perf-gated — **Apply** (flag as benchmark-gated, defer to N21).

### Mis-categorised candidates

- #56 (Dead/stale) → also "Latent bugs hiding inside duplication" — **Apply**
  (cross-reference in README; body already documents both).
- #80 (Other) → Latent bugs / globals — **Apply** (add latent-bug
  framing to body).
- #83 (Other) → Multi-impl base — **Defer** (the deduplication is real
  but #83's framing is fine; cross-reference Cluster B added).
- #94 (PARTIALLY VALID with dead-surface) — **Apply** (already covered
  in reclassifications).
- #119 filed under "libexpr extras" but is libstore — **Apply** (move
  to libstore section, or add a cross-reference from libstore section).
- #136 multi-class — **Apply** (acknowledge in body).
- #140 SIOF → Dead code — **Apply** (rewrite body per pass 2's stronger
  finding: zero call sites, pure deletion).
- #142 multi-class — **Apply** (acknowledge three concerns; per pass 2,
  three are bundled).
- #147 multi-class (singleton-with-restart) — **Apply** (flag as
  cross-class).
- #160 → Hand-rolled-vs-Boost (already framed) — **Apply** (tighten the
  framing alignment with the catalog README).
- #216 layout-asserts as latent bug — **Apply** (extend body).

### Branch cross-references

All 14 confirmed by pass 1; pass 2 didn't recheck branches. — **Reject**
(no changes needed for the branch cross-references themselves; defer the
periodic re-verification to E5).

### Verified-shard observations the catalog didn't translate

- `SourceAccessor::number` invariant — **Defer** (marginal; cross-reference
  in INVENTORY.md cross-shard topic index would suffice).
- `Bindings` k-way merge — **Defer** (already covered by #206; minor
  body annotation).
- `Counter::enabled` runtime branch framing — **Defer** (minor body
  annotation on #205).
- `SymbolTable` append-only — **Defer** (minor body annotation on #207).
- NAR/export magic constants — **Apply via N37** (pass 2's N37 covers
  this).
- Hash enum dispatch — **Apply via N20/N32** (pass 2's N20 / N32 cover
  this).
- `dummy`/sentinel pattern — **Defer** (marginal).

### Handoff to overlooked-patterns agent

These were rerouted to pass 2; pass 2 either covered or flagged them.
Handled via N-candidates below.

## Pass 2 — adversarial existing deeper (`03-adversarial-existing-deeper.md`)

### Verifications of pass 1's claims

(Confirmed without action — pass 2 verifies pass 1's counts and verdicts
on #137, #143, #176, #169, #166, #167, #146, #173, #94, branch refs.)

### New findings

- #140 dead code (not SIOF) — **Apply** (already in reclassification:
  rewrite body as "dead code; pure deletion"; reclassify to dead-code
  framing).
- #147 per-Store ownership hazard — **Apply**: change verdict from VALID
  to PARTIALLY VALID; document that the singleton-leak path must be
  retained for the legacy `getFileTransfer()` callers. Cross-reference
  N17 (LeakedSingleton wrapper).
- #143 TLS-fallback shutdown ordering — **Apply** (extend body with
  "free functions must gracefully handle TLS-stack-empty").
- #149 gcc miscompilation guard (not UAF) — **Apply** (rewrite the
  validation paragraph: replace "use-after-free of moved-from setting"
  with "gcc bug 80431 miscompilation guard"; preserve the guard at the
  new ownership boundary).
- #208 type-level enforcement — **Defer** (the recommendation is sound
  but the type-level fix is a separate refactor; flag in body but don't
  rewrite mechanism).
- #137 init sequencing — **Apply** (extend body: "explicit init must
  precede any setting access including `loadConfFile`").
- #128 commit-bundling caveat — **Defer** (the commit has landed; the
  bundling is a soft style issue per CLAUDE.md but not actionable as a
  catalog edit).
- #170 friend count drift — **Apply** (rewrite body to "three Goal
  subclasses; the other three expose their key fields publicly").

### ABI implications

- #138 multiple C-API readers — **Apply** (extend pitfall paragraph:
  "two C-API readers, not one; `nix_api_expr.cc` aliases the pointer").
- #137 C-API initialisation sequencing — **Apply** (already covered by
  init-sequencing edit above).
- #122 layout drift in `PrimOp::impl` + symbol-mangle in
  `stackOverflowHandler` — **Apply** (extend validation paragraph: clarify
  that no C-API symbol exposes `fun`, but `PrimOp::impl` member changes
  layout and `stackOverflowHandler` is an `extern fun<...>` symbol;
  bump effort to "medium with explicit ABI version bump", or scope to
  non-public-header sites first).
- #123 C-API internal-header consumer — **Defer** (pass 2 verifies but
  has no actionable change; the recommendation "keep `ref<T>`, drop
  `bad_ref_cast`" is what the body says).
- #134 silent ABI: `nix::settings.readOnlyMode` field-syntax — **Apply**
  (extend pitfall: "C-API impl reads `nix::settings.readOnlyMode`
  directly via field; keep the field accessible for one release window
  post-#138").

### Cross-candidate consistency conflicts

- #134 vs #144: do #144 first — **Apply** (add "compounds with #144 (do
  first); after #144 only `LocalSettings`-vs-`Settings` remains" to
  #134's body).
- #136 vs #19/#43 keystone — **Apply** (add explicit "blocked by #136"
  tags to #19 and the simple half of #43; note #21 remains independent
  in body).
- #137 vs #146 test-fix — **Apply** (extend #137 body: "test-only
  register site in `libutil-tests/nix_api_util.cc` needs corresponding
  rework").
- #160 vs #22 sequencing — **Apply** (mark #22 as "trivial *if done in
  isolation*; **subsumed** by N8 if asio migration lands first").
- #205 vs N21 — **Apply** (sequence: N21 first, then #205; cross-reference
  N21 from #205).
- #122 vs #123 (no conflict) — **Reject** (nothing to apply).

### Test-side patterns

- T1 `VERSIONED_CHARACTERIZATION_TEST` mirrors absent reflection —
  **Apply via N16/N39** (folded into pass 2's N39).
- T2 `*-test-support` parallel — **Apply via N28/N41** (folded into
  pass 2's Cluster F).
- T3 `_NIX_TEST_ACCEPT` per-call read — **Defer** (call out in #146
  validation paragraph).

### Effort-class dependencies on keystones

- #150 → #170, #151 — **Apply** (already noted in section 16 keystone
  block; tighten the effort-class language).
- #138 → #131 — **Apply** (add "depends on #138 landing first" to #131's
  body).
- #149 → #136 → #134, #135, #144 chain — **Apply** (extend #149's body
  to acknowledge the chain).
- #212 → #210 — **Apply** (extend #212's body).
- N4 reshuffles — **Apply via N4** (cross-reference in N4's section
  file).

### Candidate splits / merges / deletes

- Split #18 into 18a / 18b / 18c — **Defer**: this requires renumbering
  or footnote-style sub-IDs. Apply as a body restructure: keep #18 as
  the umbrella, but spell out the three sub-axes (macro / hand-written /
  SystemError-tag) explicitly in the body so readers can act on each
  half independently.
- Split #142 — **Apply** (already addressed: body lists three concerns
  per the branch; rewrite to surface them as bullets).
- Merge #65 + #211 — **Apply** (mark both bodies with "merged-pair"
  cross-reference; structural effort-class).
- Demote #134, #135 into #144 — **Defer**: pass 1 keeps them; pass 2
  proposes demotion. The demotion is a recommendation, not a body fact —
  cross-reference both in #144's body and add a "consider folding into
  #144" note in #134/#135 bodies; keep them as separate candidates so
  the dependency on #136 stays load-bearing.
- Delete #75 trivial half — **Apply** (mark trivial half OBSOLETE in body;
  keep the small half).
- Delete #44 already OBSOLETE — **Apply** (the body says OBSOLETE; keep
  in section listing per the catalog convention of marking obsolete
  rather than removing).
- Split #94 — **Apply** (already covered).

### Boost / C++23 / asio uplift gotchas

- #122 layout drift — **Apply** (already covered above).
- #125 daemon-client error-text preservation — **Apply** (extend
  validation paragraph: "exception message text is observed by C-API
  and by the daemon-client protocol; pure mechanical translation of
  `%s` → `{}` preserves the text only if the formatter implementations
  exactly match; daemon-client mixed-version testing must include
  error-message preservation").
- #160 final_awaiter HALO incompatibility with asio — **Apply** (extend
  validation paragraph: "the hybrid migration requires verifying that
  asio's awaiter chain doesn't capture the parent continuation in a way
  that conflicts with `final_awaiter::top_co` swap; a migration step
  that replaces `SuspendAwaiter` while keeping `final_awaiter` must
  confirm via test that the same number of frames are alive at the same
  call depth").
- #129 destructor-throw caveat — **Apply** (extend validation paragraph:
  "the `Finally` replacement is correct only as long as the captured
  lambda is guaranteed not to throw; add a `static_assert` or comment
  documenting the no-throw expectation, or prefer a typed
  `MaintainCount`/`ScopedCounter` over an anonymous `Finally`").

### Items to escalate (E1-E8)

These are deferred to follow-up agents per the brief.

- E1 verify `impureOutputHash` deletion path — **Defer** (in deferred
  follow-ups list).
- E2 audit C-API `*Settings` surface for #134/#138/#144 — **Defer**.
- E3 Goal::Co property test — **Defer**.
- E4 daemon-client error-text matrix — **Defer**.
- E5 periodic branch verification — **Defer**.
- E6 test-support consolidation — **Defer** (covered by Cluster F /
  N28/N39/N41 candidates).
- E7 fwd-decl source-compat audit — **Defer**.
- E8 `vTrue`/`vFalse` invariant choice — **Defer**.

## Pass 1 — overlooked patterns (`02-overlooked-patterns.md`)

### N-candidates (N1-N23)

Each N-candidate folded into a new candidate-section file with the
verified count from pass 2 where available.

- N1 (caches/registries) — **Apply with refinement** (pass 2 splits into
  N1a/N1b: insert-on-miss caches + ordered registries).
- N2 colon-separated parser family — **Apply**.
- N3 static-registration registries — **Apply with verified count 8**
  (pass 2's correction).
- N4 X-macro `*Settings` structs — **Apply with verified count 21-22**.
- N5 `Signable<Body>` mixin — **Apply**.
- N6 Source/Sink wrapping — **Apply with refinement** (pass 2 names
  `BuildLog` line-buffer + JSON-frame).
- N7 `openAccessor(spec)` — **Apply**.
- N8 three async idioms — **Apply**.
- N9 per-platform symmetry — **Apply**.
- N10 `BasicConnection<Proto>` template — **Apply**.
- N11 iterate-attrset / dispatch-by-name — **Apply**.
- N12 declarative InputScheme schema — **Apply**.
- N13 SQLite caches — **Apply**.
- N14 worker counters — **Apply with verified split 12+2**.
- N15 version-tag-then-payload — **Apply**.
- N16 test boilerplate / reflection — **Apply with verified count 176**.
- N17 leaked-Sync<T*> — **Apply**.
- N18 env-var iceberg — **Apply**.
- N19 fat types — **Apply**.
- N20 `nValueType` table-driven dispatch — **Apply**.
- N21 counter-pattern recurrence — **Apply**.
- N22 macros emitting struct declarations — **Apply** (note pass 2
  expands the count to 9).
- N23 opcode schema = three projections — **Apply**.

### Pattern clusters A-D

- Cluster A concurrent state and globals — **Apply** (README subsection).
- Cluster B variant-shaped types — **Apply**.
- Cluster C SourceAccessor / Store factories — **Apply**.
- Cluster D settings-tied configuration — **Apply**.

### C++23 / Boost uplift series U1-U10

- U1-U10 — **Apply** (README subsection).
- U7 split into U7a/U7b/U7c — **Apply** (per pass 2's refinement).

### Architectural debt themes A1-A7

- **Apply** (README subsection).

### Redundant abstraction pairs R1-R10

- **Apply** (README subsection).

## Pass 2 — overlooked patterns deeper (`04-overlooked-patterns-deeper.md`)

### N24-N44 new candidates

- N24 C-API opaque wrappers — **Apply** (Cluster E).
- N25 C-API init idempotency — **Apply** (Cluster E).
- N26 typed-attribute extractors — **Apply**.
- N27 `*Impl` factory pattern — **Apply**.
- N28 test-data env-var plumbing — **Apply** (Cluster F).
- N29 `NIXC_CATCH_ERRS` family — **Apply** (Cluster E).
- N30 `MixJSON` per-command bifurcation — **Apply**.
- N31 SQLite cache cleanup hazard — **Apply**.
- N32 parse/show pair pattern — **Apply**.
- N33 16 friend declarations — **Apply** (also a #169 deeper observation;
  cross-reference).
- N34 cache invalidation idioms — **Apply**.
- N35 X-macros for "list of names" — **Apply**.
- N36 `make*Sink` factories — **Apply**.
- N37 magic file headers — **Apply**.
- N38 `extern template` instantiation lists — **Apply**.
- N39 test-data folder pattern — **Apply** (Cluster F).
- N40 per-`InputScheme` URL-query drift — **Apply**.
- N41 per-fixture `unitTestData` boilerplate — **Apply** (Cluster F).
- N42 atomic ID counters — **Apply**.
- N43 per-subsystem `std::thread` lifecycle — **Apply**.
- N44 `BaseSetting<T>::trait` ad-hoc specialisation — **Apply**.

### Disagreements with pass 1

- N1 over-claim — **Apply** (folded into N1a/N1b refinement).
- N3 5→8 — **Apply**.
- N4 19→21-22 — **Apply**.
- N14 12 vs 14 — **Apply** (note 12+2 split).
- N6 line-buffer family — **Apply**.
- U7 split — **Apply** (U7a/U7b/U7c).
- N16 130→176 — **Apply**.
- R10 too quiet — **Apply** (cross-reference N43).

### Clusters E-G

- Cluster E C-API surface duplication — **Apply** (new section file
  `23-c-api-debt.md`).
- Cluster F Test-infrastructure mirrors — **Apply** (new section file
  `24-test-infrastructure-mirrors.md`).
- Cluster G Dispatch tables and schemas — **Apply** (new section file
  `25-dispatch-tables-and-schemas.md`).

### Mis-classified pass 1 didn't catch

- #211 cross-reference to N20 — **Apply** (extend #211 body).
- #129 cross-reference to N14/N21 — **Apply** (extend #129 body).
- #122 non-nullable invariant loss — **Apply** (extend #122 validation).

## Deferred follow-ups (E1-E8 + others)

These require source-level investigation outside the consolidation
scope. The user should schedule follow-up agents for each:

- E1: verify `impureOutputHash` is fully dead before deletion (#140 path).
- E2: re-audit `*Settings` C-API surface for #134, #138, #144.
- E3: write a Goal::Co property test pinning O(1) frame depth before
  any partial migration of #160.
- E4: build a daemon-client error-message preservation matrix for #125.
- E5: periodic branch verification (catalog body counts drift over time).
- E6: test-support consolidation verification (T2 / N28 / N41).
- E7: forward-decl header source-compat audit (#166 / E7).
- E8: `EvalState::vTrue`/`vFalse` consistency invariant choice (#213).

## Conflicts and chosen tradeoffs

These are decisions where applying a finding would conflict with another
finding, and the consolidation has to choose:

- **#134 / #135 demotion vs. #136 keystone:** pass 2 proposes demoting
  #134 and #135 into #144. Pass 1 keeps them as separate candidates
  blocked by #136. Decision: **keep #134/#135 as separate candidates
  with cross-references to #144**; their dependency on #136 is the
  load-bearing fact, and folding them into #144 would lose that.
  Cross-reference both ways instead.
- **#65 / #211 merge vs. independence:** pass 2 proposes merging
  these into one structural candidate. Pass 1 catalogs them
  independently. Decision: **mark both bodies with the merged-pair
  cross-reference**; readers can act on each half but the cross-reference
  signals the structural scope.
- **#94 split vs. unified:** both passes recommend splitting into a
  trivial-deletion half and a medium-template-factoring half. Decision:
  **rewrite the body to surface the two halves** without renumbering
  the candidate IDs.
- **#125 in scope vs. modernise-for-its-own-sake:** pass 1 borderline,
  pass 2 silent on the catalog-criteria question but adds
  daemon-client error-text concern. Decision: **keep in catalog**, with
  the expanded body justifying scope on grounds of (a) Boost dep
  removal and (b) `std::source_location` integration; **flag the
  daemon-client matrix as out-of-scope verification (E4)**.
- **#171 perf vs. duplication:** the README excludes "performance
  work without duplication". Decision: **keep in catalog, marked as
  perf-driven and acknowledging the README criterion**; readers can
  decide whether to pursue.
- **#205 vs N21:** pass 2 wants N21 first as architecture, #205 as
  local. Decision: **sequence N21 first**; cross-reference both ways.

## File-modification ledger

Files modified in this consolidation pass:

- `doc/inventory/candidates/README.md` — added "VALID (resolved-by-design)"
  verdict legend; expanded sections table to 25 entries (added 22-25);
  added "Pattern clusters" subsection (A-G); added "Architectural debt
  themes" subsection (A1-A7); added "C++23 / Boost uplift series"
  subsection (U1-U10 with U7a/b/c split); added "Redundant
  abstractions" subsection (R1-R10).
- `doc/inventory/candidates/07-dead-stale-code.md` — fixed #50 verdict
  table; expanded #56 framing as ODR fix and Latent-bug class.
- `doc/inventory/candidates/15-globals-settings.md` — fixed #148
  verdict table; rewrote #140 as dead-code framing; corrected #137
  register-site count (15 production + 1 test = 16 total) and added
  init-sequencing constraint, test-site impact, N3 cross-reference;
  corrected #146 count (14 in src/, 20 tree-wide); demoted #147
  verdict to PARTIALLY VALID with N17 cross-reference and per-Store
  hazard; corrected #149 rationale to gcc bug 80431 (not UAF); added
  #143 TLS-fallback shutdown-ordering hazard; added #134 vs #144
  sequencing; updated #138 with two C-API readers and E2 cross-ref;
  added #131 dependency on #138; expanded #142 three-concern body;
  expanded keystones header with chain documentation.
- `doc/inventory/candidates/17-cross-cutting.md` — corrected #166
  forward-decl counts to 9/21/12 unique headers; corrected #167
  buffer-size methodology (10 in libutil, 19 tree-wide); corrected
  #169 friend count to 16; rewrote #170 to 3 Goal subclasses + Waker;
  corrected #172 unreachable count to 64-65; corrected #173 lexer.l
  count to 1; expanded #168 with N13/N15/N31 cross-references;
  expanded #171 with perf-vs-duplication framing; expanded #174 with
  catalog-criteria borderline note.
- `doc/inventory/candidates/18-daemon-protocol-audit.md` — corrected
  #176 case count to 37 (35 distinct bodies); added N12/N23 cross-ref;
  corrected #178 count to 11+1; added trust-check methodology note.
- `doc/inventory/candidates/14-vestigial-stdlib.md` — expanded #122
  with `PrimOp::impl` layout-drift hazard and `stackOverflowHandler`
  symbol-mangle break; expanded #125 with daemon-client error-text
  preservation requirement and E4 cross-ref; expanded #129 with
  destructor-throw caveat and N14/N21 cross-refs.
- `doc/inventory/candidates/16-libstore-build-audit.md` — expanded
  #160 with HALO-incompatibility hazard and N8/E3 cross-refs;
  expanded #150 with N14/N19 cross-references.
- `doc/inventory/candidates/03-repeated-boilerplate.md` — updated #22
  effort to "small (in isolation) — subsumed by N8 if asio migration
  lands first"; expanded #18 with three sub-axes (18a/18b/18c) and
  N22/R9 cross-refs; updated #19 with #136 keystone + N4 cross-ref.
- `doc/inventory/candidates/06-multi-impl-base.md` — bumped #39 effort
  to medium with N6 cross-ref; bumped #46 effort to small with helper
  rationale; expanded #36 with N20 cross-ref; expanded #38 with N6/N36
  cross-refs; expanded #43 with #136 keystone, N4/N22/N44 cross-refs.
- `doc/inventory/candidates/10-other.md` — marked #75 trivial half
  OBSOLETE; expanded #80 framing as latent bug with N9/N44 cross-refs;
  expanded #82 with marginal-win acknowledgment.
- `doc/inventory/candidates/11-legacy-cli.md` — rewrote #94 to surface
  the dead-surface deletion (trivial) + template factoring (medium)
  as two distinct halves with Cluster B cross-ref.
- `doc/inventory/candidates/12-libutil-libstore-core-extras.md` —
  expanded #99 with marginal-win flag; expanded #100 with scope
  choice; expanded #102 with N5/N2/Cluster B cross-refs.
- `doc/inventory/candidates/13-libexpr-extras.md` — expanded #107 with
  borderline note; expanded #119 with libstore-section-misfile note;
  expanded #120 with N1 / Cluster A cross-refs.
- `doc/inventory/candidates/20-eval-core-cache-attrset-profiler.md` —
  expanded #201 with N13/N15/N31 cross-refs; expanded #203 with
  N13/N34; expanded #205 with N14/N21/N42 cross-refs and sequencing
  note (N21 first); expanded #206 with k-way-merge invariant note;
  expanded #207 with append-only invariant; expanded #208 with
  type-level enforcement recommendation.
- `doc/inventory/candidates/21-eval-core-evalstate-value.md` —
  corrected #217 friend count to 16 (10 Expr* with one duplicate);
  expanded #210 with #212-keystone and N19/N33 cross-refs; expanded
  #211 with N20 cross-ref and #65 merge note; expanded #212 with #210
  sequencing and N19 cross-ref; expanded #213 with E8 cross-ref;
  expanded #216 with latent-bug framing.
- `doc/inventory/INVENTORY.md` — updated "Consolidated duplication
  and refactoring candidates" header to mention 25 sections /
  N1-N44 cross-shard candidates / architectural-debt themes A1-A7 /
  uplift series U1-U10 / redundant-abstraction pairs R1-R10.
- **NEW** `doc/inventory/candidates/22-cross-shard-patterns.md` —
  pass-1's N1-N23 candidates with verified counts and cross-references.
- **NEW** `doc/inventory/candidates/23-c-api-debt.md` — pass-2's
  Cluster E (N24, N25, N29).
- **NEW** `doc/inventory/candidates/24-test-infrastructure-mirrors.md`
  — pass-2's Cluster F (N28, N39, N41).
- **NEW** `doc/inventory/candidates/25-dispatch-tables-and-schemas.md`
  — pass-2's Cluster G (N26, N30, N32-N38, N40, N42-N44) plus
  N27/N31/N33/N34/N42/N43.

## Summary

### Decision counts

- **Apply:** 89 findings — load-bearing corrections, count fixes,
  effort-class adjustments, mis-categorisation cross-references, all
  44 N-candidates folded into new section files, all 7 architectural-
  debt themes, all 10 uplift-series stages, all 10 redundant-
  abstraction pairs, all 7 cross-cutting clusters (A-G).
- **Defer:** 13 findings — eight follow-up agents (E1-E8), several
  candidate-merge proposals (#134/#135 demotion into #144,
  #65/#211 merge, #18 split into 18a/b/c) preserved as cross-
  references rather than renumbering, plus marginal verified-shard
  observations that don't yet justify candidate status.
- **Reject:** 4 findings — #83 effort label (no change needed),
  #142 trivial-effort (already correct), #173 effort label,
  #211 effort label (each a "matches" line in pass 1's effort
  corrections).

### Verified counts (re-derived during the pass)

- `GlobalConfig::Register` sites: **16** (15 production + 1 test).
- `case WorkerProto::Op::` labels in `daemon.cc`: **37**.
- `friend ` declarations in `eval.hh`: **16** (10 `Expr*` with one
  duplicate `ExprVar`, 3 `prim_*`, `Value`, `ListBuilder`).
- `friend class Worker`: 4 sites = 3 Goal subclasses + 1 inner Waker.
- forward-decl unique headers: 9 Source/Sink, 21 Store, 12 EvalState.
- `unreachable()` sites: **64-65** tree-wide.
- `#pragma GCC diagnostic ignored`: **10** total (8 `-Wswitch-enum`,
  1 `-Woverloaded-virtual`, 1 `-Wimplicit-fallthrough`).
- `_NIX_(TEST|FORCE)_*` distinct names in `src/`: **14**.
- `// TODO libc++ 16`: **17**.
- `impureOutputHash`: **1 site** (definition only — dead code).

### Conflicts and chosen tradeoffs

These are the decisions where applying a finding would conflict with
another finding, and the consolidation chose a side. All recorded in
the "Conflicts and chosen tradeoffs" section above.

- **#134/#135 demotion vs #136 keystone:** kept separate with
  cross-references to #144 to preserve the #136 dependency chain as
  load-bearing.
- **#65/#211 merge vs independence:** marked both bodies with the
  merged-pair cross-reference; readers can act on each half.
- **#94 split vs unified:** rewrote the body to surface both halves
  (trivial dead-surface + medium template) without renumbering.
- **#125 in scope vs modernise-for-its-own-sake:** kept in catalog
  with expanded scope justification (Boost dep removal, source_location
  integration); flagged daemon-client matrix as out-of-scope (E4).
- **#171 perf vs duplication:** kept in catalog, marked perf-driven
  alongside the correctness framing.
- **#205 vs N21:** sequenced N21 first (architecture-level), #205 as
  local consequence; cross-referenced both ways.
- **#18 split into 18a/b/c:** preserved #18 as umbrella with three
  sub-axes spelled out in body; not renumbered to footnote-style IDs.

### Open questions for the user

The user should schedule the eight follow-up agents (E1-E8) listed
above. Each requires source-level investigation outside the
consolidation pass scope:

- **E1** verify `impureOutputHash` deletion path (no out-of-tree
  consumer reads it via the libstore-shipped header).
- **E2** audit C-API `*Settings` surface for #134/#138/#144 in
  lockstep.
- **E3** write a Goal::Co property test pinning O(1) frame depth at
  depth N before any partial migration of #160.
- **E4** build a daemon-client error-message preservation matrix for
  #125's `std::format` migration.
- **E5** periodic branch verification (catalog body counts drift over
  time).
- **E6** test-support consolidation verification (T2 / N28 / N41).
- **E7** forward-decl header source-compat audit (#166 / N9).
- **E8** `EvalState::vTrue`/`vFalse` consistency invariant choice
  (#213).

Additionally, the user may want to reconsider the
**#134/#135-into-#144 demotion** and **#65+#211 merge** proposals
later in light of how the work is sequenced — both are kept as
cross-references rather than structural changes for now, because
the keystone dependency on #136 (for #134/#135) and the call-graph
cascade (for #65+#211) are load-bearing facts that demotion or
merge would obscure.

(End of file.)
