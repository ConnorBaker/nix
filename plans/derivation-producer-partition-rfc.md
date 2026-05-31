# RFC: Sound partition of a derivation's input-reads from ambient consumer deps

Status: DRAFT (2026-05-31). Supersedes the §3b "conservative shape" as the
forward design. Prerequisite reading: `plans/content-addressed-trace-identity-rfc.md`
(§3b, the producer-trace boundary) and `doc/eval-trace-cache-redesign-plan.md`
follow-ups #4–#8 (why the as-built §3b is default-off and why the value/coercion
re-keyings were refuted).

All mechanism claims are cited to code at `file:line` or to a committed test /
reproducer. Where this RFC corrects earlier in-tree prose, it says so explicitly.

## 0. One-paragraph summary

§3b's measured net loss (#4) and the two refuted re-keyings (#5 value channel,
#6/#7/#8 coercion channel) leave exactly one way for a derivation producer trace
to *replace* (not duplicate) the 607× flattening: isolate the derivation's
input-reads into a producer sub-scope at the `derivationStrict` args-force
boundary, and have each consumer record a single edge to the producer trace
instead of the flattened closure. That isolation is the previously-reverted
"aggressive shape." This RFC's job is to make the isolation **sound** — which
requires solving one concrete problem, now backed by a reproducer: **a
derivation forces input-reads that do NOT fold into its `drvPath`**, so a
producer trace keyed by `drvPath` alone would drop them and stale-serve. The
RFC specifies what the producer trace must be keyed/hashed by so that isolation
loses nothing the conservative shape keeps.

## 1. Why this is the only remaining shape (the funnel)

Each prior attempt to get *benefit* (edge replaces flatten) out of §3b was
measured or code-traced to failure. Summarizing so this RFC doesn't re-walk them:

| Attempt | Channel | Outcome | Evidence |
|---|---|---|---|
| §3b as-built (conservative) | record producer ALONGSIDE flattened consumer deps | net loss; gate fires ~614 vs ~6,419 records/commit | redesign-plan #4 |
| sibling-share workload | replay gate on re-forced producer Value | E:P ≈ 0.03 flat, worse than baseline; gate keyed to `strict` attrset siblings never re-force | #5 + `ca-producer-sibling-firerate.sh` |
| value-channel re-key | key producer on consumed Value identity | scalars/strings have no Value trace-identity (Tier-1 spike, dead) | spike `2ca68c498`, #5 |
| coercion-channel re-key | edge at `coerceToContextObject` output-string read | flatten is force-of-args-driven, NOT output-read-driven → edge is additive not replacing | #6/#7 RETRACTED by #8 + `DrvInput_FlattenSite_ForceVsOutputRead` (cw case) |

The flatten happens at `forceAttrs(*args[0])` + the per-attr force loop inside
`prim_derivationStrict` (primops.cc:1602 + the `lexicographicOrder` loop at
:1747). Confirmed by the cw discriminator (#8): a consumer that forces the args
deeply but reads no output string still carries the input `FileBytes`. **The only
site at which an edge can replace that flatten is a dep-capture sub-scope around
that force.** That is the aggressive shape. So the design space has funneled to:
"make the aggressive shape sound," and nothing else.

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
  sound). Under isolation that would be the producer sub-scope, but the producer
  trace's identity (`drvPath`) and content (`{drvPath, outputs}`) do not reflect
  the dropped attr — so the dep is lost at the producer boundary too.

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

The key insight vs §3a/§3b: **`drvPath` is the routing/dedup key; the
`trace_hash` over the recorded sub-scope deps is the verification key.** §3b
conflated them by assuming drvPath-identity implies dep-set-identity. §2's
reproducer shows it doesn't. Decoupling routing from verification is the fix.

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

**Severity: MEDIUM, needs measurement.** The trigger is two derivations sharing a
drvPath but recording different deps — which §2 shows is reachable via
`__ignoreNulls`-dropped file-backed attrs (nixpkgs `mkDerivation` default). How
often this occurs on a real workload is UNMEASURED. Mitigation if measured to
matter: make the routing key finer than `drvHash` — fold a digest of the recorded
dep set into the interned `__ca:` name (`internName` accepts arbitrary strings,
RFC §3a), so `aaa` and `bbb` route to distinct CurrentNode rows and neither
over-invalidates the other. This trades a small key-space increase for precision;
it does not affect soundness either way. Open item §7.6.

## 4. What this buys (the benefit §3b couldn't deliver)

Under §3b conservative, the consumer kept the flattened closure AND a producer
was recorded → strictly more work, the measured net loss. Under this design the
consumer keeps ONE edge instead of the flattened producer closure:
- The 607× shared-closure duplication collapses to 1 edge per consumer per
  producer (the `dep-flattening-baseline.cc` "before" this is designed to move).
- `resolveTraceContextHash` verifies each producer ONCE per session and memoizes
  (verifier.cc:255-276 + `verifiedTraceIds` early-out at :1786) — the build-layer
  `hashDerivationModulo`+`drvHashes` amortization lifted to eval, already proven
  reachable by `ca-trace-key-routing.cc` R2.
- Storage drops: producers are content-addressed/deduped; each carries its own
  input deps once instead of N copies in N consumers.

This is the first shape where the producer trace REPLACES consumer work rather
than adding to it — the precondition for any net win.

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
  consumer's flattened closure is REMOVED, so net storage/record work should drop
  rather than double. Must measure cold storage + record time vs the Ledger-D
  baseline.
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
   forced-then-dropped value's dep lands in the producer sub-scope. Must verify the
   `__ignoreNulls` force at primops.cc:1796 happens AFTER the sub-scope is pushed
   and that its dep isn't diverted (e.g., by a `PublicationWarmupScope` or
   replay-publish path). UNVERIFIED.
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
6. **Routing-key aliasing precision cost (§3b, Attack D).** Same-drvPath producers
   with different recorded dep sets share a `__ca:<drvHash>` routing row and can
   over-invalidate each other (sound, but spurious misses). Measure frequency on a
   real workload; if it matters, fold a dep-set digest into the routing key. This
   is the one finding from adversarial pass #1 that the original draft missed
   entirely — it is precision-only, but should be measured before claiming the
   design delivers a NET win (spurious misses eat into the amortization benefit).

## 8. Recommended sequence (each gated by the standing soundness suite)

1. Land the §7.1 red test (`__ignoreNulls` dropped-read invalidation) — pure test,
   no production change. Establishes the obligation.
2. Answer §7.2 + §7.3 by characterization tests (where do dropped/ambient deps
   land relative to a hypothetical sub-scope) — pure measurement.
3. ONLY if 1–2 show the partition is cleanly definable: throwaway prototype of the
   sub-scope + trace_hash-keyed producer + consumer edge, behind an env gate,
   default-off. Measure §6/§7.4 on Ledger-D. Go/no-go.
4. If go: wire the facet gate (§5), turn the §7.1 red test green, run the full
   suite + byte-identical gate, and only then consider default-on.

## 9. What this RFC does NOT claim
- NOT that the hot-path cost is acceptable — §6 is unmeasured and is the go/no-go.
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
