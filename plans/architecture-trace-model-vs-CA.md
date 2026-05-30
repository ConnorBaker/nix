# The bigger pattern: eval-trace flattens transitive deps; CA builds reference by hash

2026-05-30. Architectural analysis answering: is the way we trace/record/handle
deps suboptimal? How does it differ from Nix's content-addressing and from
build-layer materialization/short-circuiting? Every claim is cited.

## The core finding (verified)

**The ~607× shared-closure dep duplication is not a bug to tune — it is the
direct, necessary consequence of eval-trace's data model: each trace stores its
own FLATTENED TRANSITIVE dep closure as a value, instead of referencing child
traces by identity (edges).** Both cold and hot cost scale with the *flattened*
total (36.5 M), not the *distinct* graph (60 K), because the model has no edge to
amortize over.

### Mechanism, cited

1. **Deps propagate by value-copy UNION up the eval tree.** When a forced thunk
   is replayed into its enclosing recording scope,
   `DepRecordingContext::replayMemoizedRange` (dep-recording-context.hh:289-312)
   does `scope->ownDeps.insert(end, range.begin, range.end)` — it **copies the
   child's entire dep range into the parent's `ownDeps`**. Each ancestor scope
   re-accumulates every descendant's deps. A leaf file read deep in stdenv lands
   in the flattened set of *every* trace above it. (lines 307-310.)

2. **A trace stores that flattened set, not edges.** Verified by decoding 2,000
   trace `keys_blob`s from the cold DB (dep-kind histogram):
   - StructProj 48.4 %, StorePathAvail 34.9 %, FileBytes 7.0 %, DerivedStorePath
     4.8 %, ImplicitStruct 4.3 % — **~95 % are leaf observations** (file content,
     store-path existence, structured projections).
   - **TraceParentSlot 0.0 % (~1 per trace); TraceValueContext 0 %.** The
     cross-trace EDGE dep kinds exist (`Dep::makeValueContext`/`makeParentSlot`)
     but are **essentially unused** on this workload — a trace carries ~1 parent
     backpointer and otherwise its full transitive leaf closure.
   - avg **3,376 deps/trace** — a transitive closure (a python package's own
     direct reads are a handful; 3,376 = stdenv+toolchain+python re-accumulated).

3. **Verification re-walks the flattened set per trace.** `runPass1`
   (verifier.cc:565-648) is a flat `for` over `fullDeps` with a per-dep
   `resolveCurrentDepHash`. The L1 cache (`currentDepHashes_`) dedups the
   *recompute* across traces, but the *walk* (`verify.depsChecked` = 36.9 M) and
   the *storage/load* (343 MB values_blob, deserialized per trace) are not
   amortized — there is no "this child trace already verified, skip its subtree."

### Why content-addressing does NOT save us here

`DepKeySets` is content-addressed and shared (sqlite-trace-storage.cc:526-538):
traces with byte-identical key-sets share one row. But **0 sharing occurs**
(10,397 traces → 10,397 distinct DepKeySets) because the unit of content-address
is the WHOLE flattened closure, and two packages' closures overlap massively but
are never byte-identical. CA at the wrong granularity (whole closure) is useless;
CA at the right granularity (the shared sub-closure) is what's missing.

## The contrast with Nix's build-layer CA (cited)

This is the crux of your question. The build layer solved exactly this problem,
structurally, decades ago — and eval-trace does the opposite.

| | Build layer (derivations) | eval-trace |
|---|---|---|
| A node references its inputs by… | **HASH / edge** — `Derivation::inputDrvs` is a `DerivedPathMap<set<OutputName>>` keyed by input **drvPath** (derivations.hh:336). A drv names its input *derivations*, not their transitive input files. | **VALUE / flattening** — a trace's `keys_blob` is the inlined transitive leaf closure (above). Edges (`TraceValueContext`) exist but are ~unused. |
| Shared sub-graph cost | **Hashed once, referenced N times.** `hashDerivationModulo` (derivations.cc:19+) recursively replaces each input path with the input drv's hash, and is **memoized in `drvHashes`** (derivations.hh:591-593, the `cvisit`/`insert_or_assign` at derivations.cc:10-16). Shared stdenv is hashed once; every consumer references that one hash. | **Re-listed and re-hashed N times.** No memoized per-subgraph identity; the shared closure is copied into each ancestor and hashed in each trace (the 607×). |
| Short-circuit on reuse | **Floating-CA "resolved" short-circuit:** if an input drv's output is already known/built, the consumer references its output hash and the input's *own* inputs are never reconsidered. The graph is cut at the input boundary. | **None.** A trace miss re-evaluates and re-verifies its entire flattened subtree; a hit re-walks all 3,376 deps. There is no "child subtree unchanged → skip it" because the child isn't a referenced identity, it's inlined bytes. |
| Identity of a node | `outPath` / drv hash = a stable, composable content address that OTHER drvs build on. | A trace's `trace_hash` exists but **nothing builds on it** — it is not used as an input identity by other traces (TraceValueContext, the only mechanism, is unused). The hash is a lookup key, not a compositional edge. |

The decisive structural difference: **in the build layer the dependency graph is
a DAG of hash-identified nodes, and the hash of a node is a function of its
inputs' hashes (recursively, memoized). In eval-trace the dependency graph is
flattened into a per-node transitive leaf set, so there is no node-to-node hash
edge to compose or short-circuit over.**

## How this differs from materialization / build short-circuiting

The build layer's short-circuit (the thing that makes `nix build` skip an
already-built subtree) has two ingredients eval-trace's hot path lacks:

1. **Input-boundary cut.** A built output is referenced by its store path; the
   builder never looks inside it or at how it was made. eval-trace's verify walks
   *into* every transitive dep every time (no boundary — the deps ARE the
   transitive leaves, flattened, so there's nothing to cut at).

2. **Compositional identity.** `drv hash = f(inputs' hashes)` means an unchanged
   subtree has an unchanged hash and the parent's hash short-circuits the whole
   subtree in one compare. eval-trace's `trace_hash` is `f(flattened leaf set)`,
   not `f(child trace hashes)`, so a parent can't be validated by comparing child
   *trace* hashes — it must re-examine the leaves.

Eval-trace's `materializeResult` (cache/trace-session.cc) DOES short-circuit at
the attrset-child boundary on warm hits (a verified child trace serves its cached
value without re-eval) — that is the per-leaf laziness the OR-3 notes describe.
But that boundary is the **output attr-path tree**, not the **dependency DAG**.
It cuts re-EVALUATION at attrset children; it does NOT cut re-VERIFICATION of the
shared dependency closure, because verification consumes the flattened dep set,
not a graph of child-trace identities.

## So: is the model suboptimal? Yes — and the fix is the build layer's model

The bigger pattern we're missing: **eval-trace should reference dependency
sub-results by hash-identity (edges), the way derivations reference input drvs —
not inline the transitive closure into every node.** Concretely, the missing
primitive is a *compositional trace hash*: `trace_hash(node) = f(node's own
direct deps, child nodes' trace_hashes)`, with each subgraph hashed and verified
ONCE per session and referenced by the many nodes above it. That is exactly what
`hashDerivationModulo` + `drvHashes` memoization is for derivations.

This reframes the earlier levers:
- It is the true form of **L-A** (the dep-fragment idea), but the right fragment
  boundary is the **child trace / sub-result**, not an arbitrary dep subset —
  and the right mechanism is the already-present-but-unused **TraceValueContext
  edge**, made the primary representation instead of flattening.
- It dissolves **L-B/L-C/L-D/L-E** as the second-order tweaks they are: they
  optimize the cost of processing 36.5 M flattened deps; the architectural fix
  removes the 36.5 M (→ ~60 K distinct + edges).

### CORRECTION (verified): the edge VERIFY machinery already exists and is wired

Initial framing ("edges are missing") was wrong — checked the verify path:
**`resolveTraceContextHash` (verifier.cc:243-278) already implements
build-layer-style edge recursion with memoization**, for the
`TraceValueContext`/`ParentSlot` dep kinds:
- it RECURSIVELY verifies the referenced (parent/child) trace by edge
  (`verifyTrace(ea, parentRow->traceId, …)`, verifier.cc:267),
- MEMOIZES the result in `session.traceContextMemo` (pathId+nodeStamp keyed,
  verifier.cc:255-276) AND `session.verifiedTraceIds` (the early-out at
  verifier.cc:1786: `if (session.verifiedTraceIds.count(traceId)) return`),
- with CYCLE DETECTION via `session.inProgressTraceIds`.

This is exactly `hashDerivationModulo` + `drvHashes`'s shape: verify a referenced
node once, short-circuit on revisit, memoized per session. **The machinery to
NOT re-walk a shared subtree already exists.**

So the problem is NOT missing machinery. It is that **the recorder emits these
edges almost never** (TraceParentSlot ~1/trace, TraceValueContext 0 on this
workload) and **flattens instead** — `replayMemoizedRange` copies the child's
full dep range into the parent (dep-recording-context.hh:307). The shared
closure becomes inlined leaf deps rather than a single edge to a shared child
trace that `resolveTraceContextHash` could verify-once.

### The real blockers (revised, honest)
1. **It's a RECORDING-POLICY change, not new verify machinery.** The lever is:
   when forcing a child whose result is itself a recordable trace, record an
   EDGE (`makeValueContext`/`makeParentSlot` to the child's trace hash) instead
   of flattening the child's deps up via `replayMemoizedRange`. The verify side
   (`resolveTraceContextHash`) already handles edges with memoization. Entry
   point: the `replayMemoizedRange`/`SiblingReplayCaptureScope` recording path in
   `context.cc`/`dep-recording-context.hh` and the trace boundary in
   `trace-session.cc`.
2. **The trace hash is still positional/flat for the LEAF deps it does keep**
   (`dep.ordinal` over globally-sorted vector, hash.hh:61-72 / hash.cc:18).
   Edges already compose (a `TraceValueContext` dep IS the child's trace hash,
   resolved recursively), so this is not a blocker for the edge path — but it
   means a node's hash mixes "my flat leaf deps" + "child trace-hash edges",
   which is fine and already how `TraceValueContext` deps participate.
3. **WHY does the recorder flatten instead of emitting edges?** This is the key
   question to answer before building. Hypotheses to check: (a) edges are only
   emitted for attrset-CHILD relationships (parentSlot), not for arbitrary forced
   sub-results (function results, `let` bindings, `import`s) — so most shared
   closure (reached through function application, not attr access) has no
   trace identity to point at; (b) sub-results below the attr-path tree aren't
   recorded as traces at all (only attr-path nodes are — same root limitation as
   Lever 2), so there's no child trace hash to reference. If (b), this collapses
   into the same "only attr-path-addressable values get traces" constraint that
   blocked Lever 2 — meaning the shared closure (function-produced, not
   attr-addressable) fundamentally can't be edge-referenced without the
   content-addressed-trace-node work. **This is the decisive thing to verify
   next.**
4. Cross-trace soundness: edges that carry shape/keyset observations need the
   keyset-escape soundness floor (already tested in
   `store/keyset-escape.cc`).

## THE UNIFYING ROOT CAUSE (verified) — one constraint explains everything

Blocker-3 resolved by checking `makeChild` callers (materialize.cc:414/486,
trace-session.cc:216/329/777): **a `TracedExpr` — and therefore a recordable,
hash-identified trace — is created ONLY for the root and for attrset/list
CHILDREN during materialization. I.e. only values at a stable ATTR-PATH from the
evaluation root get a trace identity.**

The shared closure (stdenv, python interpreter, toolchain) is reached by
**function application / `let` / `import` inside a package's evaluation** — it is
NOT an attr-path child of anything. So it has **no `TracedExpr`, no trace, no
hash to reference.** That is why the recorder flattens it (no identity to edge
to) and why `resolveTraceContextHash`'s edge machinery sits unused (nothing to
point at).

**This single constraint — "only attr-path-addressable values get trace
identities; computation-produced values do not" — explains the whole arc:**
- the **607× flattening** (shared closure has no identity → inlined into every
  ancestor),
- **Lever 2's blocker** (derivations aren't attr-path-addressable → can't be
  trace nodes),
- the **unused edge machinery** (TraceValueContext ~0: nothing to reference),
- why cold AND hot scale with flattened-total not distinct-graph.

It is the SAME wall in three disguises. The build layer does not have this wall
because a derivation's identity is **content-addressed by its inputs**, available
for ANY drv regardless of how it was reached in the expression — there is no
"addressable position" requirement. Eval-trace ties identity to *syntactic
position in the output tree*; the build layer ties it to *content of inputs*.
That is the architectural gap, stated exactly.

## Recommendation
This is the genuinely promising direction the whole arc has been circling — and
it is the same conclusion the redesign-plan's "compact proof/result authority"
and "BSalC" notes gesture at, now pinned to a concrete structural diagnosis:
**move from flattened-transitive-dep-sets to a hash-edged trace DAG (the build
layer's CA model, lifted to evaluation).** It is an RFC-scale change with the
three blockers above, gated by the standing correctness + keyset-escape soundness
suite. But it is the only lever that attacks the root (the 607×) rather than its
symptoms, and unlike the derivation-boundary Lever 2 (blocked by attr-path-shaped
`TracedExpr` identity), this one has a precedent to copy almost verbatim:
`hashDerivationModulo` + `drvHashes`.

Caveats: the 607× / dep-kind histogram / zero-edge-usage are measured on
`python3Packages` (n=1, sampled 2,000 traces for the histogram). The build-layer
mechanism citations are verified. Whether a compositional trace hash is
soundly definable for *evaluation* (where, unlike builds, a node's result can
depend on observing the SHAPE/keys of a child, not just its value) is the open
research question — the keyset-provenance / cross-trace-escape work is precisely
about that, and is the soundness prerequisite for this model.
