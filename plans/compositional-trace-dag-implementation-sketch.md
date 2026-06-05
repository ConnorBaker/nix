# Compositional trace-DAG — Tier-1 implementation sketch (output-string producer identity)

> **⛔ FINAL VERDICT (2026-06-02, §10.7 is authoritative — supersedes everything
> below, including §10's own sub-claims). DO NOT IMPLEMENT "OSPI".**
>
> **Calibrated conclusion:** the output-string re-key ("OSPI") is **UNPROMISING**,
> carried by two *measured* HOT non-wins: conservative §3b is **hot-neutral**
> (1.09s ≈ 1.00s baseline — NO win), and the as-built aggressive edge is
> **hot-negative** (1.81s) because warm verify must `loadTrace` ~12K separate
> producer-trace blobs that inline conservative avoids. The edge as built loses on
> hot; the *correctly-keyed* shape was never benched (projected ≈ 1.81s, not
> measured). (§3b also adds a cold cost the edge can't reduce, but the earlier
> "+6.7s recording" framing here is a STALE figure — the live perf doc re-measured
> store-write at 0.2s and attributes cold to the always-on tax + dep-capture; cold
> is not the clean "decisive" number this banner once called it. §10.7.) It is
> **NOT** cleanly "structurally dead": §17b/§18's "no producer-consumer sharing /
> structural catch-22" is **confounded** (it reads the mis-keyed gate's 306 fires
> as fan-out, contradicting §11's 200–2000 estimate), and the correctly-keyed
> shape was never benched. So: don't implement OSPI (the measured *proxy* — the
> mis-keyed edge — is negative, conservative gives no win, and §3b adds a cold
> cost; that justifies "don't invest", NOT "proven to lose" — OSPI's own re-keyed
> shape was never benched), and **don't** chase a high-fan-out workload trusting
> the confounded "no sharing."
>
> **Reading guide (this file is a layered record of three passes — read in this
> order):**
> - **§10.7 — the final calibrated verdict (start here).**
> - §0–§9 — the ORIGINAL sketch arguing OSPI is viable. **Refuted; historical.**
>   (Its §9.4 "warm re-record → 14×" is wrong — see §10.1.)
> - §10 — the second-pass refutation. Its measured facts (§10.1, §10.3) hold, but
>   its §10.2 ("no producer-consumer sharing") and §10.4 ("structural catch-22")
>   are **themselves superseded by §10.7** as confounded. Its §10.5 cold-lever
>   framing ("async Layer 2b") is superseded by the live perf doc (cold is ~57%
>   BLAKE3 hashing, not I/O; async is low-leverage; the lever is C3 = reduce the
>   flattened dep count). See `plans/eval-trace-perf-cold-and-hot.md`.

2026-06-02. Concrete, code-grounded implementation sketch for the first
shippable slice of the compositional trace DAG: **Tier-1 derivation edges**,
re-keyed to fix the three failure modes that made RFC §3b a 3.6×/14× regression.

Status of inputs to this sketch:
- Architecture: `plans/architecture-trace-model-vs-CA.md`,
  `plans/compositional-trace-dag-design.md` (the 607× flattening diagnosis).
- Prior attempt: RFC §3b (`plans/content-addressed-trace-identity-rfc.md`,
  default-off, measured net-loss). This sketch is the **re-key** that §3b's
  §8 named as the necessary path ("a viable variant must re-key identity onto
  the value consumers re-force") but did not implement.
- Every code claim below is cited at `file:line` and was read (not grepped).

---

## 0. One-paragraph thesis

The 607× shared-closure flattening happens at exactly one place — `forceValue`'s
WHNF replay branch (`eval-inline.hh:105`) firing `replayMemoizedDeps(v)` →
`replayMemoizedRange` (`dep-recording-context.hh:306-329`) — and the value `v`
that carries a shared derivation's input-reads and gets re-forced by every
consumer is the derivation's **`outPath` / `drvPath` string** (the
`derivation.nix` wrapper's `outPath = builtins.getAttr out strict`,
`drvPath = strict.drvPath`). §3b registered its producer side-table on
`strict`'s `Bindings *` — a value **no consumer ever re-forces** — so its gate
fired ~0.03:1. The fix is to move the producer identity onto the **output
strings**, which propagate to consumers through `ExprSelect`'s `v = *vAttrs`
struct copy (`eval.cc:2207`) because a string's `publication` pointer is
**intrinsic to the Value payload** (`value.hh:382,428`), not a `Value*`-keyed
side table. The existing `replayMemoizedDeps` gate already `return`s after
emitting one edge — so re-keying onto the string simultaneously (a) makes the
gate fire and (b) **replaces** the flatten instead of adding to it. Consume-side
machinery (`resolveTraceContextHash`, `__ca:<drvHash>` routing, R1/R2/R3 tests)
is unchanged and already proven.

---

## 1. The proven foundation (REUSED, zero new work)

The consume side is discharged by landed tests and needs no change:

- **Edge dep kind:** `Dep::makeValueContext(AttrPathId pathId, DepHashValue)`
  (`deps/types.hh:1016`) — a `TraceValueContext` dep whose value is the target
  trace's hash.
- **Edge verification:** `SqliteTraceStorage::resolveTraceContextHash`
  (`store/verifier.cc:412-484`) recursively verifies the edge-target trace,
  memoizes per `(parentPathId, nodeStamp)` (`:452-455,479`), breaks cycles via
  `session.inProgressTraceIds` (`:1999-2006`), and History-bootstraps a producer
  recorded under a rotated session key (`:436-442,469-473`) — the CA-producer
  cross-session case.
- **Routing key:** a synthetic `__ca:<drvHash>` vocab path
  (`trace-session.cc:820-823`), interned like any attr name — no schema change.
- **Producer persistence:** `TraceSession::recordCAProducer`
  (`trace-session.cc:807-895`) + `TraceBackend::recordSync`
  (`context.cc:414-448`) persist a producer trace under the `__ca:` key with a
  given `innerDeps` vector and return its trace hash; WRITE-ONCE dedup per caKey
  (`memo-replay-store.hh:245-255`, `producerByCaKey`).
- **Proof it works:** `store/ca-trace-key-routing.cc` R1/R2/R3 — a producer
  under `__ca:<drvHash>` round-trips; two consumers at *different* attr-paths
  both edge to it and both hit; mutating the producer's input invalidates both;
  corrupting the stored hash flips R2 to a miss (the edge is load-bearing).

**What is net-new in this sketch is only the PRODUCER side: where the identity
attaches and which value the gate keys on.**

---

## 2. The flatten mechanism, pinned to code (why §3b's key was wrong)

### 2.1 `derivation.nix` is the indirection that defeated §3b

`src/libexpr/primops/derivation.nix:34-59`:

```nix
let
  strict = derivationStrict drvAttrs;            # the primop result §3b keyed on
  commonAttrs = drvAttrs // (listToAttrs outputsList) // { all = …; inherit drvAttrs; };
  outputToAttrListElement = outputName: {
    name = outputName;
    value = commonAttrs // {
      outPath = builtins.getAttr outputName strict;   # ← a STRING; what consumers read
      drvPath = strict.drvPath;                        # ← a STRING; what consumers read
      type = "derivation"; inherit outputName;
    };
  };
in (builtins.head outputsList).value                   # ← the WRAPPER attrset
```

A consumer with `buildInputs = [ B ]` coerces `B` to a string during *its own*
`derivationStrictInternal` (the "force-of-args" path, RFC §8): `coerceToString`
forces the wrapper and reads `B.outPath` (+ `B.drvPath` into the string
context). It **never forces `strict`**. §3b's side table was keyed on
`strict`'s `Bindings *` (`primops.cc:1685-1697`, `producerKeyFor(v)=v.attrs()`
at `context.hh:55-58`), so `lookupProducer(wrapper.attrs())` missed and
`lookupProducer(outPathString)` short-circuited (`producerKeyFor` returns
`nullptr` for non-attrsets — the "scalar-identity wall"). E:P ≈ 0.03.

### 2.2 The value that actually flattens, and why a stamp reaches it

The strings the consumer reads are created in `derivationStrictInternal`:
`drvPath` at `primops.cc:2142-2148`, each output via `mkOutputString`
(`primops.cc:218-231` → `result.alloc(o.first)`), assembled into the `strict`
result at `v.mkAttrs(result)` (`primops.cc:2152`). `derivation.nix`'s
`getAttr out strict` / `strict.drvPath` re-expose them.

`ExprSelect::eval` ends with `v = *vAttrs` (`eval.cc:2207`) — a **struct copy**
of the selected attr's `Value`. A string Value's `publication` is a pointer
**inside the payload** (`value.hh:367-382` `Context::publication`, `:428`
`Details::publication`), so the struct copy carries it. Therefore a publication
stamp placed on `strict.out` / `strict.drvPath` **propagates to
`wrapper.outPath` / `wrapper.drvPath`** — across the language-layer wrapper.

The flatten itself: `forceValue` on a non-thunk runs the else-branch
`traceActiveDepth && mayHaveMemoizedDeps(v) → replayMemoizedDeps(v)`
(`eval-inline.hh:105-106`), where `mayHaveMemoizedDeps(v) =
replayBloom.test(&v)` (`eval.cc:1883`). When the **first** consumer forces
`wrapper.outPath` (thunk→WHNF), `recordThunkDeps` memoizes the range that grew
(= B's input-reads, because forcing `getAttr out strict` forces `strict`
fresh) and sets the bloom on that slot Value\* (`memo-replay-store.hh:176-188`).
The wrapper's `outPath` slot is **one shared Value\*** (B is evaluated once), so
the other 606 consumers re-force the *same* WHNF slot → bloom hit →
`replayMemoizedDeps(wrapperOutPathSlot)` → today: `replayMemoizedRange`
flattens B's range into each. That is the 607×.

**Key consequence:** the gate is already *reachable* for the 606 subsequent
consumers (bloom set by the first), and the slot already *carries* `strict.out`'s
publication (copied at the first force). We only need (a) to put a producer stamp
on that publication and (b) to teach the gate to read it.

---

## 3. The design: output-string producer identity (OSPI)

### 3.1 Producer registration (at `derivationStrict`, drvPath known)

At the end of `prim_derivationStrict`, where §3b already computes
`innerDeps = snapshotEpochRange(epochStart, epochEnd)` and `drvPathS`
(`primops.cc:1685-1697`), keep the producer-trace recording
(`recordCAProducer`) **but change what gets stamped**:

- §3b: `state.traceCtx->registerProducer(producerValue.attrs(), caKey, hash)`
  (`trace-session.cc:874`) — keys the side table on `strict`'s `Bindings *`.
- OSPI: stamp the **publication** of `strict`'s `drvPath` string and each
  output string (`v.attrs()->get(s.drvPath)`, `v.attrs()->get(<outputName>)`)
  with a producer marker. Concretely, extend `IdentityObject`
  (`semantic-objects.hh:85`) — or add a sibling field on `SemanticHandle` — to
  carry a `ProducerId` (a small interned id), and keep a runtime side table
  `ProducerId → {AttrPathId caKey, DepHash traceHash}` (this is the existing
  `producerByCaKey` content, re-indexed by `ProducerId`).

`innerDeps`, the `__ca:<drvPath>` key, and `recordCAProducer` are unchanged.
drvPath (always concrete, even for floating-CA outputs) is the routing key;
output strings may be output placeholders, so they carry the *same* ProducerId
rather than being the key themselves.

### 3.2 Gate (in `replayMemoizedDeps`, already the flatten site)

`TraceRuntime::replayMemoizedDeps` (`context.cc:1053-1124`) currently:

```cpp
if (auto key = producerKeyFor(v))                       // attrset Bindings* only
    if (auto producer = replayStore.lookupProducer(key))
        if (auto access = TraceAccess::current()) {
            access->record(Dep::makeValueContext(producer->caKey,
                            DepHashValue(producer->traceHash)));
            return;                                       // ← already replaces the flatten
        }
// … else getReplayRange(v) → replayMemoizedRange (the flatten)
```

Add a **string tier** before the attrset tier: if `v` is a string whose
`publication()->identity` carries a `ProducerId`, look it up and emit the same
edge + `return`. Because the function already `return`s after the edge, the
subsequent `replayMemoizedRange` is skipped — **the edge replaces the flatten**
(fixing FM1) and it now fires on the value consumers re-force (fixing FM2).

Hot-path note: the string tier is only reached for bloom-positive values
(`mayHaveMemoizedDeps` already gates entry to `replayMemoizedDeps`), so it adds
one `v.type()==nString` + publication-pointer + flag check to an
already-rare path — not to the 22.9M `forceValue` fast path. (Measurement
gate — §6.)

### 3.3 Why no sub-scope isolation (fixing FM3)

§3b's *aggressive* shape opened a DepCaptureScope around
`forceAttrs(args[0]) + derivationStrictInternal` and used
`replaceWindowWithEdge` (`dep-recording-context.hh:301`) to delete consumer
`ownDeps` by key-membership in the producer window. It under-recorded ambient
deps and broke functional tests (`primops.cc:1611-1619` records the post-mortem).

OSPI does **not** isolate. `innerDeps` is the same `snapshotEpochRange` §3b
already captured; the producer trace folds in *exactly* the range that would
otherwise flatten into a consumer. The consumer change is purely: when the gate
fires on the stamped string, emit one edge and skip the range copy. There is no
key-filtering and no sub-scope, so the ambient-dep-loss failure mode cannot
recur — deps a consumer makes outside the stamped string's force stay in the
consumer's scope.

---

## 4. Soundness argument

Let producer trace `P = __ca:<drvPath_B>` record `innerDeps_B`
(B's input-reads). Consumer C, instead of flattening `innerDeps_B`, records one
edge `makeValueContext(P, traceHash_B)`.

1. **Edge re-verifies the exact replaced set.** `resolveTraceContextHash`
   recursively `verifyTrace`s P against the *live* filesystem
   (`verifier.cc:464`), recomputing `innerDeps_B`'s hashes. If any of B's
   input-reads changed, P's trace hash differs, the embedded `traceHash_B`
   mismatches, the edge is unresolvable → C invalidates (fails closed). If
   unchanged, the edge resolves → C hits. (Pinned by R3 + `derivation-outpath-
   soundness.cc DrvOutPath_InputChange_NoStaleServe`.)
2. **drvPath is content-addressed by inputs** (`DrvPath_IsContentAddressedByInputs`),
   so the routing key cannot alias two semantically different producers.
3. **Facet soundness (RFC §3c).** Only `outPath`/`drvPath` **strings** are
   stamped. A consumer that reads `B.meta`/`B.passthru` forces the *wrapper
   attrset* and reads non-output attrs; those reads are recorded normally in C's
   scope and are *not* the stamped strings' replayed range — so the edge never
   replaces a facet observation. (Pinned by `derivation-observation-facets.cc`,
   the C3 hole in `derivation-edge-soundness.cc`.)
4. **Superset-safety of `innerDeps`.** As §3b already notes
   (`primops.cc:1676-1684`), the captured range may be a *superset* of B's own
   reads (nested producers, warm-hit replays). A superset can only
   over-invalidate (drop a hit), never stale-serve — soundness-preserving.
5. **Cross-trace escape** is the keyset-escape shape, already guarded
   (`store/keyset-escape.cc`).

The soundness floor is exactly the suite that predates the design
(`derivation-edge-soundness.cc`, `derivation-observation-facets.cc`,
`derivation-outpath-soundness.cc`, `keyset-escape.cc`).

---

## 5. Honest open questions / risks (the adversarial-review targets)

- **R-FIRST: the first consumer flattens.** The bloom is set by the first
  consumer's *fresh* force of `wrapper.outPath`; that consumer records B's reads
  directly (not via replay), so it does not edge. 1/607 residual. Refinement: in
  `recordThunkDeps`, when memoizing a range for a producer-stamped value, emit
  the edge into the active scope and truncate the just-recorded range. Defer
  until measured — the 606/607 win dominates.
- **R-REACH: gate reachability is timing-dependent.** If forcing
  `wrapper.outPath` grows the epoch log by zero (e.g. `strict` already WHNF from
  B's own attr-path materialization), `recordThunkDeps` sets no bloom entry
  (`memo-replay-store.hh:179`), the slot is bloom-negative, and
  `replayMemoizedDeps` is never called → no edge. Must measure the actual
  fire-rate; may need a publication-based entry into the gate (a `||
  hasProducerStamp(v)` in `eval-inline.hh:105`, which costs the 22.9M path —
  measure before adding).
- **R-HOTSERVE: the §3b 14× hot regression.** §3b re-recorded a producer on
  every `derivationStrict` call including warm-materialize re-runs. OSPI keeps
  `recordCAProducer` at the same site; if `derivationStrict` re-runs under a
  warm-served root, the producer re-records (WRITE-ONCE caKey dedup
  (`memo-replay-store.hh:249-254`) makes it cheap but not free). Must confirm
  cold-only recording, or gate recording on "currently cold-recording this
  trace."
- **R-IDOBJ: IdentityObject overload.** `IdentityObject` already carries a
  `valueIdentityStamp` used by `sameValueIdentity` (`context.cc:1131-1138`) and
  string materialization (`materialize.cc:445-450`). Adding a `ProducerId` must
  not collide with that use — likely a separate `SemanticHandle` field, not a
  reuse of `identity`. Verify the publication round-trips through
  materialization (a warm-served output string must still carry its stamp, or
  re-derive it).
- **R-ORDINAL: trace-hash determinism.** `feedDep` writes a positional
  `dep.ordinal` over the globally-sorted dep vector (`deps/hash.hh:61-72`). With
  edges replacing large leaf runs, confirm the producer trace hash and the
  consumer trace hash are deterministic across runs (the
  `compositional-trace-dag-design.md` blocker-2). The consume-side tests already
  exercise edge hashing, but not at edge-heavy density.
- **R-SHARING: is `wrapper.outPath` actually one shared Value\*?** The 607× win
  assumes B's wrapper (and its `outPath` slot) is a single shared Value across
  consumers (the nixpkgs fixpoint). Where a "shared" derivation is re-evaluated
  per consumer (independent `import`/application instances), the slot is *not*
  shared, the bloom/stamp don't transfer, and OSPI degrades to today's flatten
  for those — correctness-preserving, but the win is workload-dependent. Pin
  with the Ledger-D `closures.gnome` E:P counter.

---

## 6. Sequenced, measurable slices (each gated by correctness + bench)

Reuse §3b's env-gate (`NIX_ENABLE_CA_PRODUCER`) so default behavior is
unchanged until proven.

1. **S1 — stamp + gate, conservative (keep flatten).** Stamp the output strings;
   add the string tier to the gate but DO NOT skip the flatten yet (emit the edge
   *and* keep the range). Purpose: measure the **fire-rate** (E:P) with the
   re-key, isolated from any soundness change. Counter:
   `nrReplayProducerEdges` / producer records. **Go/no-go:** E:P must rise far
   above §3b's 0.03 (target ≳ 0.5) on `closures.gnome` + the
   `python3Packages.${n}.outPath` sweep. If it doesn't, R-REACH dominates and the
   gate needs the publication entry first.
2. **S2 — flip to replace (skip flatten on fire).** Let the gate `return` after
   the edge. Run the full soundness suite + functional `eval-trace-*` +
   asciidoc/closures byte-identity. **Go/no-go:** byte-identical eval; consumer
   trace dep-counts drop (decode the cold DB histogram, cf.
   `architecture-trace-model-vs-CA.md` §2's 3,376 deps/trace baseline).
3. **S3 — Ledger-D bench.** `nix run .#eval-trace-bench -- generate
   --num-commits 100`, reference/cold/hot. **Go/no-go:** cold storage drops, hot
   ≤ pre-§3b 0.96s (NOT the 13.47s regression), 100/100 sound. This is the gate
   §3b failed.
4. **S4 — first-consumer refinement (R-FIRST)** only if S3 shows the 1/607
   residual matters.
5. **S5 — Tier-2 (imports/files) and Tier-3 generalization** — out of scope for
   this sketch; the design (`compositional-trace-dag-design.md` §Tier-2/3) needs
   the keyset-facet rule and is sequenced after Tier-1 proves the mechanism.

---

## 7. What changes vs §3b (surgical diff)

| | §3b (default-off, net-loss) | OSPI (this sketch) |
|---|---|---|
| Producer trace recording | `recordCAProducer(innerDeps)` | **same** |
| `__ca:<drvHash>` routing + verify | proven (R1/R2/R3) | **same, reused** |
| Side-table key | `strict`'s `Bindings *` (never re-forced) | output-string `ProducerId` via publication |
| Gate keying (`replayMemoizedDeps`) | `producerKeyFor(v)=v.attrs()` | + string-publication tier |
| Flatten removal | none (conservative) / key-filter (aggressive, reverted) | automatic — gate `return`s after edge |
| Isolation sub-scope | aggressive only (reverted, under-recorded) | **none** (no ambient-dep loss) |
| Fires on | values consumers don't re-force (E:P 0.03) | the outPath/drvPath strings they do |

The net-new code is ~the stamp at `primops.cc:1685-1697` (publication instead of
`Bindings*`) + the string tier in `context.cc:1071-1080` + the
`IdentityObject`/`ProducerId` plumbing. Everything else is reuse.

---

## 8. Verdict (pre-review)

The user's original intuition holds: CA + deferral + materialization is the
right mechanism. §3b failed on *keying*, not on the idea. OSPI re-keys onto the
value consumers actually re-force (the output strings), reached via a
struct-copy channel the code already guarantees, and makes the existing
edge-replaces-flatten gate fire — turning §3b's additive cost into a net
reduction, **if** the fire-rate (R-REACH) and hot-serve (R-HOTSERVE) risks
clear their measurement gates. Those two are the go/no-go, and S1 isolates the
first cheaply.

---

## 9. Adversarial review findings (2026-06-02, three independent reviewers + linchpin verification)

Three code-grounded adversarial reviewers (mechanism / soundness / cost) +
direct verification of the contested linchpin against the committed tests and
the redesign-plan follow-ups #7/#8/#10. Net: **mechanism viable, soundness
clean, but the keying fix is necessary-not-sufficient — the two costs that
actually sank §3b are carried unchanged, plus a warm-serve stamp-drop bug.**

### 9.1 Mechanism — VIABLE (all four load-bearing claims CONFIRMED, R-REACH softened)

- Claims 1-4 confirmed at code level. One correction: a string's `publication`
  is not a third payload word; it lives in the heap `Context` the payload's
  `context` pointer addresses (`value.hh:367-382,405-408`, `eval-inline.hh:168`).
  The struct copy `v = *vAttrs` still propagates it (shared `context` pointer),
  so the mechanism holds for the stated reason's *corrected* form.
- `producerKeyFor` returning nullptr for non-attrsets (`context.hh:55-58`) is the
  confirmed "scalar wall" — exactly why §3b's attrset key never matched.

### 9.2 The linchpin (replace vs additive) — RESOLVED in OSPI's favor for the N−1

The sketch's central "edge **replaces** the flatten" claim was challenged by
follow-up #8 ("flatten is force-of-args-driven → an output-read edge is
additive"). Verified directly against the committed tests:
- **#8** (`DrvInput_FlattenSite_ForceVsOutputRead`, cw consumer) measures the
  **first/independent forcer**: its FileBytes enters via the direct args-force
  recording, upstream of the output-string read → an edge there *is* additive
  for *that* consumer (the R-FIRST residual, 1 of N).
- **#10** (`dep-flattening-baseline.cc::SharedDrv_SameOutPath_BothConsumers`...,
  the faithful artifact-based re-measurement that *supersedes* #8 for the shared
  case) measures **two consumers sharing one `d.outPath`**: cxHasFile=1,
  cyHasFile=1 — both flatten **via `replayMemoizedRange`**, and the per-consumer
  event **re-forces the output string** (redesign-plan:2690,2707).

So for the N−1 subsequent consumers (N≈200-2000/derivation, #11) the flatten is
**replay-driven on the output string** — exactly the `replayMemoizedDeps` site
OSPI's gate precedes (`context.cc:1082,1121`). OSPI **replaces** the flatten for
the N−1 and is additive only for the 1 first forcer. Reviewer C's "additive →
dead" verdict relied on #8 alone and missed #10's correction; it is **refuted**
for the population that matters.

### 9.3 Soundness — CLEAN (not the blocker)

Six vectors reviewed; no constructable stale-serve. The edge re-verifies the
producer's `innerDeps` against the live FS on every consumer verify
(`verifier.cc:464,476`), including the History-bootstrap (rotated-key) path,
which re-publishes only **after** `verifyTrace` passes (`:469-473`). The facet
rule is mechanically enforced (stamp on the output **string** only; facet reads
land in the consumer scope — `derivation-observation-facets.cc`). `innerDeps` is
a superset of B's reads → can only over-invalidate. The trace hash is
sort-normalized (`sortAndDedupDeps`, `hash.cc:15-26`) → deterministic for
correctness. OSPI introduces no new vector beyond the existing OR-3/OR-4 floor.
**Soundness was never §3b's blocker, and OSPI doesn't change that.**

### 9.4 Cost — THE REAL BLOCKER (keying is necessary, not sufficient)

§3b's measured regression was **record cost (3.6× cold) + warm re-record (14×
hot)**, not fire-rate. OSPI fixes fire-rate (the keying) and carries both costs
**unchanged**:
- **C1 (cold):** `recordCAProducer` is still a synchronous per-derivation
  `recordSync` holding the store mutex (~6,419/commit, `context.cc:439-448`).
  Untouched. The edge benefit is a *verify/storage* dep-count reduction
  (measured 35-90%, plausibly ~99% — #11 Attack B, #13), a **different currency**
  than the record-time cost.
- **C3 (hot, the killer):** `prim_derivationStrict` runs unconditionally under a
  warm-served root (no warm-served check, `primops.cc:1605`); the
  `producerByCaKey` dedup is empty in a fresh process, so the first force of each
  derivation still pays full `recordSync`. This is §3b's 14×. OSPI's R-HOTSERVE
  flags it but does **not** solve it; the suppression plumbing (thread
  warm-serve state to the primop) does not exist.
- **R-IDOBJ stamp-drop (new, from review):** a `ProducerId` field on
  `SemanticHandle` would be **silently dropped** by the trace-result codec
  (`trace-result-codec.cc:193-243` serializes only path/text/identity) **and** by
  `mergeSemanticHandle` (`eval.cc:3341-3346`). So a cold-stamped output string
  **loses its stamp on warm/cross-process serve** → the gate can't fire on the
  hot path — precisely the path that produced §3b's 14×. §3.1/§7 omit both sites.

### 9.5 Corrected go/no-go sequence

The cheapest decisive measurement is **not** S1 (fire-rate). It is a **warm-serve
producer-record counter on the EXISTING `NIX_ENABLE_CA_PRODUCER` binary** (zero
new code): does `recordCAProducer` still fire ~6,419×/commit when the
`closures.gnome` root is warm-served? If yes (C3 predicts it will), OSPI
regresses 14× hot regardless of keying — dead before any stamp/gate work.
Promote this ahead of S1. S1's E:P fire-rate, if it passes, is **necessary but
wildly insufficient** — §3b passed soundness and still lost on cost.

### 9.6 What this means for the direction

OSPI is the **correct keying fix** and is **sound**, but it is **not
net-positive alone**. The honest precedence (Reviewer C, matching #16 Attack 4):
the binding prerequisites are (a) **async/batched producer recording** (kills
C1), (b) **warm-served suppression** of `recordCAProducer` (kills C3), and (c)
**ProducerId through the codec + merge** (kills R-IDOBJ). The keying makes the
benefit *exist*; those three make it *worth it*. Build/measure (a)+(b)+(c) — or
at minimum run §9.5's warm-record counter — **before** investing in the
stamp/gate. The compositional-trace-DAG direction is alive; this sketch is one
necessary piece of it, not the whole.

---

## 10. REFUTATION (2026-06-02, second adversarial pass — its "measured-dead" verdict is CALIBRATED DOWN by §10.7) — the proof pre-existed

The user challenged: *"Are you sure you understand the semantics correctly? And
the implementation you'd be testing with is correct?"* Both landed. Verifying
directly against git history and `derivation-producer-partition-rfc.md` §16–§18 —
**the conclusion of the exact arc this sketch extends, which §0–§9 never read** —
overturns the whole thing.

### 10.1 The §9 cost verdict was built on a pre-Fix-1a buggy binary

The 14× hot I cited as "the killer" was measured (`#4`, commit `82713fd91`)
**before** `868038369` "Fix 1a — closes the §14 cache-defeat" (an ancestor-then-
descendant pair, verified via `git merge-base`). The pre-Fix-1a §3b had a
producer-overwrite bug: re-record with an empty range overwrote producer
CurrentNodes → consumer edges failed warm verify → **30,629 broken sessions →
full re-eval**. **§16 re-benched under Fix 1a: §3b-conservative hot 7.95s → 1.09s
(≈ 1.00s baseline); the 14× was 93% that fixable defect.** And decisively for my
C3 premise: on a clean warm hit "the root trace serves the whole tree from cache,
**no derivation thunks re-force → no producer hook**" (hot `producerEdges=0`).
**`derivationStrict` does NOT re-run on warm-serve.** My proposed §9.5
"warm-record counter" measurement would have read ~0 and misled me. The §3b
binary is not the faithful proxy I claimed — and the corrected number already
existed.

### 10.2 The semantic error: leaf-dep sharing ≠ producer-consumer sharing

> **⚠ SUPERSEDED by §10.7 (third pass).** This section repeats §17b's "~97%
> singly-consumed → no producer-consumer sharing" *as fact*. That is **confounded**
> — `producerEdges`=306 is the mis-keyed gate's fire count (§5), not fan-out; it
> contradicts §11's 200–2000-consumers/derivation estimate. Read §10.7. The
> *sound* reason the edge loses on hot is the loadTrace cost (§10.3), not "no
> sharing."

My core thesis — "the 607× is shared closure an edge can amortize" — is **wrong
at the granularity that matters**, and §17b measured it. The 607× is the same
**leaf** dep (a file) appearing in many traces. An edge amortizes a shared
**producer trace** referenced by many consumer traces. On `closures.gnome` the
aggressive (edge) shape fired **306 consumer edges against ~12,836 producer
traces — ~97% of producers are singly-consumed.** The sharing is at the leaf,
not the producer-consumer boundary; the edge targets the wrong boundary. There
is no producer-consumer fan-out to amortize against.

### 10.3 The mechanism is a verify-time LOSS, not a win (the part the microbench missed)

§17 measured the aggressive shape (edge **replaces** flatten — *precisely OSPI's
mechanism*) under Fix 1a: **hot 1.81s vs 1.09s conservative — hot-NEGATIVE, and
inherent.** An edge's warm verify is not one hash-compare; it is a full
`loadTrace` (SQLite row read + zstd-decompress of the producer's keyset+values
blob) + recursive `verifyTrace`. `loadTrace.count 431 → 12,186`;
`verifyTrace.timeUs 0.55s → 2.88s`. **Inline flattened deps already in the
consumer's blob (one decompress) are cheaper to check than a pointer chased into
a separate trace.** The ~63% dep-count reduction is real but converts to a
verify-time loss. The `ca-edge-verify-cost.cc` M≈1 microbench that blessed the
gate measured steady-state with the producer already loaded; it missed the 12K
cold trace loads that dominate at scale.

### 10.4 §18's structural catch-22 names OSPI explicitly and kills it

> **⚠ SUPERSEDED by §10.7 (third pass).** §18's catch-22 rests on §17b's
> confounded "~97% low-fan-out" (see §10.2's marker). It is NOT a proven
> structural obstruction. The decisive reason not to pursue OSPI is the cold
> recording cost, not this.

- Edge to a **low-fan-out** producer (≤1 consumer, ~97% here) = pure cost.
- Edge to a **high-fan-out** producer (glibc) would amortize — but consumers
  reach it via its **output string** (`outPath`), which doesn't fire the gate
  without **re-keying**.
- **Re-keying to fire on output-string consumption fires it for ALL producers,
  including the 12K low-fan-out ones → hot WORSE.** ← This *is* OSPI.
- Targeting only high-fan-out producers needs each producer's consumer count
  **at record time** — undecidable locally (the keyset-downgrade fail-closed
  class). No tractable record-time discriminator.

§17b states it directly: *"Re-keying (Prerequisite 2) would make MORE edges fire
— but to mostly-singly-consumed producers — so it cannot rescue the hot cost; it
would likely make it worse."* OSPI is that re-keying.

### 10.5 What's actually true (the arc's real conclusion, §18)

- **HOT is already good without §3b**: no-§3b serves `closures.gnome` warm in
  ~1.0s, 7/7 hits, 0 re-eval. There is **no hot problem** for producer-partition
  to solve; §3b only ever risked making hot worse.
- **COLD is the real cost** of the cache overall (this section's "~6.7s recording"
  figure is STALE — see the §10.7 correction: store-write is 0.2s; cold is the
  always-on tax + dep-capture). §3b does **not** help cold — it *adds* to it. The
  cold lever is **orthogonal to producer-partition** — see `plans/eval-trace-perf-cold-and-hot.md`.
  (Note: the cold split was later MEASURED at ~57% BLAKE3 hashing / ~4% I/O, so
  "async/off-thread recording" is low-leverage; the named lever is C3 = reduce the
  flattened dep count. This corrects this section's original "async Layer 2b"
  phrasing.)
- The one durable win from the whole arc is **Fix 1a** (write-once caKey dedup),
  a correctness fix that makes §3b hot-neutral and is independent of the edge
  direction.

### 10.6 What I got right vs wrong

- RIGHT: the *keying diagnosis* (§3b keyed `strict`; consumers re-force the
  output string) — matches §5/§18. And soundness (matches §18 pt 1).
- WRONG: the inference that re-keying → net win. It is the measured-dead lever.
  And the verify-cost model (edge cheaper than inline) — it is the reverse at
  scale. And reading a 14× number off a known-buggy binary as "the blocker."
- PROCESS MISS: I sketched + ran three reviews + a "linchpin verification" while
  never reading §16–§18, the *conclusion of the arc I was extending*. The
  reviewers couldn't catch it either — none was pointed at the producer-partition
  RFC's final sections. The lesson (again): before extending an arc, read its
  end. The artifact existed; I re-litigated a closed question.

**Disposition (CALIBRATED — see §10.7 below for the correction):** the
producer-edge / re-keying direction is **unpromising** — but the precise claim
is narrower than "structurally dead." Do not implement OSPI; the measured hot
non-wins (conservative no win, aggressive negative) justify that. The branch's
real open problem is the cold cost (now understood as the always-on tax +
dep-capture, not "recording" — see §10.7 + `eval-trace-perf-cold-and-hot.md`),
which is orthogonal.

## 10.7 Calibration (2026-06-02, third adversarial pass) — "structurally dead" OVERSTATES the evidence

A third pass (user: "are you sure?") found that §10's "measured-dead, structural"
verdict accepted §16–§18's *conclusion* without scrutinizing its *evidence* — the
same uncritical-acceptance error that §10 itself called out for the pre-Fix-1a
14×. Distinguishing measured from inferred:

> **CORRECTION (2026-06-02, 4th pass — independent reviewer, unanchored).** Two
> errors in this section's earlier draft, both me over-reaching to support the
> verdict. The VERDICT (unpromising, not structurally-dead) was confirmed correct;
> these fix the stated *reasons*: (i) the "306 edges → 12,186 loads ⇒ internally
> inconsistent" sub-argument is STRUCK (it misreads §17 — see the CONFOUNDED block).
> (ii) the "+6.7s COLD recording cost" was wrongly called the *decisive* reason —
> the verdict rests on the measured HOT non-wins; the cold figure is flagged stale
> by the live perf doc (see below).

**MEASURED (solid):**
- Conservative §3b: **hot-neutral** (1.09s vs 1.00s baseline, §16) — i.e. NO hot
  win. Its only point was edge amortization, which the conservative shape doesn't
  do, so it is pointless.
- As-built AGGRESSIVE (**mis-keyed** gate, 306 edges): **hot-negative 1.81s**
  (§17), driven by `loadTrace.count` 431→12,186 — ~12K separate producer-trace
  blobs (SQLite read + zstd-decompress) loaded on warm verify, which inline
  conservative avoids.

**PROJECTED, NOT measured (and CONTESTED — see UNMEASURED): do NOT lean the verdict on this.**
- loadTrace is **capped at distinct producers** (memo, §17b line 1248), so
  re-keying (more edges to the *same* producers) would not *increase* the load
  count → re-keyed hot *projected* ≈ 1.81s ≈ still worse. But that is ONE
  consideration; the competing one (high fan-out amortizes each per-producer load
  across many cheap L1-deduped walks) could make it net-positive. **OSPI *is* the
  re-keyed shape, and it was never benched** — so "the edge actively loses" is true
  only of the *mis-keyed* proxy, not established for OSPI itself.

**Why "unpromising" is still the right word (without overstating):** the closest
*measured* proxy (mis-keyed aggressive) is hot-negative; conservative gives no
win; and §3b adds a cold cost (below). That is enough for "don't invest without a
strong reason" — it is NOT "proven to lose." The decisive re-keyed measurement was
never run.

**COLD cost — real that §3b adds, but NOT the clean "decisive" number it was called:**
- §3b records producer traces the edge can't reduce, so it adds cold cost. BUT the
  "+6.7s recording" magnitude (§16's §3b cold delta) is **flagged STALE** by
  `eval-trace-perf-cold-and-hot.md` (its 2026-06-02 pass, line ~1015): store-write
  = **0.2s**, and cold is dominated by the **always-on tax (~3s) + dep-capture
  building the 607×-flattened deps (~3.3s)**, not "recording." §10.1–10.6 and the
  entry points called "+6.7s COLD recording cost" the decisive reason — that
  overstated a stale figure. The verdict does not need it.

**CONFOUNDED (propagated as fact in §10.1–10.6 and the entry points — WRONG):**
- §17b/§18's "97% of producers are singly-consumed → producer-consumer sharing
  is STRUCTURALLY ABSENT" uses `producerEdges`=306 (the **mis-keyed gate's** fire
  count, §5) as a fan-out proxy. 306 measures the keying bug, not fan-out. It
  **contradicts §11** (200–2000 consumers/derivation from leaf-dep duplication).
  §18's "structural catch-22" rests on this confound; "high-fan-out can't be
  targeted" is built on a fan-out number never cleanly measured.
  - **STRUCK (4th pass):** an earlier draft added a third leg — "306 edges →
    12,186 loads ⇒ internally inconsistent (deep interconnection)." That misreads
    §17:1209, which says the 12,186 loads are **≈1 per recorded producer trace**
    (~12,836 of them), NOT caused by the 306 edges (a producer trace is recorded
    per derivation regardless of edges). The confound stands on the two legs above;
    this leg is wrong.

**UNMEASURED (the decisive experiment, never run):**
- A *correctly-keyed* aggressive shape benched on hot, with a real per-producer
  fan-out histogram. §16–§18 *reasoned* re-keying would be worse; the competing
  considerations (capped loads ⇒ no rescue; vs. high fan-out + cheap L1-deduped
  walks ⇒ possible rescue) are only resolvable by measurement.

**Net:** the direction is unpromising — *measured non-positive on the two tried
shapes* (conservative no hot win; aggressive hot-negative) — so "don't invest"
stands. But "structurally dead because there is no sharing" is **not** established;
it is a confounded inference. The honest indexing is "non-positive on tried shapes;
the re-keyed shape is unmeasured; §17b/§18's no-sharing reason is confounded; §3b
also adds a cold cost (magnitude unsettled — not the +6.7s once cited)." Do not let a future pass either (a) re-derive OSPI thinking the only
problem was keying, or (b) trust "no sharing" and chase a high-fan-out workload —
both are traps the confound sets.
