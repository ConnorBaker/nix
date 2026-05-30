# Compositional trace-DAG: what the refactor would capture, and why it's tractable

2026-05-30. Answers: what would the build-layer-CA-lifted-to-evaluation approach
actually capture? "Hash every value/env" is intractable (hundreds of millions);
so what is the boundary, and why does it not blow up? Grounded in code.

## Why "hash every value" is the wrong frame — and what the build layer actually does

The build layer does NOT hash every intermediate value. `closures.gnome` forces
~22.9 M thunks but instantiates only ~6,419 derivations (measured). The CA model
hashes **~6 K derivations, not 22.9 M values** — a ratio of ~3,500:1. The trick
is not "content-address everything"; it is **content-address only at the few
boundaries where a stable identity already exists, and reference everything else
through those boundaries.**

Crucially, the current eval-trace ALREADY selects a sparse boundary set: 10,397
attr-path traces for 22.9 M thunks (~2,200:1). **The problem was never
over-selection — both schemes pick ~thousands of nodes. The problem is that each
selected node FLATTENS its subtree (unions all descendant deps into itself)
instead of EDGING to other selected nodes.** So the refactor is not "add more
nodes"; it is "make the existing-density nodes reference each other by hash
instead of inlining."

## What must be captured at a node (the trace-hash recurrence)

A node's trace hash should be:

    trace_hash(N) = H( N's OWN direct world-observations,
                       sorted edges to child nodes' trace_hashes )

versus today:

    trace_hash(N) = H( flattened transitive union of ALL observations under N )

The two ingredients, and what each already gives us:

1. **Own direct world-observations** — the LEAF deps a node makes *itself*, not
   its children's. These are the FileBytes / StorePathAvailability / EnvLookup /
   StructuredProjection records (~95 % of current deps). They are ALREADY
   content-stable (a file's hash, a store path's validity) — this is the half
   eval-trace gets right. The epoch-range mechanism
   (`recordThunkDeps(v, epochStart)` → `epochMap[&v] = DepRange{start,end}`,
   memo-replay-store.hh:93-104) ALREADY delimits "this thunk's own contribution"
   as the range `[start,end)`. **Flattening happens because
   `replayMemoizedRange` (dep-recording-context.hh:307) copies that range UP into
   the parent. The edge alternative is: hash the range as the child's own-deps,
   and record the child's trace_hash as ONE dep in the parent.**

2. **Edges to child nodes** — references to the trace_hashes of sub-results the
   node depends on. This is the half that's missing. The edge dep kinds exist
   (`TraceValueContext`/`ParentSlot`) and the recursive-verify-with-memoization
   machinery exists (`resolveTraceContextHash`, verifier.cc:243-278). What's
   missing is (a) the recorder emitting edges instead of flattening, and (b) a
   stable identity for the child to edge TO.

## The hard part, stated precisely: child identity for computation-produced values

A node can only edge to a child if the child has a **content-addressed, stable
identity** (survives GC, survives process restart, equal iff the child's result
is equal). Three tiers of candidate boundary, by how stable their identity is:

### Tier 1 — already content-addressed (cheap, do first)
- **Derivations.** `drvPath = hashDerivationModulo(inputs)` is computed at
  primops.cc:1994 and ALREADY recorded as a `StorePathAvailability` dep
  (primops.cc:2003-2011). The derivation's identity is the strongest possible:
  it already transitively hashes its inputs (the build layer's whole point). A
  node that depends on a derivation should edge to `drvPath` and STOP — never
  flatten the derivation's input closure. **This alone would cut most of the
  607×, because the shared closure is overwhelmingly derivations (stdenv etc.).**
  This is exactly Lever 2, but reframed: not "make derivations cacheable trace
  nodes" (blocked — they're not attr-path-addressable), but "when a node
  observes a derivation, record an EDGE to its already-existing drvPath identity
  instead of inlining its closure." That is a RECORDING change, not a new
  identity scheme.

### Tier 2 — structurally identifiable (the NixBinding precedent already does this)
- **Function results `f(args)` and file/imports.** Identity =
  `H(f-structure, arg-identities)`. The f-structure half ALREADY exists:
  `computeNixScopeHash` (nix-binding.cc:111) fingerprints a lambda/let/with/call
  expression via `showForHash` + scope shape, cross-session-stable (it sorts by
  string name to survive Symbol-ID churn, nix-binding.cc:130-158). So eval-trace
  ALREADY computes "which function/binding this is" content-addressably.
- The missing half is **arg-identities**, and this is where naive recursion would
  explode into "hash every value." The resolution (and why it does NOT explode):
  **you do not hash the arg VALUES; you reference the args by THEIR node
  identities, which only exist at Tier-1/Tier-3 boundaries.** Args that are
  derivations → Tier 1 edge. Args that are themselves attr-path nodes → Tier 3
  edge. Args that are plain scalars/small values → inline their value (cheap).
  Args that are arbitrary unforced thunks → the node simply doesn't get an edge
  identity and falls back to FLATTENING for that subtree (today's behavior, as
  the safe default). **The DAG is sparse precisely because most values are NOT
  boundary-eligible; only the recurring, identity-bearing ones (derivations,
  imported files, attr-path nodes) become edges, and those are the ~thousands,
  not the millions.**

### Tier 3 — positionally identifiable (what we have today)
- **Attr-path nodes.** Identity = the attr-path from root (the current
  `AttrPathId`-keyed traces). Stable within a session/eval, content-stable across
  commits when the path is unchanged. These stay as nodes; the change is they
  edge to Tier-1/Tier-2 children instead of flattening them.

## Why this is tractable — the floor that stops the recursion

The recursion `trace_hash(N) = H(own deps, child trace_hashes)` does NOT visit
every value because **a child only becomes an edge if it is boundary-eligible
(Tier 1/2/3); otherwise its deps flatten into N (the current behavior, kept as
the fallback).** So:
- Boundary-eligible nodes: ~thousands (derivations + imported files + attr-path
  nodes), each hashed ONCE and referenced N times — the build-layer amortization.
- Everything else (the 22.9 M − ~thousands intermediate values): never
  individually hashed; their world-observations flatten into the nearest
  enclosing boundary node, exactly as today, but that flattening is now BOUNDED
  by the next boundary down (it stops at the first derivation/import/attr-path
  child) instead of spanning the whole transitive closure.

The win: a shared derivation's closure is captured once (in its own drvPath
identity) and referenced by an edge from each of the 607 packages, instead of
flattened 607×. The flattening that remains is only the THIN layer between a node
and its nearest boundary children — small and non-recurring.

## What the refactor touches (honest blast radius)

1. **Recording (the core change).** At a force boundary, decide: is the forced
   sub-result boundary-eligible (a derivation? an import? an attr-path child)? If
   yes, record an EDGE (its identity) as a single dep and DO NOT replay its
   range up. If no, flatten as today. Touch points:
   `replayMemoizedRange`/`SiblingReplayCaptureScope` (dep-recording-context.hh,
   context.cc), the derivation primop (primops.cc:2003, already records the
   drvPath — change consumers to treat it as a boundary, not just a GC-existence
   check), `evalFile`/`ExprParseFile` (eval.cc:1372+, the import boundary).
2. **Trace-hash recurrence.** `feedDep` currently writes a positional
   `dep.ordinal` over a flat globally-sorted vector (hash.hh:61-72, hash.cc:18).
   An edge dep already participates as a `TraceValueContext` whose value IS the
   child's trace_hash, so edges compose TODAY — the change is mostly that there
   are far more edges and far fewer leaf deps. Likely no preimage redefinition
   for the edge path; the leaf path is unchanged. (Needs verification — the
   `dep.ordinal` global-position binding may still need attention for
   determinism when edges replace large leaf runs.)
3. **Verification.** `resolveTraceContextHash` already recurses + memoizes
   (verifier.cc:243-278; `verifiedTraceIds` early-out verifier.cc:1786). With
   edges as the primary representation it does MORE recursion and FAR less
   per-node leaf-walking — this is where the hot win comes from (verify stdenv's
   trace ONCE, reference its verdict from 607 packages, vs re-walk its closure
   607×).
4. **Soundness — the genuine research risk.** Unlike a build, an eval node can
   observe a CHILD'S SHAPE (its keys, its `attrNames`, negative membership), not
   just its value. So an edge `N → child` must capture not just "child's value
   unchanged" but "the FACET of child that N observed is unchanged." This is
   EXACTLY the keyset-provenance / cross-trace-escape work
   (`store/keyset-escape.cc`, `plans/keyset-*.md`): an edge that carries a
   shape/keyset observation must re-verify that facet. The build layer has no
   analogue because builds observe only output paths, never "shape." **This is
   the prerequisite and the reason this is eval-specific, not a mechanical port
   of `hashDerivationModulo`.**

## Sequenced plan (each independently shippable, gated by the correctness + keyset-escape suite)

1. **Tier-1 derivation edges (highest payoff, smallest soundness risk).** When a
   node observes a derivation, edge to its `drvPath` (already computed) and stop
   flattening its closure. Derivations have NO shape-observation hazard (you
   observe `outPath`/`drvPath`, value-like) — so the soundness risk is minimal
   and the keyset machinery isn't even needed. Measure the 607× reduction.
2. **Tier-2 import/file edges.** Edge to (resolved file path + arg-identity
   fingerprint via the existing `computeNixScopeHash`). Soundness: a file's
   result can be shape-observed → needs the keyset-escape facet rule.
3. **Tier-2 function-result edges** (the general case), only if 1–2 don't capture
   enough. Highest complexity (arg-identity recursion + shape facets).

## Honest caveats
- The drvPath-edge idea (step 1) is the same lever the Lever-2 spike hit a wall
  on — BUT the wall was "make a derivation a cacheable trace NODE" (blocked: not
  attr-path-addressable). This is different: keep the existing attr-path nodes,
  and have them record an EDGE to the derivation's drvPath as a dep, replacing
  the flattened closure. The drvPath identity already exists; no new trace-node
  identity scheme is needed for step 1. **This distinction is the unlock and
  should be the first thing prototyped/verified.**
- Whether replacing a flattened closure with a drvPath edge is SOUND requires
  that everything the node observed about the derivation is captured by the
  drvPath (i.e. the node didn't peek at the derivation's `passthru`/`meta`/non-
  output attrs). That is the facet-mask question again — must verify a node's
  observations of a derivation are output-only before edging. If nodes commonly
  read `.meta`/`.passthru`, step 1's soundness needs the facet rule too.
- All measurements n=1, python3Packages/closures.gnome. The ~3,500:1
  boundary:thunk ratio and drvPath-already-recorded are verified facts; the
  projected 607× reduction is an estimate pending prototype.
- Open: the `dep.ordinal` determinism under edge-heavy keysets (blocker-2 above)
  needs a concrete check before claiming "no preimage redefinition."
