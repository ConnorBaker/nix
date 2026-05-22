# C1 — marginal verified-shard observations

Investigates the five observations the consolidation log clusters as four "marginal" deferrals from
the verified shards. For each: source check end-to-end, then a decision (new candidate / body
annotation / INVENTORY-only / no action). Cross-checks against existing N17 / N20 / N21 / N42
follow at the end, plus a few adjacent observations the prior reviewers underplayed.

## Observation 1: `SourceAccessor::number` invariant

### Source check

`src/libutil/source-accessor.cc` defines a file-local `static std::atomic<size_t> nextNumber{0}`,
and the ctor body is `number(++nextNumber)`. The header declares `const size_t number;` as a
public field plus default-derived `operator==` and `operator<=>` over `number`. The ID is also
mixed into `hash_value(const SourcePath &)` (`src/libutil/include/nix/util/source-path.hh`),
which combines `accessor->number` with `path` via boost-style hash, and into
`SourcePath::operator<=>` via `std::tie(*accessor, path)` (which routes back through the
`number`-based comparison).

So the invariant has three load-bearing consequences:

1. Two `SourceAccessor` instances are unequal even if they wrap the same logical filesystem.
   Pointer equality is *not* sufficient (you can have two `make_ref<...>` results), and
   `number` is the only equality basis.
2. `SourcePath`'s hash and comparison both depend on `accessor->number`. A `SourcePath` over a
   freshly built accessor is non-equal to a `SourcePath` over an equivalent accessor created
   earlier — even if they read the same bytes.
3. The counter is process-global and pre-increment-then-store, so id 0 is unused (acts as the
   "uninitialised" sentinel for any `size_t` field that defaults to 0).

This is genuine and current. The catalog already has it documented in INVENTORY.md's "Key
invariants and guarantees" section (the bullet point under
`SourceAccessor::operator==` / `operator<=>` are derived from `number`).

### Decision

**INVENTORY-only invariant — already there.** No catalog action needed.

The pass-1 / consolidation framing was correct: this is a marginal observation that is already
in INVENTORY.md, and the *refactor* angle (the `nextNumber` counter idiom) is already covered by
N42 (atomic ID counters — `source-accessor.cc::nextNumber{0}` is the first cited site). No body
annotation needed; N42 already names this site explicitly.

The one possible body strengthening: N42 currently lists `nextNumber` as a process-global ID
counter without naming the *consumer* invariant (that `operator==` and `<=>` use the result).
That's adjacent rather than required, since N42 is about counter unification, not about
`SourceAccessor` semantics. Leave as-is.

## Observation 2: `Bindings` k-way merge

### Source check

`src/libexpr/include/nix/expr/attr-set.hh` defines:

- `Bindings::maxLayers = 8` — caps the linked-list chain depth.
- `Bindings::numLayers` / `Bindings::baseLayer` — intrusive layer chain.
- `Bindings::iterator` — forward iterator with private `BindingsCursor` struct
  (current/end/priority) and a `boost::container::static_vector<BindingsCursor, maxLayers>`
  backing a `std::greater<BindingsCursor>` heap.
- `iterator::push` / `pop` use `std::ranges::make_heap` / `std::ranges::pop_heap`.
- `consumeAllUntilCurrentName` skips any cursor whose head is `<= lastHandledName`, so
  later-priority entries (lower priority numerals win because of the `std::greater`
  comparator) shadow earlier ones. The constructor primes the heap by walking the layer
  chain from the outermost to the innermost (`while (layer) { ... layer = layer->baseLayer; }`).
- The `doMerge` flag short-circuits the heap path when `attrs.baseLayer == nullptr`, so the
  unlayered case stays a flat scan.

`BindingsBuilder::layerOnTopOf` sets `bindings->baseLayer` and increments `numLayers`.
`finishSizeIfNecessary` (called from `finish` and `alreadySorted`) recomputes the deduplicated
`numAttrsInChain` against the base layer, using `set_intersection` when the new attrs are
larger than the base, else per-element binary search. This means `Bindings::size()` and
`empty()` reflect the merged size, not just the local FAM.

`ExprOpUpdate::eval` (in `eval.cc`) consults `bindingsUpdateLayerRhsSizeThreshold` (default 16
on 64-bit) and `Bindings::isLayerListFull()` (which checks `numLayers == maxLayers`) to decide
between layering and a flat memcpy-merge.

So the k-way merge is genuine, current, and load-bearing for evaluation correctness:
`for (auto & attr : *bindings)` *must* preserve `Symbol`-sorted order across layers, and lookups
via `Bindings::get` walk the chain doing per-layer binary search. Any refactor that changes
the merge ordering or the heap discipline silently breaks evaluation.

### Decision

**Body annotation on existing #206 — already done.** The consolidation pass already applied
this. From `doc/inventory/candidates/20-eval-core-cache-attrset-profiler.md` (#206 validation
paragraph): *"Invariant note: the k-way merge across layers (preserved by Bindings::iterator)
is itself the load-bearing invariant — when refactoring, document that the iteration order
remains a heap-merge across layers, capped at maxLayers = 8."*

INVENTORY.md's "Key invariants and guarantees" section also has the bullet
*"`Bindings::iterator` performs a k-way merge across layers"* with the cap.

So the body annotation has landed *and* the invariant is in INVENTORY.md.
No further action needed.

One minor adjacent point worth flagging: #206's body says "the cap should be a setting
alongside `bindingsUpdateLayerRhsSizeThreshold`, or there should be a regression benchmark
pinning `maxLayers = 8` as the optimum". The validation paragraph adds the type-system
constraint that exposing `maxLayers` as a `Setting<unsigned>` breaks the
`static_vector<..., maxLayers>` compile-time sizing. That tradeoff is already in #206.

## Observation 3: `Counter::enabled` runtime branch

### Source check

`src/libexpr/include/nix/expr/counter.hh` defines `struct Counter` cache-line-aligned wrapping
`std::atomic<uint64_t> inner{0}` plus `static bool enabled`. Each mutator (`++`, `++(int)`,
`--`, `--(int)`, `+=`, `-=`) is `enabled ? <atomic op> : 0`. Loaders (`operator value_type()`,
`load()`) are unconditional atomic reads. `Counter::enabled` is defined in
`src/libexpr/eval.cc` as `bool Counter::enabled = getEnv("NIX_SHOW_STATS").value_or("0") != "0";`
and gates `EvalState::maybePrintStats()` (`if (Counter::enabled) { ... printStatistics(); }`).

So when `NIX_SHOW_STATS` is unset, every Counter mutator returns `0` without touching the
atomic. Loaders still read the atomic — but since enabled-false leaves `inner` at 0, the
result is always 0. The branch is paid on every increment site (13 per `EvalState` /
`EvalMemory`), but is a predictable always-false branch in the disabled case, so it's
realistically free post-CPU-branch-prediction.

The `enabled` field being a static `bool` also makes Counter participate in the
"singleton-with-startup-init" hazard class flagged by N17 — pass-1's framing of N17 explicitly
names *"`Counter::enabled` static (counter.hh) reads `NIX_SHOW_STATS` once at startup"* as one
of the SIOF mitigation patterns adjacent to the leaked-Sync-pattern. That's already there.

### Decision

**Body annotation on existing #205 — already done.** The consolidation pass applied:
*"the `enabled` runtime branch already short-circuits the atomic increment when off, so the
existing design already avoids the per-increment cost when disabled — the alignment cost is
the only remaining concern."* (`doc/inventory/candidates/20-eval-core-cache-attrset-profiler.md`,
#205 validation paragraph.)

INVENTORY.md's invariant block also has the bullet
*"`Counter` arithmetic short-circuits when `Counter::enabled` is false. Set by `NIX_SHOW_STATS`.
When disabled, every increment/decrement returns 0 without touching the atomic. Per-eval
counters (`nrAvoided`, etc.) are gated on this."*

Both the candidate body and the cross-shard invariant section are in good shape. No further
action needed.

One adjacency that nobody named: `Counter::enabled` is the *one* of the four counter shapes
(per N21) that has a runtime-disable gate. `MaintainCount<T>` doesn't; the `Worker` plain
counters don't; `Logger::nextId` doesn't (and shouldn't — it's an ID generator). So if N21
ever lands and unifies the four shapes, the `enabled` gate is a `Counter`-specific
optimisation that doesn't generalise. Worth a sentence in N21's body, but minor.

## Observation 4: `SymbolTable` append-only

### Source check

`src/libexpr/include/nix/expr/symbol-table.hh`:

- `SymbolTable` has private `BumpMemoryResource buffer` (monotonic; never frees), private
  `SymbolStr::SymbolValueStore store` (a `ChunkedVector<SymbolValue, 65536, ...>`), and a
  `boost::concurrent_flat_set<SymbolStr, ...> symbols` for interning.
- `create(string_view)` uses `insert_and_cvisit` to either append to `store` (via
  `SymbolStr(const Key &)` on miss) or look up (on hit). Both branches return the same
  `Symbol`, so duplicate inserts are idempotent.
- `operator[](Symbol s) const` is `s.id - 1` lookup into `store`, with `unreachable()` on
  out-of-bounds. The class comment notes: *"SymbolTable is an append only data structure.
  During its lifetime the monotonic buffer holds all strings and nodes ..."* and the
  load-side comment explains the happens-before reliance on `concurrent_flat_set` /
  `ChunkedVector`.
- `Symbol::id == 0` is reserved (the default-ctor `Symbol{}` returns id 0, and
  `operator bool()` returns `id > 0`). `StaticSymbolTable::create` allocates `Symbol(size + 1)`
  to honour this.
- `SymbolStr(const Key &)` asserts `size < numeric_limits<uint32_t>::max()`. The chunked
  vector itself caps at `MaxChunks = numeric_limits<uint32_t>::max() / 65536 = 65535` chunks
  (i.e. ~4G symbols), which is well above the assertion bound.

So append-only is enforced by:

- No public eraser (`SymbolTable` has no `erase` / `remove` / `clear`).
- No private eraser invoked elsewhere (grep confirms).
- `BumpMemoryResource` doesn't `deallocate` (per its name).
- `ChunkedVector::add` is the only growth path; chunks are never freed.

`StaticSymbolTable::copyIntoSymbolTable` walks the static entries in order and asserts that
each runtime-assigned `Symbol` matches the static one. This works *only* because the runtime
`SymbolTable` is empty at that point (#208's framing). The append-only invariant is also what
makes the static-id replay safe — once the static entries are interned, the next `create`
gets the next sequential id and `EvalState::s.<name>` references stay valid.

### Decision

**Body annotation on existing #207 — already done.** The consolidation pass applied:
*"Invariant note: SymbolTable is append-only and concurrent (verified-shard invariant);
preserve that across any refactor — std::deque<SymbolValue> shares pointer-stability but not
concurrent-append (would need Sync<deque> or similar)."*

INVENTORY.md's invariant block has:
*"`SymbolTable` is append-only and concurrent. Symbol IDs start at 1 (id 0 is the 'unset'
sentinel). Static symbols (`with`, `outPath`, etc.) are pre-allocated at compile time and
replayed at runtime so that `EvalState::s.<name>` never allocates."*

Both halves of the documentation are in place. No further action needed.

One adjacency worth flagging adversarially: the `SymbolStr(const Key &)` ctor adds to the
`store` *before* the `concurrent_flat_set::insert` succeeds. Per the file comment
*"strings are unique, so that a pointer comparison is OK"* — this is true *because* the
flat-set's transparent equality compares strings (not pointers) on insert. If two threads
race to intern the same string, both would call `SymbolStr(Key)`, both would
`store.add(SymbolValue{})` (so the store grows by 2 even though only one symbol is exposed),
then exactly one wins the flat-set insert. The losing thread's `SymbolValue` is orphaned but
not destroyed (append-only). So the append-only invariant has a *secondary* consequence:
under contention, the store size can exceed the symbol count. This is fine semantically (the
loser is unreachable), but it's a bookkeeping difference between `store.size()` and the
symbol count that's worth knowing if anyone benchmarks the table. **Optional small body
addition to #207** noting that under contended interning the store can grow faster than the
symbol set; not load-bearing for refactoring, but adversarial honesty. Decision: **mark as
optional; not required.**

## Observation 5: `dummy` / sentinel pattern

### Source check

Two static `dummy` instances live in the in-tree headers:

- `StorePath::dummy` (`src/libstore/include/nix/store/path.hh`, defined in
  `src/libstore/path.cc`) — `StorePath("ffffffffffffffffffffffffffffffff-x")`. The `f`-padded
  hash is intentionally an all-ones sentinel; the name `"x"` is the `MissingName` placeholder.
- `Hash::dummy` (`src/libutil/include/nix/util/hash.hh`, defined in
  `src/libutil/hash.cc`) — `Hash(HashAlgorithm::SHA256)` with the default zero-filled `hash[]`
  buffer.

Adjacent sentinels:

- `Store::MissingName = "x"` (`src/libstore/include/nix/store/store-api.hh`) — the placeholder
  used in `StorePath::dummy`'s name and in `goodStorePath`, where
  `expected.name() == Store::MissingName` allows the expected path to match any actual path
  with the same hash part. Also used by `LocalBinaryCacheStore::queryAllValidPaths`
  (constructs paths from disk) and `BinaryCacheStore::narFromPath` (constructs a pseudo-path
  for substitution).
- `Symbol::id == 0` reserved sentinel (covered above).
- `noPos` (`src/libutil/include/nix/util/pos-idx.hh`) — `inline PosIdx noPos = {};` (default-
  constructed, so id 0). Used as a default argument in many AST node ctors / signatures.
- `Bindings::emptyBindings` — read-only zero-attr singleton instance.
- `Value::vEmptyList`, `Value::vNull`, `Value::vTrue`, `Value::vFalse` — covered by the
  catalog as #213 (the "documented not singleton — yet `getBool` and `ExprList::maybeThunk`
  hand out their addresses" candidate).
- `COMPRESSION_LEVEL_DEFAULT = -1` (libutil compression) — sentinel for "library default".
- `BaseNix32::reverseMap`'s `invalid` sentinel — internal lookup-table marker.

Usage of the two `dummy` sentinels:

- `Hash::dummy` is used as a placeholder in code paths that need a `Hash` object before the
  real hash is known: the SQLite `queryPathInfoInternal` parses the stored hash but starts
  from `auto narHash = Hash::dummy;`; the `LegacySSHStore::queryPathInfosUncached` rejects
  any `info.narHash == Hash::dummy` with `"NAR hash is now mandatory"` (i.e. `Hash::dummy` is
  the wire-marker for "missing"). `nar-info.cc::NarInfo` ctor uses it as a placeholder during
  parsing (annotated with `// FIXME: hack`). `serve-protocol.cc` constructs a
  `dummyId = Hash::dummy.to_string(...)+"!"+outputName` for protocol fall-back outputs.
  `make-content-addressed.cc` uses it when constructing a `ValidPathInfo` whose hash will be
  recomputed.
- `StorePath::dummy` is used similarly for code paths that need a placeholder `StorePath`.
  `nar-info.cc::NarInfo` ctor again (`// FIXME: hack`). `derivation-trampoline-goal.cc`
  initialises `auto drvPath = StorePath::dummy;` then overwrites in a `try`/`catch` block.
  `unix/build/derivation-builder.cc` uses it in the `Hash::dummy` companion site.

### Existing catalog coverage

INVENTORY.md's invariants section already has:

- *"`StorePath::dummy` is `'ffffffffffffffffffffffffffffffff-x'`; `StorePath::MissingName = 'x'`
  is the placeholder when only the hash is known."*
- *"`Hash::dummy` is a zero SHA-256."*

The `// FIXME: hack` comments in `nar-info.cc` are not currently a candidate target; the
broader sentinel pattern is not catalogued.

### Decision

**No action — but with one potential adjacent candidate noted.** The marginal observation
is correctly marginal: the two `dummy` constants are isolated, their meaning is documented in
INVENTORY.md, and N42 covers the *atomic ID counter* shape (which is the active-refactor
adjacent pattern, not the dummy pattern).

That said, a real catalog-worthy observation is hiding here: **the pattern of "construct a
placeholder, then overwrite via field assignment" is itself a smell at three sites
(`local-store.cc::queryPathInfoInternal`, `derivation-trampoline-goal.cc`,
`nar-info.cc::NarInfo` ctor), and the `nar-info.cc` site is even annotated `// FIXME: hack`
twice.** This isn't really about `dummy` — it's about *delayed-initialisation in classes
whose ctors don't yet support partial construction*. The right factoring is either
(a) a `std::optional<Hash>`-typed member (with an explicit "uninitialised" state instead of a
sentinel-valued `Hash`), or (b) a two-phase ctor pattern. Neither is in the catalog.

But this is a separate observation from the "is `dummy` worth tracking" question, and it
already has two `// FIXME: hack` markers in `nar-info.cc` driving any future cleanup — so the
codebase itself is signalling. **I considered proposing a new candidate "rationalise
`Hash::dummy` / `StorePath::dummy` placeholder-then-overwrite sites with their `// FIXME:
hack` markers"** but the scope is genuinely small (5 sites total, 2 already FIXME-tagged) and
the right fix depends on how the surrounding ctors are restructured (e.g. once `NarInfo`'s
parser is a free function rather than a member ctor). **Decision: log as an "open question"
below for the user to consider explicitly; no candidate proposed in this pass.**

## Cross-check against existing N-candidates

Five candidates are most relevant. Each is checked for whether the marginal observations are
already subsumed.

- **N17 (leaked-Sync<T*> SIOF pattern)**: Pass-1's body explicitly names `Counter::enabled`
  static as one of the SIOF-mitigation patterns adjacent to the leaked-Sync pattern. So
  Observation 3's framing is in N17's body. No double-counting needed.
- **N20 (`nValueType` table-driven dispatch)**: Not relevant to any of the five marginal
  observations. The nearest adjacency is that `SymbolTable` interns *strings*, not values;
  unrelated to the value-type dispatch.
- **N21 (counter pattern, four shapes)**: This is the architectural keystone for #205 and
  Counter overall. Observation 3 (`Counter::enabled`) is a refinement of the
  `Counter`-shape arm of N21. Not a duplicate; complementary.
- **N42 (atomic ID counters, five sites)**: This is the keystone for the `nextNumber{0}`
  idiom in `SourceAccessor`. Observation 1 (`SourceAccessor::number`) is fully named in N42's
  body. So the catalog *does* track the refactor angle of Observation 1.
- **#205 / #206 / #207**: Already discussed above.

**Summary: Observations 1-5 are all subsumed by existing INVENTORY.md invariants and / or
existing candidate body annotations.** The consolidation log's framing was correct: these are
genuinely marginal, the consolidation pass already applied the body annotations it described,
and INVENTORY.md already documents them.

## Adjacent observations

While reading the verified shards end-to-end, four adjacent points the prior reviewers either
underplayed or didn't surface:

### A1. The `Hash::dummy` / `StorePath::dummy` placeholder-then-overwrite pattern is a small candidate target

Five sites use `Hash::dummy` or `StorePath::dummy` as a "null-ish" placeholder before the
real value is computed. Two of those sites (`nar-info.cc::NarInfo` ctor) are explicitly
annotated `// FIXME: hack`. The right factoring is a `std::optional<Hash>` / `std::optional<
StorePath>` field with `.has_value()` checks, replacing the sentinel-valued check
`info.narHash == Hash::dummy` (in `legacy-ssh-store.cc`) with an explicit unset state. This
is small (5 sites), has FIXME markers driving it, and surfaces a genuine ambiguity (right now
`Hash::dummy` is overloaded as both "uninitialised" and "wire-marker for missing"; an
optional field disambiguates). **Worth filing as a small candidate**, possibly as an
N42-adjacent entry or as a new entry under "Latent bugs hiding inside duplication" / "vestigial
stdlib" themes. *Open question for the user.*

### A2. `Bindings::iterator`'s "duplicate symbol shadowing" is documented in code but not in INVENTORY.md

`BindingsCursor::consume(Symbol name)` skips entries with `name <= lastHandledName`, and
`consumeAllUntilCurrentName` then drains shadow-equal entries from later layers. The
INVENTORY.md bullet says the iterator does k-way merge but doesn't note the *priority-based
shadowing* — i.e. earlier-layer entries override later ones for the same name. This is the
load-bearing semantic of `//` (the Nix attrset update operator). A reader of INVENTORY.md
could miss that the merge order is *priority-then-name*, not *name-only*. **Optional one-line
expansion of the existing INVENTORY.md bullet** to make this explicit. *Marginal; optional.*

### A3. `Symbol::id == 0` reserved-sentinel coupling with `ExprVar::name`

`Symbol::id == 0` is the unset sentinel. `ExprVar::name` is a `Symbol`; an unbound `ExprVar`
can have `name` defaulting to id 0 (e.g. via the default-ctor branch of the AST). This means
`if (var.name)` works as a "name has been set" check. But this isn't documented anywhere in
INVENTORY.md — and a refactor that changes `Symbol`'s default-ctor (e.g. to mark the
unset state with `std::optional<Symbol>`) would silently break that contract. **Worth a
sentence in INVENTORY.md's invariant block** if one wanted maximal documentation; minor.
*Marginal; not required.*

### A4. `noPos` (`PosIdx{}`) is a default-arg sentinel used at 100+ call sites

`inline PosIdx noPos = {};` (in `pos-idx.hh`) is the default argument for every position-
sensitive function in libexpr (15+ default-argument signatures). Like `Symbol::id == 0`, this
relies on `PosIdx`'s default constructor producing the "no position" state. None of the
candidates name this explicitly (it's not really a refactor target), but `noPos`-as-default-
argument is a coupling between AST construction and the pos-table machinery worth being aware
of. **No action**; flagged for adversarial-review awareness only.

## Decisions summary

| Observation | Decision | Notes |
| --- | --- | --- |
| 1: `SourceAccessor::number` invariant | INVENTORY-only — already there | N42 covers the refactor angle (atomic ID counters) |
| 2: `Bindings` k-way merge | Body annotation on #206 — already done | INVENTORY.md also has the cross-shard bullet |
| 3: `Counter::enabled` runtime branch | Body annotation on #205 — already done | N17 names the SIOF aspect; N21 is the counter-pattern keystone |
| 4: `SymbolTable` append-only | Body annotation on #207 — already done | INVENTORY.md also has the cross-shard bullet |
| 5: `dummy` / sentinel pattern | No action on the dummy constants themselves | Adjacent A1 is a real but small candidate target (5 sites, 2 FIXMEs); listed as open question |

All four "marginal" observations the consolidation log flagged for deferral are
**legitimately subsumed** by the catalog as it stands. The consolidation pass applied the
body annotations it described; nothing was lost. The fifth (dummy / sentinel pattern) is
also genuinely marginal at the *constant* level, but I surfaced one adjacent finding (A1)
that *is* a small candidate target and that nobody named.

## Open questions

1. **`Hash::dummy` / `StorePath::dummy` placeholder pattern (A1).** Five sites use the dummy
   constants as null-ish placeholders before computing the real value. Two are annotated
   `// FIXME: hack` in `nar-info.cc`. The right factoring is `std::optional<Hash>` /
   `std::optional<StorePath>` fields, removing the sentinel ambiguity (where `Hash::dummy` is
   overloaded as both "uninitialised" and "wire-protocol-missing"). Should this be a small
   new candidate (perhaps under N42-adjacent or "Latent bugs hiding inside duplication"),
   or is it small enough to leave to the existing `// FIXME: hack` markers to drive a
   one-shot cleanup? My recommendation: **file as a small candidate** under
   `12-libutil-libstore-core-extras.md` or `10-other.md`, citing the two FIXME markers as the
   trigger and `legacy-ssh-store.cc::info.narHash == Hash::dummy` as the load-bearing
   wire-protocol use that needs special handling.

2. **A2 (k-way merge priority-shadowing) and A3 (`Symbol::id == 0` coupling with
   `ExprVar::name`) — INVENTORY.md expansions only.** Both are one-line additions to existing
   bullets. Should I propose them, or are they too marginal? My recommendation: **A2 yes
   (priority-shadowing is the semantic that makes the merge correct for `//`); A3 no
   (`Symbol::getId() == 0` is internal and the existing `operator bool()` convention is
   sufficient).**

3. **`SymbolTable` interning race adversarial note (Observation 4 epilogue).** Under contended
   interning, the `store` can grow faster than the symbol set (loser thread's `SymbolValue` is
   orphaned). This is correct but bookkeeping-noisy; not a refactor target. Worth a sentence
   in #207's body or not? My recommendation: **not — it's a property of the design, not a
   defect, and #207 is already long.**
