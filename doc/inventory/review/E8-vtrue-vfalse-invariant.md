# E8 - vTrue/vFalse/vNull/vEmptyList invariant choice

Charter: decide whether the four canonical Value instances declared in
`src/libexpr/include/nix/expr/value.hh` (`Value::vTrue`, `Value::vFalse`,
`Value::vNull`, `Value::vEmptyList`) should be renamed to drop the singleton
implication, or canonicalised so consumers must reference rather than copy.

The header comment for all four reads:

> This is _not_ a singleton. Pointer equality is _not_ sufficient.

This document inventories every use, classifies it, and recommends a direction.

## Consumer inventory

The full inventory across `src/`, `tests/`, and the C-API is short. Every
reference is either the declaration/definition or an address-of use. No tests,
C-API binding, or evaluator path performs pointer comparison or value-copy
against these four instances.

| File | Identifier | Form | Category |
|------|------------|------|----------|
| `src/libexpr/include/nix/expr/value.hh` | `Value::vEmptyList`, `vNull`, `vTrue`, `vFalse` | `static Value` declaration | Declaration |
| `src/libexpr/value.cc` | `Value::vEmptyList`, `vNull`, `vTrue`, `vFalse` | namespace-scope static definition via IIFE | Definition |
| `src/libexpr/eval.cc` (`EvalState::getBool`) | `&Value::vTrue`, `&Value::vFalse` | `return b ? &Value::vTrue : &Value::vFalse;` | Address-of |
| `src/libexpr/eval.cc` (`ExprList::maybeThunk`) | `&Value::vEmptyList` | returned as the binding `Value*` for empty literal `[]` | Address-of |
| `src/libexpr/primops.cc` (`prim_tryEval`) | `&Value::vTrue`, `&Value::vFalse` | inserted as attr values for the success/value attrs | Address-of |
| `src/libexpr/primops.cc` (`prim_match`) | `&Value::vNull` | written into list slots for unmatched regex groups | Address-of |
| `src/libexpr/primops.cc` (`prim_split`) | `&Value::vNull` | written into list slots for unmatched regex groups | Address-of |
| `src/libexpr/primops.cc` (`createBaseEnv`/`addConstant("null", ...)`) | `&Value::vNull` | passed by pointer to `addConstant`, registered as the env binding for the user-visible name `null` | Address-of |
| `src/nix/nix-env/nix-env.cc` (`loadSourceExpr`) | `&Value::vEmptyList` | inserted as the `_combineChannels` attr value | Address-of |

Categories not present in the codebase:

- Value-comparison consumer (e.g. `v == Value::vTrue`): none.
- Construction reference (e.g. `*v = Value::vTrue;`): none.
- Pointer-comparison consumer (e.g. `p == &Value::vTrue`): none.

A grep collision: `src/libexpr-tests/value/print.cc` declares a *local*
variable named `vNull`. It is unrelated to `Value::vNull`.

## Comment-vs-code analysis

The comment "This is _not_ a singleton. Pointer equality is _not_ sufficient"
asserts a semantic about the language, not about C++ uses of the four
identifiers.

Reading `EvalState::eqValues` in `src/libexpr/eval.cc`:

- `eqValues` first short-circuits on `&v1 == &v2`, but that is a
  performance hack labeled "to support some old broken code" that is
  unrelated to the canonical instances.
- After the short-circuit, `eqValues` dispatches by type and unwraps:
  `case nBool: return v1.boolean() == v2.boolean();`,
  `case nNull: return true;`, `case nList: ... per-element eqValues ...`.

So at the language semantics level, `null == null`, `true == true`, and
`[] == []` must hold *regardless* of which `Value` object holds the underlying
state. A user-constructed `nNull` Value (e.g. allocated via `allocValue()` and
filled by `mkNull()`, as `prim_match`'s no-match branch does on line where it
writes `v.mkNull()` for the whole-string case, or as the JSON parser does in
`json-to-value.cc`) must compare equal to the canonical `&Value::vNull`. The
codebase already commits to this: many primops construct fresh `nBool`/`nNull`
values via `mkBool`/`mkNull` rather than using the canonical pointer.

So the *comment* is a true statement about language semantics, not a
statement about whether the four C++ objects are unique.

But at the C++ level, the *code* is more interesting. Every call site in the
inventory is an "address-of" consumer that hands a `Value*` to be embedded in
some downstream data structure (an attrset, a list element, a binding in
the base env). No site copies the canonical instance, no site compares
addresses against it, and no site makes pointer-identity-based decisions.

The four objects therefore behave *de facto* as singletons in the codebase:

- They are never copied (only their address is taken).
- They are never used as comparison targets (neither by pointer nor by
  value).
- They are never mutated (the only mutating method on `Value` is `mk*`,
  and no `mk*` call exists against any of the four after construction).

The comment's defensive framing is therefore correct as a language-level
invariant ("the evaluator must not assume there is only one true Value"),
but is misleading when read at the C++ object level: the four objects are
in fact singletons in the library's actual usage. The comment as written
discourages downstream consumers from relying on pointer identity, but the
existing call sites already do something stronger: they leak the canonical
pointer into long-lived data (attrsets, env bindings, list backing arrays),
trusting that no one will mutate it.

## Eval-cache impact

`src/libexpr/eval-cache.cc` does not interact with the canonical instances at
all.

The on-disk schema (`AttrType::Bool`, `AttrType::String`, `AttrType::Int`,
`AttrType::ListOfStrings`, plus opaque markers `Placeholder`/`Missing`/
`Misc`/`Failed`/`FullAttrs`) stores raw scalars or symbol lists. There is no
code path that materialises a cached value into a `Value` of type `nNull` or
`nList` and there is no code path that hands out a `Value*` to a cached
boolean.

Concretely:

- `AttrCursor::getBool()` returns `bool`, not `Value*`. It either reads
  the stored integer from sqlite or forces and unwraps `v.boolean()`. It
  never produces `&Value::vTrue` or `&Value::vFalse`.
- `AttrCursor::getInt()`, `getString()`, `getStringWithContext()`,
  `getListOfStrings()` all return primitive C++ types.
- The cache has no `nNull` branch in `forceValue()`'s persistence
  logic. A `nNull` value falls through to `setMisc()`.
- The cache has no `nList` value-persistence branch beyond
  `setListOfStrings`, which iterates `v.listView()` and forces each
  element to a string. There is no code that reconstructs an
  `&Value::vEmptyList` handle from the cache.

Implication for the directional choice: the eval-cache is *invariant under
either direction*. It already operates entirely on primitive scalars; it does
not benefit from canonicalisation (no pointer-dedup opportunity for cached
booleans), and it would not be impacted by renaming away from the singleton
language (no construction sites to migrate).

## C-API impact

`src/libexpr-c/nix_api_value.cc` does not expose any reference to the four
canonical instances.

- `nix_init_bool(value, b)` calls `v.mkBool(b)` on a freshly allocated
  `Value`. A C consumer constructing a true via `nix_init_bool` gets a
  distinct Value object every time, never `&Value::vTrue`.
- `nix_init_null(value)` calls `v.mkNull()` on a freshly allocated
  `Value`. Same story.
- `nix_get_bool(value)` returns `bool`; it does not return a handle.
- There is no `nix_value_eq` or any pointer-comparison API exposed to C
  consumers in this file.
- The `nix_value` wrapper is a `{Value *, EvalMemory *}` pair allocated
  via `state.mem.allocBytes`, and `nix_alloc_value` always calls
  `state.allocValue()`, which goes through the GC allocator. C consumers
  never naturally end up holding a pointer to one of the four canonical
  static instances.

There is one indirect path: a C consumer that walks an attrset produced by
`prim_tryEval`, `prim_match`, `prim_split`, or `prim_functionArgs`, or that
walks the `_combineChannels` attrset from `nix-env`, will obtain via
`nix_get_attr_byname` a `nix_value*` whose internal `Value*` *is*
`&Value::vTrue`/`vFalse`/`vNull` or `&Value::vEmptyList`. That is the only
way for a C consumer to hold a canonical pointer.

`nix_get_attr_byname` calls `new_nix_value(attr->value, ...)` which stores the
raw `Value*` from the attr. So the canonical pointer leaks across the C-API
boundary in those cases. Because `Value::vTrue` etc. are namespace-scope
statics that live for the lifetime of the process, the C consumer's pointer
remains valid forever. (See GC implications below.)

Implication for the directional choice: the C-API is largely orthogonal. It
does not currently expose canonical handles, and it does not currently do
pointer-identity comparison. Direction A (drop the singleton implication)
requires no C-API change. Direction B (canonicalise and document pointer
identity as a fast path) would *enable* exposing canonical handles and a
`nix_value_eq` fast path, but doing so is purely additive and not required.

## GC implications

The four canonical instances are namespace-scope `static Value` definitions in
`src/libexpr/value.cc`. They live in the data segment of `libnixexpr` (or the
process image, depending on linking), not in the GC heap.

- Boehm GC scans the static data segments of the process by default, so
  any pointers stored *inside* `Value::vTrue` etc. would be treated as
  potential roots. In practice the four instances contain no GC-traceable
  pointers (`vEmptyList` has `elems = nullptr`; `vTrue`/`vFalse` carry
  only a packed bool; `vNull` carries no payload).
- Boehm GC will *not* collect the four instances themselves: they are
  static, not heap-allocated.
- A consumer holding `&Value::vTrue` (or any of the others) holds a
  pointer that remains valid for the lifetime of the process and across
  arbitrary collections. There is no use-after-free risk.
- Conversely, the address-of uses in `prim_tryEval`, `prim_match`, etc.,
  store the canonical pointer into Bindings/list backing arrays that
  *are* GC-allocated. Boehm tracing follows the stored `Value*` into the
  static instance and stops there (the static is not in the heap, so
  there is nothing to mark; tracing of its payload is via the data-segment
  root scan).

Direction A increases GC pressure by allocating an additional `Value` for
every Boolean/null/empty-list handed back by the affected primops. Direction
B keeps GC pressure flat (in fact, slightly reduces it relative to today,
since `prim_functionArgs` already saves an allocation per arg via `getBool`,
and the same optimisation could be extended to the more common
`mkBool`-then-stored callers).

## Direction comparison

### Direction A: rename to drop singleton implication

Replace the four `static Value` instances with factory functions, e.g.:

- `static Value makeTrue();`
- `static Value makeFalse();`
- `static Value makeNull();`
- `static Value makeEmptyList();`

Migrate the nine address-of sites to either `state.allocValue()`-and-init
patterns or to passing-by-value where downstream APIs accept a `Value`.

Costs:

- Nine call sites to migrate. Each migration is a 1-3 line change:
  allocate a `Value*` via `state.allocValue()`, init it with the
  appropriate `mk*` call, then use it where the canonical pointer was
  previously used.
- Approximate hot-path cost: per `if`-expression that evaluates to a
  Boolean default in a function-args set, per empty-list literal in a
  source file evaluated, per `null` reference in a source file evaluated.
  In practice eval is dominated by attrset evaluation, function calls,
  and string operations; the four canonicals are a sub-1% optimisation.
- The `_combineChannels` and `prim_match` no-match-group cases would
  allocate one extra `Value` per such occurrence (typically dozens per
  channel reload, dozens per regex match without groups - negligible).
- The `addConstant("null", ...)` site has a subtlety: a per-EvalState
  fresh `Value` would need to be allocated in `createBaseEnv`. This is
  cheap and aligns with how `true`/`false` already work (they are already
  built via `v.mkBool(true)` on a stack `Value` and then copy-installed
  by `addConstant(name, Value &, ...)` into a freshly allocated heap
  Value - see `EvalState::addConstant(... Value & v ...)` in `eval.cc`).
  In other words, `true` and `false` are *already* not aliased to
  `&Value::vTrue` from the user's perspective; only `null` currently
  installs `&Value::vNull` directly.

Benefits:

- The "this is not a singleton" comment becomes truthful at the
  C++-object level, not just at the language level. The defensive comment
  can be removed.
- Eliminates the lone outlier `addConstant("null", &Value::vNull, ...)`,
  bringing it in line with how `true` and `false` are registered.
- Removes an asymmetry where `prim_tryEval` produces an attrset whose
  Boolean members live in the static data segment but `prim_isString` or
  `==` produce attr values that live in the GC heap. After the rename,
  every Boolean/null/empty-list visible to user code lives in the GC
  heap.
- C consumers no longer have an oblique way (via attrset traversal) to
  obtain a canonical static-storage pointer. All `Value*` they see
  uniformly reference GC-managed memory.

Risks:

- None identified. There are no pointer-identity consumers, no
  value-equality short-circuits, and no value-copy users to break.
- A future reader might add an optimisation that *re-introduces*
  canonicalisation, defeating the rename. This is a pure documentation
  risk.

### Direction B: canonicalise

Promote the four to first-class singletons, document pointer identity as a
fast path, and migrate every Boolean/null/empty-list construction in the
evaluator to reference the canonical instance.

Costs:

- Far more invasive. Approximately 30+ `mkBool` call sites and 5+
  `mkNull` call sites in `src/libexpr` would need to be rewritten to
  store `&Value::vTrue` / `&Value::vFalse` / `&Value::vNull` into the
  outparam slot, rather than calling `mk*` on the outparam.
- The outparam idiom is incompatible with canonicalisation: most
  evaluator entry points are `void eval(... Value & v)`. A canonical
  rewrite would have to either (a) replace the outparam with a `Value
  *&` so the caller's slot can be repointed at the canonical address, or
  (b) keep `mkBool` and accept that pointer identity is *not* a useful
  fast path because most Booleans still live in fresh allocations. Both
  options are unsatisfying.
- Eval cache and C-API would need new canonical handles to make the
  invariant useful (e.g. an exposed `nix_value_true`,
  `nix_value_false`, `nix_value_null`, `nix_value_empty_list`). This is
  net new public surface area.
- `eqValues` could shortcut on `&v1 == &v2 == &Value::vTrue`, but
  `eqValues` *already* short-circuits on `&v1 == &v2` for any reason,
  and the per-type unwrap is already O(1) for Boolean/null. There is no
  measurable win.
- The static instances would need to remain `Value` objects (not
  `inline constexpr Value` since `Value` has non-trivial constructors
  and is not a literal type). The current static initialisation order is
  fine.

Benefits:

- Pointer identity becomes a documented fast path for `eqValues`. In
  practice this saves 1-2 ns per Boolean/null comparison, but
  Boolean/null comparisons are rare in evaluation hot paths.
- A future eval cache that wants to dedup cached Booleans or empty lists
  in memory could rely on pointer identity. There is no current need.

Risks:

- Pervasive evaluator rewrite for marginal benefit.
- Outparam conventions across the evaluator would all be touched. This
  conflicts with the parallel direction in the rest of the catalog,
  where outparam conversions are themselves a candidate for refactoring
  (see catalog entries on `eval(... Value & v)` signature uniformity).
  Locking the outparam protocol into "must be repointed for
  canonicals" would obstruct that broader cleanup.

## Recommendation

Adopt Direction A: rename the four to factory functions and migrate the nine
address-of sites to allocate fresh values.

Evidence:

1. The codebase already operates as if these four are not singletons.
   Most Boolean and null constructions go through `mkBool`/`mkNull` on
   freshly allocated outparams. Only the nine inventoried sites use the
   canonical addresses, and they do so opportunistically (to save an
   allocation in cases where the result is going to be embedded as an
   immutable attr/list element).

2. The comment is defensible at the language level but contradicted by
   the code at the C++ level. Renaming to `Value::makeTrue()` etc. makes
   the comment unnecessary - the API itself communicates the
   non-singleton guarantee.

3. The migration cost is small and bounded: nine call sites, each a
   trivial allocate-and-init.

4. The eval cache is already independent of the canonicals.

5. The C-API does not depend on or expose the canonicals (except
   indirectly through attrset traversal of primops that embed them, an
   exposure that goes away under Direction A).

6. The asymmetry between `addConstant("null", &Value::vNull, ...)` and
   the `true`/`false` registrations (which build via `mkBool` on a stack
   Value and have `addConstant` value-copy into a fresh heap Value) is
   eliminated.

7. Direction B requires invasive evaluator rewrites and conflicts with
   the catalog's broader direction on outparam-style entry points.

The catalog should record the decision and remove the four "This is _not_ a
singleton" comments along with the canonicals themselves; replace with
documentation on the factory functions explaining that fresh values are
allocated each call.

## Open questions

- `prim_functionArgs` calls `state.getBool(i.def)` for each formal
  parameter and inserts the resulting `&Value::vTrue` / `&Value::vFalse`
  into a returned attrset. Functions with many optional parameters
  (e.g. `nixpkgs.mkDerivation`) call this on every `builtins.functionArgs`
  query. Direction A would allocate one `Value` per formal. Should the
  catalog measure the allocation cost on a representative
  `nixpkgs`-scale eval before committing? In practice
  `builtins.functionArgs` is rare during normal builds (mostly used by
  meta-tools), so the cost is likely sub-ms over a full evaluation.

- `_combineChannels` (in `nix-env`) currently embeds the canonical
  `&Value::vEmptyList` into the channel-loading attrset. If a future
  consumer mutates that attr's payload (it should not), Direction A
  guarantees safety; current code accidentally has no protection beyond
  "no one writes through the canonical pointer". This is a latent
  reliability win for Direction A but not a forcing concern.

- The catalog's broader work on `Value` representation (`ValueStorage`
  bit-packing, the discriminator scheme) is unaffected by either
  direction. The recommendation here is independent of those changes.
