# Content-addressed trace identity for non-attr-path values — the proposal

2026-05-30. This is the design behind the wall every lever keeps hitting
(Lever 2, Lever 5, the Tier-1 recorder spike). It answers three questions
directly: (1) what exactly is being proposed; (2) what the design looks like; and
(3) — the one you raised — how it relates to the barrier that you cannot cache
every intermediate value/env because there are hundreds of millions of them.

All mechanism claims below are verified in code at the cited sites.

## 1. The exact gap (verified, precise)

Trace identity today is **positional**: a value gets a recorded, hash-identified
trace iff it sits at a stable attr-path from the eval root. `TracedExpr` (the
identity carrier) is minted only by `materialize.cc:414/486` for attrset/list
CHILDREN; nothing else gets one. Verified three ways:
- Lever 2 (verify side): a derivation deep in `make-derivation.nix` has no
  attr-path → `makeChild` can't construct identity for it.
- The recorder spike (record side, commit 2ca68c498): a scalar producer
  (`pkg.outPath` is a STRING) has no identity → the edge can't fire; the existing
  parentSlot path already covers the identity-bearing attrset case.
- The flattening site (this session): `prim_derivationStrict` is a primop with
  **no DepCaptureScope of its own** (the only opener is
  `evaluateResolvedTarget`, per-`TracedExpr`). So `forceAttrs(args[0])` and all
  of `derivationStrictInternal` (primops.cc:1552, 1624) record their input-reads
  into **whatever scope called it** — the consumer's. The `derivationStrict`
  result is a `let`-binding thunk (`strict = derivationStrict drvAttrs`,
  derivation.nix) with **no trace identity**, so its epoch dep-range
  (`recordThunkDeps`, memo-replay-store.hh:93) flattens (copies) into every
  consumer that forces it. THAT is the per-derivation 607×.

Crucially: the content-addressed identity already EXISTS as a *value* —
`drvPath = hashDerivationModulo(inputs)` (primops.cc:1994), emitted as a
`StorePathAvailability(.drv)` dep (primops.cc:2003-2011). The gap is not "compute
an identity"; it is "**the derivation's input-reads are recorded against the
consumer's positional trace instead of against the derivation's existing
content identity.**"

> **MEASURED CORRECTION (2026-05-30, real evaluator — see
> `store/derivation-input-flattening.cc` + `store/derivation-edge-loadbearing.cc`):**
> The `StorePathAvailability(.drv)` dep is NOT a usable input-identity edge.
> It verifies by `isValidPath(oldDrvString)` — a pure EXISTENCE check
> (store-path-availability.cc) — so its answer is *identical before and after*
> the derivation's input changes (the old `.drv` string is what's recorded; in a
> real store it stays valid). Two real-eval facts now pinned:
> 1. A derivation whose `args` embed `readFile(shared)`, consumed by two siblings
>    via `.outPath`, flattens the shared `FileBytes` into BOTH consumer traces
>    (the 607× mechanism is REAL for derivations) — *alongside* an inert SPA dep.
> 2. The flattened `FileBytes` is therefore **load-bearing** for soundness; the
>    SPA dep cannot distinguish v1-input from v2-input.
>
> **Consequence for the design:** the producer-trace edge in §3 must be a
> producer-TRACE-HASH edge (a `TraceValueContext`-style dep whose value folds in
> the derivation's input deps), NOT the existing SPA dep. `drvPath`-the-string is
> input-addressed, but the SPA *dep* discards that. §3b already specifies a
> trace-hash edge, so the design target is unchanged — but the "already recorded"
> framing above was an over-claim: only the trace-hash edge tracks inputs, and it
> does not exist yet. This is additive recorder work, confirmed by measurement.

## 2. What is being proposed (one sentence)

Give a small, principled set of **non-attr-path values a content-addressed trace
identity** — starting with the `derivationStrict` result, keyed by its already-
computed `drvPath` — so its input-reads are recorded ONCE against that identity
(a "producer trace"), and the (many) consumers that force it record a single
**edge** to that identity instead of each flattening its input closure.

This is the build layer's `inputDrvs`-by-hash model (derivations.hh:336) +
`hashDerivationModulo` memoization (drvHashes, derivations.cc:19), lifted from
the build graph to the eval trace graph. It is NOT a new idea; it is the model
Nix already uses one layer down, applied to the layer above.

## 3. What the design looks like (concretely)

### 3a. A second identity kind: content-addressed trace nodes
Today: `TraceId` rows keyed (for lookup) by `(session_key, attr_path_id)` in
`Sessions`/`History` (sqlite-trace-storage-lifecycle.cc schema). The `Traces`
BODY table is already content-addressed by `full_hash` and attr-path-independent
(verified §schema) — so NO body-table change is needed.

Add a parallel routing key: a content-addressed identity
`CATraceKey = H(producer-kind, content-address)`. For derivations,
`content-address = drvPath`. This is a routing-layer addition (a
`ContentAddressedTraces(ca_key, trace_id)` table, or — cheaper — a reserved
`AttrVocabStore` namespace `"__ca:<drvHash>"` interned as an `AttrNameId`, which
`internName(string_view)` already accepts, so the EXISTING Sessions/History/
recovery pipeline works unchanged). The spike confirmed
`resolveTraceContextHash` already does recursive, memoized, cycle-broken edge
verification (verifier.cc:243-278) — the consume side needs NO new machinery.

> **PINNED (2026-05-30, `store/ca-trace-key-routing.cc`):** Design A's
> routing mechanism is proven against the real store/vocab/verify pipeline
> with no production change: (R1) a producer trace recorded under a synthetic
> `"__ca:<drvHash>"` key round-trips and verifies as a first-class trace
> identity; (R2) TWO consumers at genuinely DIFFERENT attr-path positions both
> edge to the SAME CA producer (via the `TraceValueContext` trace-hash edge)
> and BOTH hit — the cross-scope sharing C2b could not show with a literal
> vpath; (R3) mutating the shared producer's input changes its trace hash and
> invalidates BOTH consumers' edges. The edge is load-bearing, not vacuous:
> corrupting the stored producer hash flips R2 to a miss. This is the
> consume-side floor; only the §3b producer-trace boundary is net-new.

### 3b. A producer-trace boundary at `derivationStrict`
Wrap the `derivationStrict` result evaluation in its own dep-capture scope (the
thing it lacks today), keyed by `CATraceKey(drvPath)`:
- Open a DepCaptureScope around `forceAttrs(args[0])` + `derivationStrictInternal`
  so the input-reads are captured as THIS producer's own deps, not the
  consumer's.
- Record a producer trace under `CATraceKey(drvPath)` with those deps + the
  `{drvPath, outputs}` result. Content-addressed dedup (`getOrCreateTrace` by
  `full_hash`) means a derivation reached from N attr-paths records ONCE.
- The consumer that forced it records a single `TraceValueContext`-style edge to
  `CATraceKey(drvPath)` (the existing edge dep kind), NOT the flattened inputs.

The chicken-and-egg the spike surfaced (you must know the identity before
forcing, but drvPath is only known after forcing the inputs) is resolved by the
producer-trace being keyed AFTER the force, on the result: the consumer's edge is
recorded when it observes the already-computed `drvPath`. The consumer does not
need the identity before forcing — it needs it at the moment it records its dep,
which is after.

### 3c. Soundness — inherited from the tests already landed
- Input change → different drvPath → producer trace hash differs → consumer's
  edge mismatches → invalidate. (Pinned: `derivation-outpath-soundness.cc`
  `DrvOutPath_InputChange_NoStaleServe` + `DrvPath_IsContentAddressedByInputs`.)
- Facet hazard: an output edge is sound ONLY if the consumer observed output-
  only; a file-backed facet read records its own deps and must NOT be dropped.
  (Pinned: `derivation-observation-facets.cc` + the C3 hole in
  `derivation-edge-soundness.cc`.) The producer-trace boundary must therefore
  cover ONLY the derivation's own inputs; facets the consumer reads stay on the
  consumer.
- Cross-trace escape: a producer trace consumed by many is the keyset-escape
  shape — already guarded (`keyset-escape.cc`).
The soundness floor for this design is, deliberately, the test suite this
session built BEFORE the design.

## 4. Your barrier: "you can't cache all intermediate values — there are
   hundreds of millions"

This is the central question, and the proposal is precisely a way to NOT do
that. Three points, each grounded:

**(a) This proposal caches FEWER nodes, not more.** The barrier is real for
"hash every Value/Env" — `closures.gnome` forces ~22.9 M thunks (measured). But
the proposal adds identity to a SMALL, principled set: derivations
(~6,419 in `closures.gnome`; ~10,455 `derivationStrict` calls measured) — a
~3,500:1 reduction vs thunks. That is the SAME order as the ~10,397 attr-path
nodes already traced today. The node count does not grow; what changes is that
the existing-density nodes reference each other by hash instead of inlining.

**(b) The barrier is exactly WHY the boundary must be principled, not "all
values."** The reason "cache every value" fails is not storage of the identities
(those are cheap) — it is that most intermediate values have NO stable,
cross-process, content-addressable identity: a `Value*` is an ephemeral GC
pointer (BUG-7, memo-replay-store.hh), and a value's meaning depends on its
captured Env. You CANNOT content-address an arbitrary intermediate value cheaply
and soundly. So the proposal restricts identity to values that ALREADY HAVE a
content address from elsewhere:
  - derivations: `drvPath` (the build layer computed it).
  - imported files: the resolved source path + a scope fingerprint
    (`computeNixScopeHash` already exists, nix-binding.cc:111).
  - attr-path nodes: their position (today's mechanism, kept).
Everything else — the hundreds of millions of intermediate thunks — gets NO
identity and continues to flatten into its nearest enclosing boundary. **The
flattening is not eliminated; it is BOUNDED**: it stops at the first
content-addressed node below, instead of spanning the whole transitive closure.
This is the precise answer to your barrier: the proposal does not cache all
intermediate values — it caches only the ones that are already content-
addressed, and lets the rest flatten within tight bounds.

**(c) The recursion that would explode is cut at the identity boundary.** A naive
`trace_hash(N) = H(N's deps, children's trace_hashes)` would recurse into every
value. It does NOT, because a child becomes an EDGE only if it is identity-
bearing (a derivation / import / attr-path node); otherwise its deps flatten into
N as today. The recursion bottoms out at the ~thousands of identity-bearing
nodes, never at the ~millions of anonymous thunks. (This is the same reason
`hashDerivationModulo` terminates: it recurses on input DRVS, not on every value
that went into computing them.)

## 5. The honest cost / risk ledger
- **Producer-trace boundary at `derivationStrict`** = a new DepCaptureScope on the
  hot eval path, per derivation (~10 K/closure). The vptr-in-hot-loop reversal
  (rearch §2.1) warns that hot-path structure changes can regress; this must be
  measured against the Ledger-D baseline + the 10-commit correctness gate. The
  scope is per-derivation (~10 K), not per-thunk (~22 M), so the overhead is
  bounded — but it is NOT zero and must be benchmarked, not assumed.
- **Storage**: producer traces are content-addressed/deduped, so ~6,419 producer
  traces/closure, each carrying its OWN input deps (not the consumers' copies).
  Net storage should DROP (today's 343 MB values_blob is dominated by the 607×
  duplication). Must measure, not assume.
- **Routing key**: Design A (synthetic `__ca:` vocab namespace) needs no schema
  bump; Design B (parallel CA table) does. Start with A.
- **The facet gate** must be enforced at the edge-recording site: emit the edge
  only when the consumer's observation of the derivation is output-only (a
  recorded facet dep is the signal it is NOT). The spike showed this is
  detectable from the recorded dep set.

## 6. What this is NOT
- Not "cache every value" (your barrier) — see §4.
- Not a `replayMemoizedDeps` widening (the spike: dead, scalars have no identity).
- Not a `TracedExpr`-makeChild change (Lever 2: derivations aren't attr-paths).
- It IS: a producer-trace boundary at `derivationStrict` keyed by the EXISTING
  drvPath identity, + the existing edge-verify machinery on the consume side.

## 7. Recommended next step (smallest provable slice)
Before the full design, ONE measurement settles whether it is worth it: prototype
ONLY the producer-trace boundary at `derivationStrict` (open a DepCaptureScope,
record a `CATraceKey(drvPath)` producer trace), WITHOUT yet changing consumers to
edge — and measure (i) does it record ~6,419 deduped producer traces, (ii) what
is the per-derivation scope overhead on the Ledger-D benchmark, (iii) does cold
storage drop. If the scope overhead is acceptable, wire the consumer edge +
facet gate and re-run the soundness suite. This is the first slice that touches
the hot path with a measurable yes/no; it needs the correctness+benchmark gate
and is the genuine go/no-go for the whole direction.

Caveats: all counts are n=1 on `closures.gnome`/`python3Packages`; drvPath is
verified content-addressed (`DrvPath_IsContentAddressedByInputs`); the
per-derivation scope overhead is the one number that decides this and is
UNMEASURED.

## 8. Outcome (2026-05-31): MEASURED, NET LOSS, DEFAULT-OFF

The §7 measurement was completed. The conservative shape (no consumer-side
isolation; producer trace persisted alongside flattened consumer deps) measured
as a NET LOSS on the Ledger-D anchor (`closures.gnome`, 100 commits):

| | reference (no-trace) | cold (mean / median) | hot (mean) |
|---|---:|---:|---:|
| pre-§3b (run-1)  | 6.47s | 3.72s / 1.11s | **0.96s** (cache works) |
| with §3b (run-2) | 6.23s | **13.44s / 13.32s** | **13.47s** (cache mostly bypassed) |
| Δ                | (noise) | **3.6× slower** | **14× slower** |

Soundness PASS — `nix eval` is byte-identical with vs without the hook.
Performance regression is real and large.

**Decision: ship default-OFF.** The hook is gated on
`NIX_ENABLE_CA_PRODUCER=1`. The infrastructure (~17 commits, ~22 production
tests across 4 test files, plus 6 store-level tests) stays in tree as proven
scaffolding. Default behavior is identical to pre-§3b.

**Why the conservative shape lost.** The consumer keeps its flattened deps
(soundness floor unconditional), and the producer trace is persisted in
addition. So every `derivationStrict` call now records TWO traces: the
consumer's (as before) and a new CA-keyed producer. Cost: ~6,419 producer
records/commit. Benefit: the gate (`replayMemoizedDeps`) fires ~614 times per
commit on average — small. Hot is especially bad because cache-hit at the root
trace materializes a STRING (`closures.gnome.x86_64-linux`), but evaluating
that string still re-runs derivation thunks deep in `make-derivation.nix`, and
the hook fires for each. Per-session in-memory dedup helps within one process
but each `nix eval` is a fresh process, so the dedup doesn't span invocations.

**Why the aggressive shape was reverted earlier.** Sub-scope isolation around
`forceAttrs(*args[0])` + `derivationStrictInternal` was tried before the
conservative shape. It under-records ambient-eval deps that legitimately flow
into the consumer scope but aren't tied to the producer's content (e.g.,
`config.nix` reads from `mkDerivation`'s body, the flake source path identity).
Functional tests `eval-trace-core` and `eval-trace-deps` failed under the
aggressive shape — the consumer lost deps it needed.

**Critical adversarial-fix history (committed):**
- `8d393a1e9` — `producerBloom` template params were misread:
  `<Bits, PointerAlignment>` not `<NumSlots, NumHashes>`. Initial alignment 4
  (wrong); fixed to 16 to match GC-allocated `Bindings *`.
- `b8ae91b12` — `producerMap` keyed by `Bindings *`, not `Value *`. With
  `Value *` keying the gate **never fired** in real eval (`producerEdges = 0`
  despite 1129 producers persisted). Root cause: `prim_derivationStrict` receives
  `&v` that is `&vCur` — `callFunction`'s stack-local. After return,
  `vRes = vCur` (eval.cc:2530) COPIES the result; `&vCur` becomes stale.
  `Bindings *` is stable across the copy.
- `82713fd91` — default-off via `NIX_ENABLE_CA_PRODUCER=1` after bench measurement.

**Conditions that would justify re-enabling.** The infrastructure is sound; the
cost is the blocker. Re-enabling becomes attractive when one or more of:
1. **Async/batched producer recording.** `recordSync` runs synchronously per
   derivation. If producer-trace persistence batches at session end (or runs on
   a background thread with `traceId` reconciliation deferred), the per-call cost
   drops dramatically.
2. **Suppress on warm-served derivations.** When the consumer's TracedExpr is
   serving from cache, derivation thunks inside re-run as part of materialize.
   The hook firing here costs 100% (every record) for 0% benefit (the consumer
   isn't recording a new trace anyway). Detection requires plumbing not present
   today.
3. **Sibling-share-heavy workloads.** The gate fires from `SiblingForceScope` and
   `forceValue`'s replay branch (bloom-gated). On `closures.gnome` it fires ~614
   times/commit. A `nix-eval-jobs`-style deep attrset enumeration might exercise
   it more. NOT MEASURED on that workload yet.

**Cross-references.**
- Production code documented: `src/libexpr/eval-trace/CLAUDE.md` "RFC §3b" section.
- Test guide: `src/libexpr-tests/eval-trace/CLAUDE.md` `dep/` section.
- Bench data + adversarial-fix narrative: `doc/eval-trace-cache-redesign-plan.md`
  "2026-05-31 follow-up #4".
- Architectural backstory: `plans/architecture-trace-model-vs-CA.md`,
  `plans/compositional-trace-dag-design.md`.
