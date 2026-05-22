# V4 - verify E8 vTrue/vFalse perf claim

E8 (`doc/inventory/review/E8-vtrue-vfalse-invariant.md`) recommends Direction A
(rename `Value::vTrue/vFalse/vNull/vEmptyList` to factory functions and migrate
the nine address-of sites to `allocValue() + mk*` patterns). The
recommendation rests on the claim under "Direction B: canonicalise":

> `eqValues` could shortcut on `&v1 == &v2 == &Value::vTrue`, but
> `eqValues` *already* short-circuits on `&v1 == &v2` for any reason,
> and the per-type unwrap is already O(1) for Boolean/null. There is no
> measurable win.

This document verifies that performance claim against the source.

## eqValues short-circuit position

`EvalState::eqValues` in `src/libexpr/eval.cc` opens with:

```
bool EvalState::eqValues(Value & v1, Value & v2, const PosIdx pos, std::string_view errorCtx)
{
    auto _level = addCallDepth(pos);

    forceValue(v1, pos);
    forceValue(v2, pos);

    /* !!! Hack to support some old broken code that relies on pointer
       equality tests between sets.  (Specifically, builderDefs calls
       uniqList on a list of sets.)  Will remove this eventually. */
    if (&v1 == &v2)
        return true;
    ...
```

Two semantic notes:

- The pointer-equality short-circuit is **not** the first check. Both
  arguments are forced first via `forceValue`. For the four canonical
  instances, `forceValue` is cheap: `forceValue` (defined inline in
  `src/libexpr/include/nix/expr/eval-inline.hh`) tests `v.isThunk() ||
  v.isApp() || v.isFailed()` and bails immediately for already-final
  values. The four canonicals are constructed in
  `src/libexpr/value.cc` via IIFEs that call `mkBool`/`mkNull`/`mkList`
  directly, so they never appear as thunks. The forceValue calls on
  canonicals are effectively two `bool` checks.

- The comment frames the short-circuit as a hack for legacy
  builderDefs code, **not** as a fast path for canonical singletons.
  E8 paraphrases this correctly.

What work the short-circuit skips, when it fires:

- The float/int compatibility checks (two `type()` reads).
- The type-mismatch check.
- The type switch and the per-type unwrap.

For `nBool` and `nNull`, the per-type unwrap is `return
v1.boolean() == v2.boolean();` and `return true;` respectively
- both O(1). The total skipped work is ~5-10 CPU instructions. This is
not a meaningful saving in itself.

## Call-site classification

There are exactly four direct call sites of `eqValues` in `src/`:

| File | Caller | Frequency | Args reach via | Canonical can flow in? |
|------|--------|-----------|----------------|------------------------|
| `src/libexpr/eval.cc` (`ExprOpEq::eval`) | Nix `==` operator | Hot (per `==` in source) | Two stack-local `Value` copies via `e->eval(state, env, vN)` | **No** - see below |
| `src/libexpr/eval.cc` (`ExprOpNEq::eval`) | Nix `!=` operator | Hot (per `!=` in source) | Same as above | **No** |
| `src/libexpr/primops.cc` (`prim_lessThan` list-element fallback) | Recursive list compare in `<`/`<=`/`>`/`>=` | Cold (only fires when ordering nested lists) | `Value *` from listView | Possible (but null is not orderable, so list-of-null compares throw before reaching here) |
| `src/libexpr/primops.cc` (`prim_elem`) | `builtins.elem` | Mixed | `Value *` from `args[0]` (preserved through `maybeThunk`) and listView | Only `&Value::vNull` from `prim_match`/`prim_split` results, plausibly |

Why the user-level `==` operator can never see the canonical pointer:

`ExprOpEq::eval` (`src/libexpr/eval.cc`) is:

```
void ExprOpEq::eval(EvalState & state, Env & env, Value & v)
{
    Value v1;
    e1->eval(state, env, v1);
    Value v2;
    e2->eval(state, env, v2);
    v.mkBool(state.eqValues(v1, v2, pos, "..."));
}
```

`v1` and `v2` are stack-allocated `Value` locals. `e->eval(state, env,
vN)` writes the result *into* the slot `vN`. For the variable case
`ExprVar::eval` ends with `v = *v2;` - it copies the looked-up
Value out of the env into the caller's slot. So even when `e1`
evaluates the user-visible identifier `null` (whose env binding
literally is `&Value::vNull`, see below), the resulting `v1` is a
fresh stack copy of the canonical's payload, not the canonical pointer
itself. `&v1 == &v2` cannot fire from canonicals here.

For `prim_elem` and the recursive list-compare in `prim_lessThan`,
arguments arrive via `maybeThunk`, which for `ExprVar` returns the
env-stored `Value*` directly without copying. So `args[0]` for
`builtins.elem null xs` *is* `&Value::vNull` - the canonical pointer
*can* survive into `eqValues` here.

## Frequency of canonicals in eqValues

Empirical frequency cannot be measured in-session without building
both Direction A and Direction B variants, instrumenting `eqValues`,
and running a representative eval (`nixpkgs#hello.outPath` or
similar). I have not done that.

What I can establish from source:

- `==` and `!=` at the Nix language level are the dominant
  `eqValues` callers (every `if x == "y"`, `if foo == null` in
  nixpkgs invokes them). For these, the short-circuit cannot fire
  from canonicals at all (see above). It can still fire from the
  legacy "same set passed twice" path, but that is unrelated to E8's
  question.

- For `prim_elem`, the canonical can only arrive via `null` (the
  asymmetry is explained next). `builtins.elem null xs` is rare in
  nixpkgs: a grep over a representative tree would be needed to
  quantify, but the idiomatic check is `x == null`, not `elem null
  [...]`.

- `prim_lessThan` only recurses into `eqValues` for list-element
  ordering, which is rare; `null` is not orderable, so a list of
  nulls would error before reaching `eqValues`.

The asymmetry between `null` and `true`/`false`:

- `addConstant("null", &Value::vNull, ...)` (in
  `createBaseEnv`/`primops.cc`) calls the `Value *` overload of
  `addConstant`, which stores the pointer **as-is** in the base env.
  So a Nix-level `null` token, when looked up via `ExprVar::maybeThunk`,
  yields the canonical `&Value::vNull`.

- `v.mkBool(true); addConstant("true", v, ...)` calls the `Value &`
  overload, which copies via `Value * v2 = allocValue(); *v2 = v;`
  before installing. So a Nix-level `true` token yields a
  per-EvalState heap copy, **not** `&Value::vTrue`.

This means the only canonical that can flow into `eqValues` from
user-visible identifiers is `vNull`, and only via `prim_elem` /
recursive list compare - neither of which is a hot path.

The remaining canonicals (`vTrue`/`vFalse`/`vEmptyList`) reach
`eqValues` only when the user constructs an attrset/list containing
them via `prim_tryEval`, `prim_match`, `prim_split`,
`prim_functionArgs`, or the `_combineChannels` path, and then
compares an element of that structure for equality with a value the
user gets back through one of those same primops. That intersection
is empirically negligible.

I cannot put a number on the firing rate. The structural argument is
that the rate is small because the asymmetry described above already
strips canonicals out of the most common comparison path (`==`/`!=`).

## Cost of Direction A's allocation

Direction A migrates the nine `&Value::vTrue/vFalse/vNull/vEmptyList`
sites to `state.allocValue()` + `mk*` patterns. Per-call cost of
`allocValue` from `src/libexpr/include/nix/expr/eval-inline.hh`:

```
Value * EvalMemory::allocValue()
{
    static thread_local std::shared_ptr<void *> valueAllocCache{...};
    if (!*valueAllocCache) {
        *valueAllocCache = GC_malloc_many(sizeof(Value));
        ...
    }
    void * p = *valueAllocCache;
    *valueAllocCache = GC_NEXT(p);
    GC_NEXT(p) = nullptr;
    stats.nrValues++;
    return (Value *) p;
}
```

Hot path: thread-local cache pop (one indirect load + one pointer
swap + one stats increment + return). When the cache is empty,
`GC_malloc_many` refills in batch. Per-call cost is comparable to a
malloc fast-path.

Per-occurrence allocation cost on each migrated site:

- `prim_tryEval` (success/value attrs): one allocation per `tryEval`
  call where the success path hits, two on the failure path.
  `tryEval` is invoked rarely.
- `prim_match` no-match-group case: one allocation per unmatched
  capture group per regex match. Negligible aggregate.
- `prim_split` no-match-group case: same as above.
- `prim_functionArgs` (`getBool` per formal): one allocation per
  formal of every function passed to `builtins.functionArgs`. Rare
  in build paths; meta-tooling only.
- `_combineChannels` empty-list slot: once per channel reload.
- `addConstant("null", ...)`: once per EvalState. Trivial.
- `ExprList::maybeThunk` empty-list literal `[]`: once per
  `ExprList::maybeThunk` call on an empty list. Currently saves an
  allocation by returning `&Value::vEmptyList` directly. Direction A
  forces a fresh allocation here, which is the most common
  hot-path-relevant cost. nixpkgs evaluation does construct empty
  lists, but the cost per-occurrence is the same single-cache-pop
  that already dominates Nix's value graph.

I cannot quantify the aggregate cost without building and running.
None of the sites is in a per-eval-step or per-attribute hot path
except `ExprList::maybeThunk` for the literal `[]`, which is
called per-occurrence of `[]` in source, not per evaluation step.

## Decision

**Inconclusive on the perf claim - but the structural argument
strongly favours Direction A regardless of perf.**

The verification produced four findings beyond E8's stated reasoning:

1. The pointer-equality short-circuit in `eqValues` is gated behind
   two `forceValue` calls (cheap on canonicals, but not zero), and
   skips only ~5-10 instructions of work. Even when it fires, the
   per-call benefit is sub-microsecond.

2. The user-level `==`/`!=` operators cannot benefit from canonical
   pointer-equality at all, because `ExprVar::eval` copies values
   into stack locals before passing them to `eqValues`. This is the
   highest-frequency `eqValues` caller and it is *immune* to
   canonicalisation benefits.

3. Among the four canonicals, only `vNull` actually propagates as
   the canonical pointer into env bindings; `vTrue`/`vFalse` are
   already heap-copied at `addConstant` time. So Direction B's
   "fast path" is, at best, narrower than its framing implies.

4. `allocValue` is fast (thread-local Boehm batch cache pop), and
   the nine sites Direction A migrates are not in per-eval-step
   hot paths - they are in primop-result-construction paths.

Findings 1-3 reduce Direction B's hypothetical perf benefit further
than E8's "no measurable win" assertion, but I have not run an A/B
benchmark to *prove* the absence of a measurable win. E8's
recommendation should be downgraded from "no measurable win"
(unsupported numeric claim) to "structural reasoning suggests
negligible win, perf unmeasured":

- Strongest pro-A argument: the `==`/`!=` operators - the
  highest-frequency `eqValues` callers - cannot benefit from
  canonical pointer-equality at all. Direction B's only meaningful
  hot-path savings would be in `prim_elem` / `prim_lessThan` list
  recursion against the `vNull` canonical specifically, and only
  when the user passes `null` (which `addConstant` preserves as the
  canonical pointer) into those primops. This is a narrow scenario
  and unlikely to dominate any real eval.

- E8's overall recommendation (Direction A) stands on the structural
  arguments (de-facto-singleton consumers, asymmetry with
  `true`/`false`, C-API leakage of static-storage pointers, parallel
  catalog direction on outparams). The perf claim is not load-bearing.

## Implied catalog edits

Keep E8's Direction A recommendation. Edit the perf paragraph to
remove the unsupported "no measurable win" framing and replace with
structural reasoning. Concretely, in the "Direction B: canonicalise"
> "Costs" subsection, replace the bullet:

> `eqValues` could shortcut on `&v1 == &v2 == &Value::vTrue`, but
> `eqValues` *already* short-circuits on `&v1 == &v2` for any reason,
> and the per-type unwrap is already O(1) for Boolean/null. There is no
> measurable win.

with something like:

> `eqValues` could shortcut on `&v1 == &v2 == &Value::vTrue`, but the
> short-circuit lies behind two `forceValue` calls and skips only
> O(1) per-type unwrap work. Furthermore, the highest-frequency
> `eqValues` callers (`ExprOpEq`/`ExprOpNEq`) copy their arguments
> into stack-local `Value` slots before comparing, so canonical
> pointer-equality cannot fire from them at all - regardless of
> Direction B. Hot-path benefit is therefore confined to a narrow
> `prim_elem`/`prim_lessThan` scenario against `vNull`. Aggregate
> impact is unmeasured but structurally negligible; a real perf
> comparison would require A/B builds against a representative eval.

This preserves E8's directional conclusion (Direction A) while
correcting the unsupported numeric claim that the verification
charter required.

The "Benefits" subsection of Direction B should similarly downgrade

> Pointer identity becomes a documented fast path for `eqValues`. In
> practice this saves 1-2 ns per Boolean/null comparison, but
> Boolean/null comparisons are rare in evaluation hot paths.

to remove the fabricated "1-2 ns" figure (no source backing) and
restate as a structural claim only.

## Open questions

- An A/B benchmark of `nixpkgs#hello.outPath` (or a similar
  representative eval) under both Direction A and Direction B variants
  would resolve the perf question definitively. This requires patches
  for both directions and a stable benchmark harness. Out of scope
  for in-session verification.

- The cost of the migrated `ExprList::maybeThunk` empty-list site
  (which currently returns `&Value::vEmptyList` and would, under
  Direction A, allocate fresh) is the most plausible Direction-A
  regression. A targeted micro-benchmark on heavy `[]`-using code
  (e.g. nixpkgs lib functions that thread empty lists) would bound
  it. Not done.

- Whether `prim_functionArgs`-with-defaults is genuinely meta-only
  (E8's open question) is also unmeasured. Direction A would shift
  one allocation per formal per call. nixpkgs `mkDerivation` is
  rarely introspected at scale, but flake-check-style tools may
  hit this.
