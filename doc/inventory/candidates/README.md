# Refactoring candidates

Split per-section from the previous monolithic `CANDIDATES.md`. Each section
file lists the relevant candidates with the validation verdict, effort class,
and any corrections discovered during the verification pass. The verification
pass read the cited source end-to-end; where the original candidate text
drifted from current source, the entry has a `**Validation:**` paragraph
with the corrected detail.

## What this catalog targets

The catalog tracks **duplication, complexity, and generalisation
opportunities** — places where C++23, current Boost, or simple
restructuring lets us replace bespoke or hand-rolled code with cleaner
abstractions. Concretely, the kinds of debt collected here:

- **Macro-driven boilerplate that should be a template.** CRTP,
  deducing-this, concept-constrained templates, or X-macros each replace
  a recurring `#define`-based pattern. Examples: `MakeError`,
  `MAKE_WRAPPER_CONSTRUCTOR`, the three protocol macro families,
  `MakeBinOp`, the `Setting<T>{this, ...}` initialiser pattern.
- **Two parallel implementations that are really one knob.** A pair
  differing in a single mutex, flag, or type parameter is one template
  parameter, not two classes. Examples: `Sync<T>` vs `SharedSync<T>`,
  `Pipe::create` Unix vs Windows non-blocking, `Counter`
  enabled/disabled, the `WorkerProto` / `ServeProto` / `CommonProto`
  trio.
- **Diamond inheritance dressed up as polymorphism.** When the type
  hierarchy is really a Cartesian product (platform × strategy), prefer
  a strategy/policy split with composition. Example: the
  `DerivationBuilder` diamond (chroot strategy × Linux/FreeBSD/Darwin).
- **Stdlib uplift made possible by C++23 (and modernisation more
  broadly).** Defaulted `operator<=>` for the libc++16 workaround sites,
  `std::move_only_function` for `nix::fun`, `std::ranges::append_range`
  for the `append` shim, `std::format` for `boost::format` + `HintFmt`,
  `std::unreachable()` for genuinely-unreachable switch fall-through,
  `std::jthread` for hand-rolled thread lifecycles. Modernisation is
  in scope even without an underlying duplication: replacing a
  Boost-only mechanism with a stdlib mechanism shrinks the dependency
  surface, replacing a hand-rolled idiom with a stdlib idiom shrinks
  the volume of bespoke code we maintain, and migrating to a feature
  the toolchain already supports is a genuine simplification.
- **Hand-rolled abstractions where Boost or stdlib already has it.**
  Bespoke `Goal::Co` coroutine machinery vs `boost::asio::awaitable`,
  ad-hoc concurrent-map insert-on-miss vs
  `boost::concurrent_flat_map::try_emplace_and_cvisit`, manual
  `nlohmann::json_sax` parser vs `nlohmann::json::parse + walker`.
- **Globals-as-architecture.** Process-wide singletons or `extern`
  globals that are really per-subsystem state, where dependency
  injection or per-`Store` / per-`EvalState` ownership belongs.
  Examples: the 19 `Config`-derived globals, `drvHashes` shared across
  stores, the four logging globals.
- **Latent bugs hiding inside duplication.** Each cluster of "the same
  thing five times" tends to have one site that drifted. Surfacing the
  duplication and consolidating it forces these into the open. Already
  surfaced examples: `SQLiteSettings::useWAL` UB, `getCustomRegistry`
  first-call-wins, `CollectGarbage`'s unconditional `ignoreLiveness`
  throw, `AttrDb::getAttr` bypassing the `doSQLite` failed-flag
  protocol.
- **Dead code.** Declarations without definitions, parameters never
  read, `#if 0` blocks, leaked macros, vestigial `extern` globals.
  Smallest tier; usually first to land because it's pure deletion.
- **Performance.** Wrong-container choices on hot paths (e.g.
  `std::list<std::string>` vs `std::vector<std::string>` for the
  canonical `Strings` alias on the CLI/settings path), unnecessary
  allocations in inner loops, cache-line-aligned counters whose
  alignment cost is paid even when disabled, missed concurrency where
  independent operations run sequentially. Performance work belongs
  in scope even when no duplication backs the change — a measurable
  improvement to the eval / build / fetch hot paths is an end in
  itself.

What the catalog does **not** target: cosmetic changes (formatting,
naming preferences) or feature additions. Performance work and
modernisation are in scope on their own merits; an entry should still
explain *why* the change is worth doing (faster hot path, smaller
dependency surface, simpler abstraction), but the entry is not
required to point at duplication or hidden complexity.

The eight kinds of debt above also cover the cross-shard candidates
(N1-N44, sections 22-25). Pattern-discovery passes that surfaced
"there are five hand-rolled instances of pattern X" reduce to one of
the eight: typically "Macro-driven boilerplate that should be a
template" (N3 registries, N4 settings structs, N22 macros, N35
X-macros, N38 extern-template lists), "Hand-rolled abstractions where
Boost or stdlib already has it" (N1 `Memo`, N6 `WrappingSink`, N7
`openAccessor`, N8 asio, N10 `BasicConnection<Proto>`, N13
`SqliteCache<Schema>`, N17 `LeakedSingleton`, N20
`valueTypeTraits[]`, N21 metrics, N26 typed extractors, N32 parse/show,
N36 `makeSink`, N42 `IdGenerator`, N43 `std::jthread`), "Globals-as-
architecture" (N4, N17, N18, N19), "Latent bugs hiding inside
duplication" (N3 duplicate-policy drift, N31 orphaned caches, N40
URL-parser allow-list drift), or "Stdlib uplift made possible by
C++23" (the U1-U10 series). The N-numbered candidates are *not* a new
debt class; they are cross-shard instances of the existing classes
that no per-shard cataloger had in view simultaneously.

## Evidentiary standard

Findings in the catalog must rest on semantic source evidence. Specific
rules, derived from failure modes seen in prior review passes:

1. **Walk the call graph.** A claim about a function's behaviour must
   trace at least one level of caller — including across virtual
   dispatch and static-destruction order. Do not reason about a
   function in isolation. The `windowSize()` un-leak hazard
   (#148, INVALID) and E8's `eqValues` short-circuit reasoning are
   the canonical examples of this failure mode.
2. **Trace what ships, not what's in src/.** ABI claims must check
   `meson.build` install rules, library symbol export, and what's
   visible in the install set. Source-tree contents are not a proxy
   for ABI surface. E2's "no settings struct type is exposed across
   the C ABI" was correct about source-text uses but wrong about
   shipping headers (V1 found 5 of 6 C-API libraries install their
   `*_internal.h(h)` files).
3. **Counts require closed enumeration.** When a body cites a count,
   the next reader must be able to reproduce it. Record the exact
   grep / filter / canonicalisation. "Approximately N" is acceptable
   only when an exact count is documented as out of scope and the
   approximation is bounded. Counts that drift between passes (#169
   went 13 -> 17 -> 16) should be re-derived against current source
   on every catalog update.
4. **Distinguish predicted from verified.** A claim about how a
   refactor will turn out is a prediction; a claim about how the
   current code behaves is verifiable. Mark predictions explicitly
   ("predicted, not verified" or similar). E6's "fits cleanly"
   classifications were predictions; V5 re-verified each against the
   fixture body.
5. **Re-derive inherited claims.** If you rely on a prior pass's
   finding, verify it against current source. Citation alone is not
   evidence. The "#136 keystone for #134/#135/#137/#144/#149" chain
   propagated through three passes before B1 found that #135 does
   not actually depend on #136.
6. **"Same shape" is not "same problem".** Structural similarity is
   a hypothesis to test, not a conclusion. Walk every alleged shared
   site individually. B2 found that `forceAttrs`/`forceList` use
   `withTrace` rather than `addTrace + try/catch`, despite the prior
   reviewer lumping them with the rest of the `force*` family.
7. **Numbers require measurement.** Performance claims, frequency
   claims, and quantitative comparisons must be measured or labelled
   "structural reasoning, unmeasured". Do not invent figures. E8's
   "1-2 ns" was unsupported and downgraded by V4.
8. **Comments are hypotheses.** When source comments make a claim
   about contract or rationale, verify it against source behaviour.
   Code that contradicts a comment is the truth. The `Value::vTrue`
   "this is _not_ a singleton" comment is contradicted by every
   actual consumer; the comment lost.

When verifying a claim, the report should state which of these rules
was exercised. When proposing a candidate, the body should be
self-checking against the rules: a candidate that proposes "merge
these N sites" requires that the reviewer walked the N sites
individually (rule 6); a candidate that cites a count requires the
closed enumeration (rule 3).

## Verdict legend

- **VALID** — claim corroborated by source; refactor is genuinely available.
- **PARTIALLY VALID** — core claim holds but a detail (file path, count,
  proposed mechanism) needs adjustment. The validation paragraph spells out
  what.
- **INVALID** — claim does not hold under closer reading; do not pursue.
- **OBSOLETE** — duplication is real but too small to justify a refactor.
- **VALID (resolved-by-design)** — verdict-table marker for entries
  whose body identifies a refactor target but whose validation paragraph
  reframes the entry as already-correct or already-resolved. The body is
  kept for the historical record; the effort label is `none`.

## Effort classes

`trivial` (one-line edit) → `small` → `medium` → `large` → `structural`
(multi-PR; reshapes module boundaries).

## Sections

| File | Range | Title |
| ---- | ----- | ----- |
| [01-wire-serialisation.md](01-wire-serialisation.md) | 1-10 | Duplicated wire / serialisation logic |
| [02-parallel-stores.md](02-parallel-stores.md) | 11-16 | Parallel store implementations |
| [03-repeated-boilerplate.md](03-repeated-boilerplate.md) | 17-25 | Repeated boilerplate |
| [04-per-platform-symmetry.md](04-per-platform-symmetry.md) | 26-29 | Per-platform symmetry |
| [05-inheritance-flattening.md](05-inheritance-flattening.md) | 30-35 | Inheritance chains worth flattening |
| [06-multi-impl-base.md](06-multi-impl-base.md) | 36-47 | Multi-implementation patterns that could share a base |
| [07-dead-stale-code.md](07-dead-stale-code.md) | 48-60 | Dead or stale code |
| [08-duplicated-parsers.md](08-duplicated-parsers.md) | 61-68 | Duplicated parsers / regexes |
| [09-cache-keys.md](09-cache-keys.md) | 69-71 | Cache-key construction scattered across modules |
| [10-other.md](10-other.md) | 72-88 | Other distinct candidates |
| [11-legacy-cli.md](11-legacy-cli.md) | 89-95 | Legacy CLI duplication |
| [12-libutil-libstore-core-extras.md](12-libutil-libstore-core-extras.md) | 96-104, 218 | libutil + libstore-core extras |
| [13-libexpr-extras.md](13-libexpr-extras.md) | 105-120 | libexpr extras |
| [14-vestigial-stdlib.md](14-vestigial-stdlib.md) | 121-130 | Vestigial code, dead workarounds, stdlib replacements |
| [15-globals-settings.md](15-globals-settings.md) | 131-149 | Globals, settings architecture, and dependency-injection |
| [16-libstore-build-audit.md](16-libstore-build-audit.md) | 150-160 | libstore/build deep audit |
| [17-cross-cutting.md](17-cross-cutting.md) | 161-175 | Cross-cutting: platforms, headers, magic numbers |
| [18-daemon-protocol-audit.md](18-daemon-protocol-audit.md) | 176-188 | Daemon and protocol-dispatch deep audit |
| [19-eval-core-fetcher-lookup-json.md](19-eval-core-fetcher-lookup-json.md) | 189-200 | libexpr eval-core: fetcher primops, lookup-path, JSON |
| [20-eval-core-cache-attrset-profiler.md](20-eval-core-cache-attrset-profiler.md) | 201-209 | libexpr eval-core: cache, attr-set, profiler |
| [21-eval-core-evalstate-value.md](21-eval-core-evalstate-value.md) | 210-217 | libexpr eval-core: EvalState and Value |
| [22-cross-shard-patterns.md](22-cross-shard-patterns.md) | N1-N23 | Cross-shard patterns (pattern-discovery pass 1) |
| [23-c-api-debt.md](23-c-api-debt.md) | N24-N25, N29 | C-API surface duplication (Cluster E) |
| [24-test-infrastructure-mirrors.md](24-test-infrastructure-mirrors.md) | N28, N39, N41 | Test-infrastructure mirrors (Cluster F) |
| [25-dispatch-tables-and-schemas.md](25-dispatch-tables-and-schemas.md) | N26, N30, N32-N38, N40, N42-N44 | Dispatch tables and schemas (Cluster G) plus other pass-2 candidates |

The N-numbered candidates were generated by a cross-shard pattern-discovery
pass and intentionally use a separate numbering range (N1-N44) so the
existing 1-217 cross-references stay stable. N1-N23 are pass-1 candidates;
N24-N44 are pass-2 candidates.

## Pattern clusters

The 21 per-shard sections plus the four cross-shard sections (22-25)
group candidates by *symptom location*. Several cross-cutting *patterns*
span multiple sections:

- **Cluster A — Concurrent state and globals.** `getFileTransfer`
  (#147), `windowSize` (#148), `getInterruptCallbacks` (#148-note),
  `logger` (#143), `drvHashes` (#139), `Counter::enabled` (#205),
  `PosTable::origins_`, the `Sync<map>` vs `concurrent_flat_map`
  choice (N1), the `ThreadPool` vs `nix::asio` vs `Goal::Co` axis (N8).
  Architectural direction: name a "global runtime state" subsystem
  with DI sequencing; covered by N17 / Cluster A.
- **Cluster B — Variant-shaped types.** #20 (`MAKE_WRAPPER_CONSTRUCTOR`),
  #94 (`BuiltPath`/`SingleBuiltPath`), #102 (Unkeyed/Keyed),
  #194 (`LookupPath::Elem`); plus the 13+ ad-hoc typedefs
  (`SingleDerivedPath`, `DerivedPath`, `OutputsSpec`,
  `ContentAddressMethod`, `ContentAddressWithReferences`, ...) all want
  one `TaggedUnion<Tag, Alternatives...>` template.
- **Cluster C — SourceAccessor and Store factories.** N7
  (`openAccessor(spec)`), #11/#12 (delegating store), N3 (registries
  of factories — covers `RegisterCommand`/`RegisterPrimOp`/
  `RegisterBuiltinBuilder` etc.).
- **Cluster D — Settings-tied configuration.** The 21-22 `*Settings`
  structs (N4), the `_NIX_TEST_*` and `NIX_*` env-var protocol
  (#146 + N18), the SQLite cache schema versions encoded in filenames
  (N13, N15), the trust-check and version-check ad-hoc dispatches
  (#178, #179).
- **Cluster E — C-API surface duplication.** Section 23. N24 (opaque
  wrappers), N25 (init idempotency), N29 (NIXC_CATCH_ERRS variants);
  plus the existing #50, #87, #88 forwarder pairs.
- **Cluster F — Test-infrastructure mirrors.** Section 24. N16
  (production reflection mirrors test grid), N28 (meson env-var
  plumbing), N39 (test-grid declarative), N41 (per-fixture
  unitTestData boilerplate).
- **Cluster G — Dispatch tables and schemas.** Section 25 plus the
  catalog's #36, #66, #176, #178, #181, #182, #211, plus pass-1's N3,
  N12, N20, N23, plus pass-2's N26, N32, N40. Architectural direction:
  "schema as data, not code" with `boost::describe` (U10) as the
  enabling technology.

## Architectural debt themes

Six themes that the per-shard sections document at the per-candidate
level but don't articulate as named architectural debt:

- **A1 — No `EvalContext` separating eval state from settings and
  primop helpers.** Slices: #131, #132, #133, #138, #209, #210, #212,
  N19, N33.
- **A2 — No `IOContext` / `Executor`.** Slices: #147 (curl thread),
  #160 (Goal coroutine), N8 (three async idioms), N43 (per-subsystem
  `std::thread` lifecycle).
- **A3 — No "store wrapper" abstraction.** Slices: #11, #12, N7
  (`openAccessor(spec)` is the SourceAccessor-side counterpart).
- **A4 — No `LoggingContext`.** Slices: #143, plus the four
  orthogonal logging globals named there.
- **A5 — No unified Configuration architecture.** Slices: #134, #135,
  #136, #144, N4, N18, N44.
- **A6 — No declarative protocol schema.** Slices: #176, #177, #178,
  #179, #181, #182, N12, N23, N32, N40.
- **A7 — No `SignedFingerprint` / `Keyed` / `Renderable` traits.**
  Slices: #102, #104, N2, N5.

## C++23 / Boost uplift series

The catalog has scattered C++23/Boost uplift candidates that form a
coherent staged series. Recommended order is U1 → U4 → U2 → U3 → U6 →
U5 → U7 → U10 → U8 → U9. U10 is the highest-leverage middle-of-graph
step; it unblocks generation-of-protocol-code and
generation-of-JSON-serialisers.

- **U1 (small) — libc++ 16 cleanup.** Cand. #121. Replace 17 `// TODO
  libc++ 16` workarounds with defaulted `operator<=>`. Risk minimal.
- **U2 (trivial) — `append` shim deletion.** Cand. #130. Use
  `std::ranges::append_range`. Already on branch
  `vibe-coding/cleanup/libutil`.
- **U3 (small) — `Finally`-as-`MaintainCount`.** Cand. #129. Stack
  sites only; defer goal-hierarchy `MaintainCount`.
- **U4 (trivial) — `std::unreachable()` for dead-after-exhaustive
  switches.** Cand. #172. ~4 of 64 sites; keep `nix::unreachable`
  helper for any reachable path (UB-vs-panic distinction).
- **U5 (medium) — `std::move_only_function`.** Cand. #122. Replace
  `nix::fun<Sig>` at fixed call sites; `std::function` retained for
  shared/copyable cases. Note layout drift in `PrimOp::impl` and
  symbol-mangle break in `stackOverflowHandler`.
- **U6 (trivial) — `gsl::not_null` evaluation.** Cand. #123. Per
  validation note: keep `nix::ref<T>`, only drop `bad_ref_cast` in
  favour of `std::bad_cast`.
- **U7 — `concurrent_flat_map` vs `Sync<map>` rationalisation.** Cand.
  N1 (split into N1a/N1b per pass 2). Three sub-stages:
  - **U7a (small):** write the rubric and `getOrInsertConcurrent`
    helper.
  - **U7b (medium):** migrate the actual cache sites (4-5 sites).
  - **U7c (out of scope):** the iteration-needing registries
    (`gc.cc::connections`, `remote-store.hh::connectionFds`,
    `filtering-source-accessor.cc::allowedPrefixes`) are *not*
    candidates for migration — they need ordered iteration semantics
    that `concurrent_flat_map` doesn't provide; document why.
- **U8 (structural) — `boost::asio::awaitable` migration.** Cand.
  #160 + N8. Replace `Goal::Co::SuspendAwaiter` and
  `ChildEventAwaiter` with asio primitives, keeping the bespoke
  `final_awaiter`. Migrate `Callback<T>` consumers (N8). Depends on
  prior #150/#151.
- **U9 (large; last) — `std::format` migration.** Cand. #125.
  226 `boost::format`/`HintFmt` matches. Touches every error
  message; daemon-client error-text preservation matrix (E4) is a
  prerequisite.
- **U10 (medium-large) — `boost::describe` reflection.** Cand. #9 +
  #176 + N12 + N16 + N32 + N35 + N38 + N39. Header-only; auto-derive
  `nlohmann::json` adl_serializer and protocol Serialise<T>
  specialisations; eventually auto-derive `*Settings` struct
  surface. **Highest-leverage middle-of-graph step.**

## Redundant abstractions

Cases where the codebase has 2+ in-tree abstractions solving overlapping
problems and the choice is not consistent. Each pair needs a documented
choice rule.

- **R1 — `nix::Sync<T>` vs leaked-pointer + `std::mutex`.** Cands.
  #147, #148, N17. Rule: `Sync<T>` for ordered-iteration / cv-wait;
  leaked `Sync<T>` only for SIOF-load-bearing process-wide singletons.
- **R2 — `nix::ref<T>` vs `gsl::not_null<shared_ptr<T>>` vs
  `std::shared_ptr<T>`.** Cand. #123. Rule: keep `ref<T>`, drop
  `bad_ref_cast`.
- **R3 — `boost::concurrent_flat_map` vs `Sync<unordered_map>` vs
  `Sync<map>`.** Cand. N1. Rule: see U7.
- **R4 — `Pool<R>` vs hand-rolled connection cache.** Cands. #16,
  #184, N10. Rule: `BasicConnection<Proto>` template + `PooledClientStore<C>`.
- **R5 — `nix::fun<Sig>` vs `std::function<Sig>` vs
  `std::move_only_function<Sig>` vs templated functor.** Cand. #122.
  Rule: stable-call-site → templated parameter; one-shot move-only →
  `std::move_only_function<Sig>`; shared/copyable → `std::function<Sig>`.
- **R6 — `std::list<std::string>` vs `std::vector<std::string>` vs
  `boost::container::small_vector<std::string, N>`.** Cand. #171.
  Choose explicitly per-site; document the rule.
- **R7 — `Callback<T>` vs `boost::asio::awaitable<T>` vs `Goal::Co`
  vs `promise/future`.** Cand. N8. Pick `asio::awaitable<T>`.
- **R8 — `Counter` (libexpr) vs `MaintainCount<T>` vs raw
  `std::atomic<uint64_t>` vs raw `uint64_t`.** Cands. N14, N21, #205.
  Pick one shape per axis (per-instance vs RAII vs ID-mint vs metric).
- **R9 — `MakeError(name, parent)` vs `class X final : public
  CloneableError<X, BaseError>`.** Cand. #18. Two error-declaration
  patterns; converge on a single shape.
- **R10 — `nix::asio` vs `std::async` vs raw `std::thread` /
  `std::jthread`.** Cands. N8, N43. Migrate the 6+ `std::thread`
  member fields with hand-rolled lifecycles to `std::jthread` or to
  asio-managed workers.

## Cross-references

Candidates reference one another by number (e.g. "compounds with #34"). The
range column above is the lookup table — find the section file by number,
then the heading inside that file.

Candidate bodies may also carry a `**Branch:**` line pointing at the
`vibe-coding/cleanup/<shard>` branch on origin that addresses the
candidate. Cleanup branches are long-lived and shard-scoped — multiple
candidates in the same shard accumulate as separate commits on a
single branch, reviewed together as one PR per shard. The `**See also:**`
line cross-references associated review reports under
[`../review/`](../review/) (E/F/B/C/V/N series).

## Working on the catalog

If you are picking up this work (whether human or agent), read in
order:

1. [`../INVENTORY.md`](../INVENTORY.md) — codebase navigation map.
2. This file — catalog scope, eight-debt-shape framing, evidentiary
   standard.
3. [`../STATUS.md`](../STATUS.md) — what's queued, in flight, blocked.
4. [`../review/00-INDEX.md`](../review/00-INDEX.md) — index of every
   investigation report.
5. [`../review/AGENT-CHARTER.md`](../review/AGENT-CHARTER.md) —
   operational rules for any investigation.

The catalog is being worked through three layers concurrently:

- **Per-candidate refactors landing as PRs.** Each addressed
  candidate gets a `vibe-coding/cleanup/<name>` branch, pushed to
  origin, with a worktree under `nix-worktrees/`. The seven branches
  in flight today are listed in `STATUS.md`.
- **Investigations producing reports under `review/`.** Adversarial,
  pattern-discovery, follow-up, and verification passes generate
  reports that update the catalog. The eight-rule evidentiary
  standard (above) governs report quality.
- **Catalog maintenance** as findings land. Counts re-derived, stale
  references corrected, new candidates added with cross-references
  to the original 1-217 numbering.

## Related

- [`../INVENTORY.md`](../INVENTORY.md) — navigation map and preserved
  invariants across the 19 verified shards.
- [`../verified/`](../verified/) — the per-shard verified inventories that
  the candidates cite.
- [`../review/`](../review/) — investigation reports.
- [`../STATUS.md`](../STATUS.md) — running ledger of pending /
  in-flight / blocked work.
