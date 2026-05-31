# RFC: Sound partition of a derivation's input-reads from ambient consumer deps

Status: DRAFT (2026-05-31). Supersedes the §3b "conservative shape" as the
forward design. Prerequisite reading: `plans/content-addressed-trace-identity-rfc.md`
(§3b, the producer-trace boundary) and `doc/eval-trace-cache-redesign-plan.md`
follow-ups #4–#8 (why the as-built §3b is default-off and why the value/coercion
re-keyings were refuted).

All mechanism claims are cited to code at `file:line` or to a committed test /
reproducer. Where this RFC corrects earlier in-tree prose, it says so explicitly.

> **Numbering convention (two distinct sequences — do not conflate).** `RP#N`
> refers to `doc/eval-trace-cache-redesign-plan.md` "follow-up #N" (the §3b arc,
> RP#4–RP#8). "pass #N" / "Attack X" refers to THIS RFC's own adversarial-review
> passes (recorded in its git commit series). Earlier drafts used a bare `#N` that
> ambiguously straddled both; this pass (the RFC's own fresh-eyes pass) disambiguated
> them.

## 0. One-paragraph summary

§3b's measured net loss (RP#4) and the two refuted re-keyings (RP#5 value channel,
RP#6/#7/#8 coercion channel) leave exactly one way for a derivation producer trace
to *replace* (not duplicate) the 607× flattening: isolate the derivation's
input-reads into a producer sub-scope at the `derivationStrict` args-force
boundary, and have each consumer record a single edge to the producer trace
instead of the flattened closure. That isolation is the previously-reverted
"aggressive shape." This RFC's job is to make the isolation **sound** — which
requires solving one concrete problem, now backed by a reproducer: **a
derivation forces input-reads that do NOT fold into its `drvPath`**. Verification
must therefore be keyed by the producer's recorded dep-set hash (`trace_hash`), NOT
by `drvPath` — a producer *verified* by `drvPath` alone would drop those reads and
stale-serve; routing MAY use `drvPath` because routing granularity doesn't affect
soundness (§3, the verifier recomputes-and-compares the trace_hash). The RFC
specifies that decoupling so isolation loses nothing the conservative shape keeps.

## 0.5 Settled position (the current conclusion, without re-executing the pass history)

The numbered sections carry an inline adversarial-pass trail ("pass #N found X,
pass #M corrected it") — deliberately, as the audit record. For a reader who wants
only the current settled state:

- **Design:** open a dep-capture sub-scope at `prim_derivationStrict` entry
  (the "aggressive shape"); finalize it as a producer trace; route by `drvPath`,
  **verify by `trace_hash`** over the recorded sub-scope deps; the consumer records
  one `TraceValueContext` edge instead of the flattened closure.
- **Soundness: CLOSED.** No new hole vs the conservative shape (§3 + §7.3 dichotomy,
  passes #2/#3; design positively confirmed by pass #13/Attack Q — `computeTraceHash`
  is over the dep vector, so `__ignoreNulls`-dropped reads fold in). Routing key is
  `drvPath`, determinate; do NOT fold deps into it (pass #12).
- **The blocker is PERF, not soundness. §8 step 0's first probe gave a FALSE STOP
  (retracted); the corrected measurement shows the sharing premise HOLDS (2026-05-31).**
  A probe at `prim_derivationStrict` reported "85.9% singly-consumed → STOP" — INVALID:
  the primop runs ONCE per derivation (memoization), so it counted only first-forcers
  and was blind to the memoized-thunk sharing the RFC targets (redesign-plan follow-up
  #9 retraction; a re-hook at `replayMemoizedDeps` ALSO failed a controlled case because
  the re-forced Value is the output STRING, not the derivation attrset — the probe was
  removed). The faithful measurement is a UNIT test through the real
  `makeCache`/`TracedExpr` consumer machinery
  (`dep-flattening-baseline.cc::SharedDrv_SameOutPath_BothConsumersFlattenInputClosure`):
  one shared derivation, two consumers reading the SAME `.outPath`, BOTH traces
  independently carry its input closure (`cxHasFile=1 cyHasFile=1`). So **the 607×
  consumer-sharing is REAL** — `replayMemoizedRange` flattens a shared derivation's
  closure into EVERY consumer trace, decisively at unit scale. §7.7b premise HOLDS at
  unit scale (#10) AND at WORKLOAD scale (#11): re-deriving over the architecture doc's
  already-decoded python3Packages numbers (SPA = 34.9% of 36.5M flattened deps ≈ 12.7M
  derivation-flatten-instances over ~thousands of distinct derivations) gives **mean
  ~200–2,000 consumers per derivation** — insensitive to the one unpinned input across
  its whole plausible range; N≫1 robustly. **So sharing is no longer open at all.** The
  SOLE remaining go/no-go is the args-force sub-scope HOT-PATH COST (§6), which needs the
  throwaway prototype (RFC §8 step 3) — a deliberate larger investment touching the hot
  eval path. See `doc/eval-trace-cache-redesign-plan.md` follow-ups #10 (mechanism) + #11
  (population).
- **Honest scope:** ALT-4 (verify-time fragment sharing, §7.8) remains a real
  alternative regardless.

## 1. Why this is the only remaining shape (the funnel)

Each prior attempt to get *benefit* (edge replaces flatten) out of §3b was
measured or code-traced to failure. Summarizing so this RFC doesn't re-walk them:

| Attempt | Channel | Outcome | Evidence |
|---|---|---|---|
| §3b as-built (conservative) | record producer ALONGSIDE flattened consumer deps | net loss; gate fires ~614 vs ~6,419 records/commit | RP#4 |
| sibling-share workload | replay gate on re-forced producer Value | E:P ≈ 0.03 flat, worse than baseline; gate keyed to `strict` attrset siblings never re-force | RP#5 + `ca-producer-sibling-firerate.sh` |
| value-channel re-key | key producer on consumed Value identity | scalars/strings have no Value trace-identity (Tier-1 spike, dead) | spike `2ca68c498`, RP#5 |
| coercion-channel re-key | edge at `coerceToContextObject` output-string read | flatten is force-of-args-driven, NOT output-read-driven → edge is additive not replacing | RP#6/#7 RETRACTED by RP#8 + `DrvInput_FlattenSite_ForceVsOutputRead` (cw case) |

The flatten happens at `forceAttrs(*args[0])` + the per-attr force loop inside
`prim_derivationStrict` (primops.cc:1602 + the `lexicographicOrder` loop at
:1747). Confirmed by the cw discriminator (RP#8): a consumer that forces the args
deeply but reads no output string still carries the input `FileBytes`. The only
site at which a RECORD-TIME edge can replace that flatten is a dep-capture sub-scope
around that force — the aggressive shape. So within the *record-time-edge* family,
the design space has funneled to "make the aggressive shape sound."

> **Scope honesty (adversarial pass #9, Attack M; CORRECTED by pass #10, Attack N —
> the funnel is NOT the whole design space, and the first correction over-reached).**
> The funnel above exhausts the RECORD-TIME-edge family (replace the flatten as it is
> recorded). It does NOT exhaust ways to attack the 607× cost. The notable
> alternative is **ALT-4: verify-time fragment sharing** — at VERIFY, recognize that
> consumers C1, C2 both contain stdenv's flattened sub-closure and verify it ONCE per
> session instead of re-walking it in all 607 consumers.
>
> Pass #9 claimed ALT-4 "needs no record-time change" and "this RFC is a prerequisite
> for ALT-4." Pass #10 (Attack N) found BOTH claims wrong — they were rationalizations
> to keep this RFC central:
> 1. **This RFC and ALT-4 are MUTUALLY-EXCLUSIVE ALTERNATIVES, not prerequisite +
>    dependent.** This RFC makes the consumer store an EDGE (no flattened closure);
>    ALT-4 keeps the consumer's flattened closure and dedups it at verify. If you do
>    this RFC, there is no flattened closure left for ALT-4 to dedup — ALT-4 is
>    MOOTED, not enabled. "A producer trace IS the verify-once fragment" conflated two
>    designs that store different things in the consumer.
> 2. **ALT-4 is NOT record-change-free.** Per the L-A refutation
>    (`perf-levers-cold-and-hot.md`: the global dep sort + positional `dep.ordinal`
>    mean a sub-closure is NOT a contiguous run in the flat vector), ALT-4 would need
>    the recorder to MARK fragment boundaries — a record-time change too, just not a
>    force-time sub-scope. So ALT-4 trades the §6 force-time-sub-scope hazard for a
>    different record-time change + verify-time fragment-matching cost; it does not
>    avoid touching the recorder.
> Honest standing: ALT-4 is a genuine, materially-different alternative (not refuted,
> not a mere prerequisite step) whose cost profile (record-mark + verify-dedup) vs
> this RFC's (force-time sub-scope) is unmeasured on both sides. This RFC pursues the
> record-time edge because it reuses the already-proven edge-verify machinery
> (`ca-trace-key-routing.cc`, `resolveTraceContextHash`) end-to-end, whereas ALT-4's
> verify-time fragment matcher is undesigned. That is a maturity argument, NOT a
> dominance argument. §7.8 records ALT-4 as a real competing direction to evaluate,
> not a fallback that this RFC unlocks.

## 2. The core soundness obligation (REPRODUCER, corrects the revert prose)

The §3b revert comment (primops.cc:1554-1568) says the aggressive shape
"under-records: any ambient-eval dep that legitimately flowed into the consumer
scope but isn't tied to the producer's content would be lost." That prose is
**imprecise** — it suggests the lost deps are "ambient" / not the producer's.
The real, sharper statement, with a reproducer:

> **A derivation's `derivationStrict` call forces input-reads that do NOT fold
> into its `drvPath`.** A producer trace keyed/hashed by `drvPath` alone would
> not encode them; isolating them into such a producer would drop them from the
> consumer, causing a stale serve.

**Reproducer (run against `result/bin/nix`, recorded here for regression):**

```nix
# config.nix: mkDerivation = args: derivation ({ system=…; builder=…; } // args);
let
  condStr = builtins.readFile ./cond.txt;
  optionalAttr = if condStr == "yes\n" then "present" else null;
in {
  drv = (import ./config.nix).mkDerivation {
    name = "nulltest";
    __ignoreNulls = true;          # nixpkgs mkDerivation sets this BY DEFAULT
    extra = optionalAttr;          # forced (reads cond.txt) then DROPPED when null
    buildCommand = "echo hi > $out";
  };
}
```

Measured: `cond.txt = "aaa"` and `cond.txt = "bbb"` both produce
`drvPath = /nix/store/m8gb8b44yz10kpllkr5prja1lpchjyz2-nulltest.drv` (identical).
But `derivationStrict` forced `extra` (to test it for null), recording a
`readFile(cond.txt)` dep — which `drvPath` does not encode, because
`__ignoreNulls` drops the null `extra` before it enters the derivation
(primops.cc:1795-1798: `if (ignoreNulls) { forceValue(*i->value); if (type==nNull) continue; }`).

Mechanism, cited:
- `__ignoreNulls` is read at primops.cc:1723-1727; when true, every attr value
  is forced at :1796 then dropped via `continue` at :1798 if null.
- nixpkgs `stdenv.mkDerivation` sets `__ignoreNulls = true` by default, so this
  is not a corner case — it is the dominant real-world shape.
- The forced-then-dropped value's deps land in whatever scope is active during
  the force. Under the conservative shape that is the consumer scope (kept,
  sound). Under isolation that would be the producer sub-scope. The hazard this
  motivates is specifically a producer keyed/verified by `drvPath` (or by its
  result payload `{drvPath, outputs}`): neither reflects the dropped attr, so such
  a producer loses the dep. NOTE (clarified pass #13, Attack Q): this RFC's design
  (§3) does NOT lose it — the producer's verification key is the `trace_hash` over
  its recorded DEPS (`computeTraceHash` over the dep vector, hash.cc:46-48), and the
  dropped-attr `readFile` IS one of those recorded deps, so it folds into the hash.
  The "dep lost at the producer boundary" failure is the motivation for rejecting
  drvPath-keying (§3), not a property of the RFC's trace_hash-keyed producer.

**This is the obligation the RFC must discharge:** the producer trace must be
keyed/verified by something that changes whenever ANY input-read it performed
changes — including reads that don't fold into `drvPath`. `drvPath` alone is
insufficient.

> NOTE — this also corrects a latent over-claim in
> `content-addressed-trace-identity-rfc.md` §3c and the
> `derivation-outpath-soundness.cc` framing, which argue "input change → different
> drvPath → invalidation." That is true for inputs that fold into drvPath, and
> `__ignoreNulls`-dropped reads are a measured counterexample. The existing
> soundness tests don't exercise the dropped-attr path. (Tracked as an open item
> in §7.)

## 3. The design: key the producer by its recorded input-dep set, not by drvPath

The fix follows directly from §2: do not key the producer trace by `drvPath`.
Key it by the **content hash of the dep set the producer sub-scope actually
recorded** — i.e. the existing trace-hash mechanism (`computeTraceHash` over the
sorted dep vector, `deps/hash.cc`), which is what every other trace is already
keyed/verified by.

Concretely, the producer-trace boundary becomes:

1. At `prim_derivationStrict` entry, open a dep-capture sub-scope (the isolation
   the aggressive shape did). `forceAttrs(*args[0])` + `derivationStrictInternal`
   record their input-reads into THIS scope, not the consumer's.
2. On success, finalize the sub-scope into a producer trace whose **own deps are
   exactly the recorded set** (including `__ignoreNulls`-dropped reads — they are
   in the sub-scope because the force happened inside it). The producer's
   `trace_hash = computeTraceHash(those deps)`.
3. Route/store it under a content-addressed key. `drvPath` is fine as the
   *routing* key (it dedups producers that ARE drvPath-distinct), BUT
   verification is by `trace_hash`, which folds in the dropped reads. Two
   derivations with the same drvPath but different recorded dep sets (the §2
   `aaa`/`bbb` case) get **different trace_hashes** → the consumer's edge
   (a `TraceValueContext` carrying the producer's `trace_hash`) mismatches when
   cond.txt changes → invalidation. Soundness restored.
4. The consumer records ONE `TraceValueContext` edge to the producer's
   `(routing key, trace_hash)` instead of the flattened closure. This is the
   exact edge mechanism already proven by `ca-trace-key-routing.cc` (R1/R2/R3)
   and verified recursively + memoized by `resolveTraceContextHash`
   (verifier.cc:243-278).
   **Why dropping the consumer's flattened closure is sound** (the load-bearing
   step — full argument in §7.3, dichotomy from passes #2/#3 Attacks F/G): every dep
   the consumer would have flattened lands in EITHER the producer sub-scope's
   `ownDeps` (→ the producer trace, carried transitively by the edge via the
   recursive `resolveTraceContextHash`) OR the durable global `epochLog_` (→ replayed
   into the consumer's own scope if the consumer independently re-forces the thunk —
   `popScope` doesn't touch `epochLog_`). There is no third bucket except the
   pre-existing no-active-context drop, which isolation does not widen. So the edge
   carries, or the consumer independently re-records, everything the closure held;
   the drop loses nothing the conservative shape kept. (Over-CAPTURE — a
   consumer-incidental dep trapped in the producer — is possible but is precision,
   not soundness; §7.3.)

The key insight vs §3a/§3b: **the routing/dedup key (`drvPath`) is SEPARATE from the
verification key (`trace_hash` over the recorded sub-scope deps).** §3b conflated
them by assuming drvPath-identity implies dep-set-identity. §2's reproducer shows it
doesn't. Decoupling routing (drvPath) from verification (trace_hash) is the fix.
Routing stays drvPath — coarsest, best dedup, and folding deps into it would
re-couple the two (pass #12, Attack P / the routing-key note below); the trace_hash
compare alone carries verification soundness regardless of routing granularity.

**Why decoupling is sound — the compare is stored-vs-recomputed (verified, adversarial pass #1 Attack C).**
The consumer's edge is a `TraceValueContext` dep whose stored hash is the producer
trace hash the consumer observed at record time. On warm verify, `runPass1`
(verifier.cc:579-586) calls `resolveTraceContextHash` to RECOMPUTE the producer's
current trace hash and compares: `matched = expected->value == *traceContextHash`
(verifier.cc:585). And `resolveTraceContextHash` does not trust a recorded hash —
it calls `verifyTrace(parentRow->traceId, …)` (verifier.cc:267), which "enters its
own CurrentTraceScope on the parent's fullDeps" and yields a hash that "encodes
hash(op(current F)) only after that trace proved current F == its recording of F"
(verifier.cc:260-264). So the producer's deps — including the `__ignoreNulls`-dropped
`readFile` — are RE-READ against the current filesystem on every consumer warm verify.
A changed `cond.txt` → producer re-verify recomputes a different hash → the
stored-vs-recomputed compare fails → consumer invalidates. The decoupling holds
because verification never relies on the routing key matching the content; it
always recomputes the content hash and compares to what the consumer stored.

### 2-vs-3 worked example (the `aaa`/`bbb` reproducer under the new design)
- `cond.txt=aaa`: producer sub-scope records `readFile(cond.txt)=hash(aaa)`;
  `trace_hash_A = H(…, readFile=hash(aaa))`. Consumer edge carries `trace_hash_A`.
- Edit to `bbb`: re-eval, producer records `readFile(cond.txt)=hash(bbb)`;
  `trace_hash_B ≠ trace_hash_A`. Consumer's stored edge (carrying `trace_hash_A`)
  fails `resolveTraceContextHash` → consumer invalidates. SOUND.
- Same drvPath both times (routing key stable) → producer dedups correctly across
  siblings, but verification still distinguishes the two dep sets. Both properties
  hold simultaneously — which is exactly what §3b couldn't do.

### 3b. Routing-key aliasing is a PRECISION cost, not a soundness hole (adversarial pass #1 Attack D)

The §2 reproducer exposes a subtlety the naive design misses: `aaa` and `bbb` have
the IDENTICAL `drvPath`, hence the IDENTICAL `__ca:<drvHash>` routing key, but
DIFFERENT recorded dep sets (`readFile=hash(aaa)` vs `hash(bbb)`). The producer's
trace BODY is content-addressed (`getOrCreateTrace` by `fullHash`,
sqlite-trace-storage.cc:546-548) so the two produce distinct `Traces` rows. But the
ROUTING row (`CurrentNode` keyed by `(session_key, pathId)`,
sqlite-trace-storage.cc:558-571) maps the one `caKey` → one `traceId`, and is
OVERWRITTEN on each record (:562 "replaced on current-state publication"). So within
a session that evaluates both an aaa-consumer and a bbb-consumer, the `caKey`'s
CurrentNode points at whichever producer was recorded LAST.

Consequence, verified by case analysis (NOT a soundness hole):
- aaa-consumer's stored edge holds `hash_aaa`. If `caKey` currently routes to the
  bbb-producer, `resolveTraceContextHash` re-verifies the bbb-producer and returns
  `hash_bbb ≠ hash_aaa` → the aaa-consumer MISSES and re-evaluates. That is
  **over-invalidation (precision loss), not a stale serve.**
- If the on-disk `cond.txt` has actually changed under the aaa-consumer, the
  producer re-verify (Attack C, re-reads current FS) recomputes against the new
  content and the stored-vs-recomputed compare fails → MISS. Also sound.
- In no branch does a consumer serve a value computed against a dep set that no
  longer matches the filesystem. Soundness is preserved by the recompute-and-compare
  in §3; only precision suffers from the routing alias.

**Severity: LOW (downgraded from MEDIUM by pass #12, Attack P), needs measurement.**
The trigger is two same-drvPath-different-deps derivations BOTH evaluated in ONE
session (cross-process, the warm consumer's edge points at a routing row a different
variant last wrote). How often is UNMEASURED, but pass #12 showed the consequence is
milder than first stated: even when the routing alias misdirects, the routed
producer is RE-VERIFIED against the current FS (Attack C), so it returns nullopt
unless its recorded deps match disk — i.e. the consumer gets a sound MISS and
re-records correctly. The alias costs at most a spurious miss in the rare
both-variants-coexist session; it does not persist.
~~Mitigation: fold a dep-set digest into the `__ca:` routing key.~~ **REJECTED (pass
#12, Attack P):** folding the trace_hash into the routing key RE-COUPLES routing and
verification — the exact thing §3 decouples. It would make the routing pathId
unstable across content changes and rely on lookup-miss instead of the clean
hash-compare, and it is architecturally at odds with the design's core principle for
no soundness gain (both are sound). The correct stance: keep drvPath-only routing;
the trace_hash compare (Attack C) already distinguishes the variants on verify. The
aliasing is a rare, non-persisting precision blip, not something to re-couple the
architecture to fix. Open item §7.6 (now: measure whether it's even worth caring
about, NOT a mitigation to build).

> **Routing key = drvPath, full stop (adversarial pass #7 raised a granularity
> tension; pass #12 Attack P RESOLVED it).** Pass #7 framed a three-way tension —
> §3 wants coarse drvPath routing, §3b wanted a finer `H(drvPath, dep-set-digest)` to
> cut aliasing, §6 wants the coarsest key for dedup — and called granularity a
> measured tuning knob. Pass #12 collapsed the tension: the finer-key option is
> SELF-DEFEATING (folding the trace_hash into the routing key re-couples routing and
> verification, the exact thing §3 decouples — Attack P), and it buys nothing because
> the trace_hash compare already distinguishes same-drvPath variants on verify
> (Attack C) and the residual aliasing is a rare non-persisting precision blip (§3b,
> downgraded to LOW). So there is no real tension: **route by drvPath (coarsest, best
> dedup, §6-optimal); verify by trace_hash (§3); do NOT fold deps into the routing
> key.** The earlier "granularity is a tuning parameter" framing is withdrawn — the
> parameter has a determined value.

## 4. What this buys — IF the consumer-sharing exists (the benefit §3b couldn't deliver)

Every benefit in this section is CONDITIONAL on producers being consumed by N>1
distinct consumers per eval (§7.7, the master go/no-go, UNMEASURED; RP#5 measured the
as-built gate firing ~0). Stated as the mechanism that WOULD deliver the win where
sharing exists, not as a delivered win — §4 is the design's promise, §6/§7.7 are the
unmet measurement bar.

Under §3b conservative, the consumer kept the flattened closure AND a producer was
recorded → strictly more work, the measured net loss. Under this design the consumer
keeps ONE edge instead of the flattened producer closure, SO WHERE a producer is
shared by N>1 consumers:
- The 607× shared-closure duplication collapses to 1 edge per consumer per
  producer (the `dep-flattening-baseline.cc` "before" this is designed to move).
  For singly-consumed producers (N=1) it is a small net add, not a collapse (§6).
- `resolveTraceContextHash` verifies each producer ONCE per session and memoizes
  (verifier.cc:255-276 + `verifiedTraceIds` early-out at :1786) — the build-layer
  `hashDerivationModulo`+`drvHashes` amortization lifted to eval, proven REACHABLE by
  `ca-trace-key-routing.cc` R2. "Reachable" ≠ "triggers on the workload": whether a
  consumer at the args-force boundary actually edges to a SHARED producer is exactly
  the §7.7b open question (RP#5's as-built gate did not).
- Storage drops IN AGGREGATE because the 607× is dominated by high-N shared infra
  (§6 refinement); per singly-consumed producer it is a small add.

This is the first shape where the producer trace COULD replace consumer work rather
than add to it — the precondition for any net win, contingent on §7.7. If §7.7
measures sharing ~1 at the args-force boundary, this section's benefits do not
materialize and the design reduces to §3b's net loss (§9).

## 5. Facet gate (unchanged, still binds)

`derivation-observation-facets.cc` measured that `(drv).outPath` records only
`StorePathAvailability(.drv)` + system (output-only), so an edge replacing the
input closure drops nothing the consumer observed — IF the consumer's observation
of the derivation is output-only. A consumer that reads a non-output facet
(`drv.meta`, `drv.passthru`, a file-backed `meta=fromJSON(readFile …)`) records
its OWN deps in the consumer scope (NOT inside `derivationStrict`), so those stay
on the consumer and are unaffected by the producer isolation. The edge is additive
to any such facet dep, never a replacement. This gate is the same one §3b
specified; it is enforced at the consumer's edge-recording site, and is
mechanically checkable from the recorded dep set (a recorded facet dep is the
signal the observation was not output-only).

> **Load-bearing assumption made explicit (adversarial pass #4, Attack I).** "Facet
> reads happen OUTSIDE `derivationStrict`" is NOT an intrinsic property of the
> primop — `derivationStrictInternal`'s attr loop (primops.cc:1747) forces EVERY
> attr of the arg attrset (the default case at ~:1902 `coerceToString`s unknown
> attrs into `drv.env`). If `meta`/`passthru` were passed in the arg attrset, they
> WOULD be forced inside `derivationStrict` and their reads trapped in the producer.
> The gate is sound only because the Nix-level wrapper STRIPS them: nixpkgs
> `mkDerivation` builds `derivationArg = makeDerivationArgument (removeAttrs attrs
> ["meta" "passthru" …])` (make-derivation.nix:828-831), and the in-tree test shim
> does the same — `derivation (… // removeAttrs args ["builder" "meta"]) // { meta
> = args.meta or {}; }` (tests/functional/config.nix.in:29-30) — re-attaching
> `meta` only on the OUTER package attrset. So facet reads land in the consumer
> scope via the outer attrset, as §5 claims. CAVEAT: a hand-written
> `derivationStrict`/`derivation` call that passes `meta` directly (bypassing
> `mkDerivation`) would force the facet inside the boundary → over-capture into the
> producer. That is still SOUND (the facet read becomes a recorded producer dep, so
> a change invalidates via producer verification; the gate's "recorded facet dep ⇒
> not output-only" detection still fires) — it is a precision cost, not a hole. But
> the gate's CLEAN output-only case relies on the `removeAttrs` convention, which a
> prototype must not assume holds for arbitrary `derivation` callers.

## 6. Blast radius / cost (must be measured, not assumed)

- A new `DepCaptureScope` per `derivationStrict` call (~10,455/closure measured,
  spike). The vptr-in-hot-loop reversal (rearch §2.1) is the standing warning:
  hot-path structure changes can regress even when logically sound. This is
  per-derivation (~10⁴), not per-thunk (~10⁷), so bounded — but UNMEASURED and
  the deciding number, exactly as the original §7 slice was.
- Producer-trace recording cost: same `recordSync` cost as §3b, but now the
  consumer's flattened closure is REMOVED. **CORRECTION (adversarial pass #5, Attack
  J): net storage drops ONLY for multiply-consumed (N>1) producers.** The arithmetic:
  conservative today = N consumers × full closure (the 607×). Proposed = 1 producer
  closure + N edges + 1 producer routing row (`Sessions` + `Traces` + `Results` rows,
  sqlite-trace-storage-lifecycle.cc:170-191). For N>1 this is a clear win
  (closure + N·edge ≪ N·closure). For **N=1 (singly-consumed derivation) it is a
  small net ADD** — closure + 1 edge + producer routing rows vs just the consumer's
  flattened closure. So per-producer, "storage drops" is CONDITIONAL on N>1.
  **REFINEMENT (adversarial pass #6, self-check on Attack J — the pass #5 edit was
  too pessimistic):** in AGGREGATE storage clearly drops, because the consumer-
  sharing distribution is bimodal and the high-N side dominates. The 607× figure
  (architecture-trace-model-vs-CA.md: 36.5M flattened vs ~60K distinct = 99.8%
  duplication) IS producer-sharing for shared-infra derivations: a stdenv file
  appearing in 607 package traces means stdenv-the-producer is consumed by N≈607.
  Removing that duplication removes ~99.8% of the flattened bytes; Attack J's N=1
  net-add applies only to the ~0.2% distinct tail (leaf packages nothing builds on).
  So **net storage drops** — Attack J was directionally right (N=1 is a per-item add)
  but I overstated it to "doesn't drop." Corrected.
  CRUCIAL distinction this refinement must NOT blur: storage-drop ≠ net-PERF-win. The
  607× says the bytes are shared; it does NOT say the §4 verify-amortization or the
  edge-emission actually TRIGGER at the args-force boundary. RP#5 measured the as-built
  gate fired ~0 (E:P) on this exact workload because re-forces hit the wrapper not the
  `strict` attrset. Whether moving the edge to args-force makes the amortization fire
  is the genuinely open §7.7 question; the storage arithmetic does not answer it.
- The isolation changes consumer trace SHAPE (fewer leaf deps, one edge). This is
  a precision-neutral change ONLY if §3's trace_hash argument holds and the facet
  gate (§5) is correct. The standing correctness gate (byte-identical `nix eval`)
  + the §7 soundness suite are the guardrails.

## 7. Open problems / what must be proven before building

1. **The §2 dropped-read obligation — DONE as a committed test (pass #2 Attack E).**
   `DrvIgnoreNullsDroppedRead_ChangeInvalidatesConsumer`
   (`store/derivation-input-flattening.cc`) pins it end-to-end: `__ignoreNulls`
   drops a file-backed `extra` attr; `cond.txt` "aaa"→"bbb" keeps drvPath identical
   but the consumer re-records (`calls==1`). Verified the conservative shape is
   SOUND for this case TODAY (not a pre-existing bug — Attack E ruled that out: the
   readFile dep IS kept in the consumer scope). This is now the red obligation any
   producer-ISOLATION prototype must keep green: keying the producer by drvPath
   alone loses this read. Still unverified FOR the isolation path (no prototype yet)
   — verified for the conservative path by construction + this test.
2. **Does the sub-scope actually capture the dropped-attr read?** §3 assumes the
   forced-then-dropped value's dep lands in the producer sub-scope. PARTIALLY VERIFIED
   by inspection (pass #7, Attack K): nothing between `prim_derivationStrict` entry
   and the `__ignoreNulls` force (primops.cc:1602 `forceAttrs` → :1796 per-attr force)
   pushes or swaps a recording scope, so a sub-scope pushed at primop entry WOULD be
   the active scope at the force, and reads route to it. RESIDUAL (needs the
   prototype): whether `forceValue`'s internal machinery (`PublicationWarmupScope`,
   the replay-publish path in `forceThunkValue`) diverts the dep to a different scope
   for the specific values forced here. Inspectable structurally but cleanest to
   confirm with the throwaway prototype's counters.
3. **Deps forced inside the sub-scope that aren't producer-intrinsic — RESOLVED to
   PRECISION, not soundness (pass #2 Attack F + pass #3 Attacks G/H).**
   CORRECTION to an earlier draft assumption: the arg attrset is NOT WHNF before
   `prim_derivationStrict`. Primops receive raw `Value*` (eval.cc:2445 dispatches
   `fn->impl(...args.data()...)` without forcing), so `forceAttrs(*args[0])`
   (primops.cc:1602) is the FIRST force — meaning the attrset-construction work (the
   `mkDerivation` `//` merge, `with pkgs;` lookups) runs INSIDE the would-be producer
   sub-scope, not before it. So those deps DO fire inside the boundary. Measured
   (Attack H): a `readFile(which.txt)` used in the consumer's `//` merge fires inside
   `forceAttrs`; two contents that produce the identical drv (`my1sjwlvz…`) both
   record it into the boundary.
   But this is a PRECISION cost, not a soundness hole, by the following dichotomy
   (Attack G). Every dep forced inside the sub-scope lands in exactly one of:
   - **`ownDeps`** (the producer sub-scope's) → the producer trace, verified by its
     `trace_hash` (which folds in ALL its deps, drvPath-folding or not — §3). A change
     to any such dep → producer re-verify (re-reads current FS, Attack C) → producer
     hash changes → consumer edge mismatch → consumer invalidates. SOUND. Covers both
     Case 1 (folds into drvPath) and Case 2 (`__ignoreNulls` dropped-read, §2) and the
     Attack-H merge-dep.
   - **`epochLog_`** (the durable global log) → replayable. If the consumer
     independently re-forces the thunk, it replays the range into the consumer scope
     (Attack F: `popScope`/`takeDeps` at dep-recording-context.hh:348-372 don't touch
     `epochLog_`). SOUND for the shared-thunk case.
   - **neither** → dropped. This is the PRE-EXISTING `nrDepRecordNoActiveContext` path
     (recording.cc:404-412): fires only when NO `DepRecordingContext` is active
     (manual/internal evals), independent of any sub-scope. A producer sub-scope is
     itself a `DepCaptureScope` on the stack, so in-boundary reads route to it, not to
     nothing. Isolation introduces no NEW drop here; whatever this path loses today,
     the conservative shape loses identically.
   Net: isolation introduces **no new soundness hole** — every in-boundary dep is
   either in the producer trace (sound via producer verification) or replays to the
   consumer (sound) or was already dropped pre-isolation. What isolation DOES introduce
   is **over-capture**: a purely-consumer dep (e.g. the Attack-H merge `readFile`) gets
   trapped in the producer trace, so a change to it over-invalidates the producer (and
   thus its consumers) even though no consumer's OWN observation changed. That is a
   precision cost in the same family as §3b/Attack D, bounded and measurable, NOT a
   correctness problem. The residual question is therefore quantitative (how much
   over-capture on a real workload), folded into §7.6 + §6, not a soundness blocker.
4. **Hot-path cost (§6).** The one number that decides go/no-go. Needs a throwaway
   prototype + Ledger-D bench, exactly the §7 slice shape.
5. **Nested derivations.** A buildInput is itself a derivation forced inside the
   outer `derivationStrict`. Its input-reads would land in the OUTER producer
   sub-scope unless the inner `derivationStrict` opens its own nested sub-scope.
   `ca-producer-boundary-recording.cc` P4 proved nested producer chains verify;
   must confirm the sub-scope nesting composes (inner producer edge inside outer
   producer trace). UNVERIFIED for this isolation shape.
6. **Routing-key aliasing precision blip (§3b, Attack D; downgraded by pass #12,
   Attack P).** Same-drvPath-different-deps producers sharing a `__ca:<drvHash>`
   routing row can cause a spurious consumer miss IN the rare session where both
   variants coexist — but it doesn't persist (the misdirected producer is re-verified
   vs FS and the consumer re-records correctly, Attack C). The pass-#1 "fold a dep-set
   digest into the routing key" mitigation is REJECTED (Attack P: re-couples routing
   and verification for no soundness gain). Remaining work is only to MEASURE whether
   the blip is frequent enough to matter on a real workload — NOT a mitigation to
   build. Almost certainly negligible; listed for completeness, not as a blocker.
7. **Does the amortization actually TRIGGER at the args-force boundary? (pass #5
   Attack J, refined by pass #6.)** Two sub-questions, now separated:
   - (a) STORAGE sharing — ANSWERED by the existing 607× measurement (§6 refinement):
     shared-infra producers have N≈607, the flattened total is 99.8% duplication, so
     replacing it with edges drops aggregate storage. Not the blocker.
   - (b) AMORTIZATION TRIGGERING — OPEN and the real go/no-go: does a consumer at the
     args-force boundary actually emit an edge to a SHARED producer (so verify runs
     once, §4), or does each derivation get its own producer with no cross-consumer
     reuse? RP#5 showed the as-built gate fired ~0 because re-forces hit the wrapper;
     this RFC edges at args-force, which is a DIFFERENT site, but whether that site
     is reached once-per-distinct-producer (good) or once-per-consumer-occurrence
     (no reuse) is UNMEASURED. If the latter, the design adds the producer-record cost
     without the verify saving — §3b's net loss in a new costume.
   **Cheap pre-prototype measurement:** instrument the conservative recorder to count,
   per producer drvPath, how many distinct consumer traces flatten its closure
   (Ledger-D + python3Packages). High N for infra derivations confirms the SHARING
   exists; then the only remaining question is whether the args-force edge CAPTURES
   that sharing, which needs the prototype. If even the conservative-recorder sharing
   count is ~1 across the board, STOP.
8. **ALT-4: verify-time fragment sharing — a competing direction, not a fallback
   (pass #9 Attack M, corrected by pass #10 Attack N).** Keep conservative flattened
   recording but mark shared sub-closure boundaries at record time, then at VERIFY
   dedup so a shared closure is verified once per session. This is MUTUALLY EXCLUSIVE
   with this RFC (this RFC removes the consumer's flattened closure, which ALT-4 needs
   to dedup), NOT a step this RFC unlocks. Its cost profile — a record-time
   fragment-boundary mark (it still touches the recorder; the flat global dep vector
   has no fragment boundaries today, the L-A blocker) plus a verify-time
   fragment-matcher (undesigned) — trades this RFC's force-time-sub-scope hot-eval
   hazard (§6) for verify-path work. Neither side is measured. This RFC pursues the
   record-time edge for MATURITY (it reuses proven edge-verify machinery —
   `ca-trace-key-routing.cc`, `resolveTraceContextHash`) not DOMINANCE. A serious
   evaluation should prototype-cost both before committing. UNDESIGNED on the ALT-4
   side; this is the most important "what else" the RFC leaves open.

## 8. Recommended sequence (each gated by the standing soundness suite)

0. **CHEAPEST KILL-SWITCH FIRST (§7.7) — but on the RIGHT workload, with the RIGHT
   metric (corrected by a pre-build adversarial check, pass #18).**
   WORKLOAD: measure on **python3Packages, NOT Ledger-D/closures.gnome.** closures.gnome
   is a single coarse string-leaf trace (~6,419 derivations forced inside ONE
   `evaluateResolvedTarget` scope, RP#8), so "distinct consumer traces per drvPath"
   reads ~1 there as a SHAPE ARTIFACT, not evidence of no sharing — and closures.gnome's
   cost is outlier re-eval, not flattening anyway. The 607× flattening this RFC targets
   is a python3Packages phenomenon (~10K distinct attr-path traces each flattening shared
   stdenv); that is where the metric is meaningful.
   METRIC PITFALL: "the DepCaptureScope active at `derivationStrict`" is NOT a faithful
   consumer identity by itself — the active scope can be one coarse leaf even when many
   logical consumers share a producer (the same conflation that made closures.gnome
   misleading, applied per-package). The faithful metric is "how many distinct attr-path
   CONSUMER TRACES have the derivation's closure flattened into them," which is NOT
   recoverable from the unmodified recorder (flattened deps carry no source-derivation
   tag). So step 0 needs EITHER (a) source-tagging flattened deps with their producing
   drvPath (more instrumentation than "just a counter"), OR (b) a proxy: count, per
   drvPath, the distinct attr-path TracedExpr scopes whose `evaluateResolvedTarget`
   transitively triggered that `derivationStrict` — which requires threading the
   current-consumer-pathId and is only faithful if derivations are forced under their
   consuming package's child scope (must verify, not assume). Either way step 0 is
   "small measurement-only instrumentation," NOT "just a counter" as earlier drafts
   said. If the faithful sharing ratio is ~1, STOP. This gates everything below.
1. Land the §7.1 red test — DONE (`DrvIgnoreNullsDroppedRead_ChangeInvalidatesConsumer`).
2. Answer §7.2 (does the dropped read land in the sub-scope?) by characterization
   test — pure measurement, no production change.
3. ONLY if 0 shows N>1 sharing (on python3Packages) AND 2 confirms capture: throwaway
   prototype of the sub-scope + trace_hash-keyed producer + consumer edge, behind an
   env gate, default-off. Measure the §6/§7.4 hot-path COST on Ledger-D/closures.gnome
   (the established hot anchor — the sub-scope-per-derivationStrict overhead shows up
   there regardless of sharing) AND the net win on python3Packages (where the sharing
   benefit, if any, materializes). §7.6 aliasing frequency is almost certainly
   negligible (pass #12) — measure only if cheap. Go/no-go.
4. If go: wire the facet gate (§5, respecting the §5 removeAttrs caveat), keep the
   §7.1 red test green, run the full suite + byte-identical gate, and only then
   consider default-on.

## 9. What this RFC does NOT claim
- NOT that there is a net PERF win — storage clearly drops (§6, 607× is dominated by
  high-N shared infra), but whether the §4 verify-amortization TRIGGERS at the
  args-force edge is open (§7.7b): RP#5 showed the as-built gate fired ~0. Storage-drop
  does NOT imply perf-win; the two are separate and only the latter is the blocker.
- NOT that the hot-path cost is acceptable — §6 is unmeasured.
- NOT that drvPath is the verification key — §2 disproves that; drvPath is routing
  only, trace_hash is verification.
- NOT that precision is preserved — the design has TWO measured over-capture /
  over-invalidation sources (§3b/§7.6 routing-key aliasing; §7.3 in-boundary
  consumer-dep capture). Both are SOUND (over-invalidate, never stale-serve) but
  eat into the amortization win, so the NET benefit is unproven pending §6/§7.6
  measurement.
- It DOES claim (and these are now grounded, not aspirational):
  - The soundness obligation is concrete and reproducible (§2) and pinned by a
    committed test (`DrvIgnoreNullsDroppedRead_ChangeInvalidatesConsumer`).
  - The design discharges it by decoupling routing (drvPath) from verification
    (trace_hash over the recorded sub-scope deps), and that decoupling is sound by
    the recompute-and-compare in the existing verifier (§3, Attack C).
  - Isolation introduces **no new soundness hole** vs the conservative shape — every
    in-boundary dep is covered by producer verification, consumer replay, or was
    already dropped pre-isolation (§7.3 dichotomy, Attacks F/G/H). The earlier
    "ambient-vs-intrinsic boundary may not be cleanly separable" worry is RESOLVED:
    it need not be separated for SOUNDNESS; separation only buys PRECISION.
  - The remaining unknowns are the hot-path cost (§6/§7.4) and the magnitude of the
    two precision costs (§7.6) — all quantitative, gated, and testable. There is no
    remaining open SOUNDNESS question; the blocker is now "is the net perf win real,"
    which only a prototype + Ledger-D bench answers.
