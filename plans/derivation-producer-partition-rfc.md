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
  its whole plausible range; N≫1 robustly. So the SHARING premise is settled.
  PROXY measurements taken (NO end-to-end aggressive shape was built — two proxies +
  synthesis; framing corrected in follow-up #19; #12–#18):
  - **Sharing/benefit: ~63% of consumer deps are derivation-attributable** (consumer
    flattens mean ~3,296 deps referencing ~1,213 distinct derivations → an edge model
    keeps ~1,213 edges = ~63% dep-COUNT/storage reduction). SOLID (artifact/test-based,
    #10/#11/#13). NOT the ~99% briefly claimed (retracted #15); above the 37% SPA floor.
    CAVEAT (#19): this is a STORAGE/count number — the VERIFY-time (hot) win is the
    cross-consumer memo dedup (verifiedTraceIds), reachable but UNMEASURED in magnitude.
  - **Structural cost ~11%** (per-derivation sub-scope, proxy: open+discard; a 23×
    re-record proxy was caught + discarded #14). A LOWER bound; real adds §3b recordSync.
  Net: NOT measured end-to-end. The direction is net-negative at synchronous recording
  (it pays §3b's same dominant recordSync cost) — so a real go/no-go needs TWO builds,
  neither done: (a) the end-to-end edge-emitting prototype measured on a HOT workload
  (the verify-memo benefit magnitude), and (b) async/batched producer recording (the
  cost; §3b condition a). See follow-ups #12–#19.
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

## §9. Aggressive edge-recorder — GROUNDED IMPLEMENTATION DESIGN (2026-05-31, code-verified)

Written after reading the actual record/replay/snapshot code (not the isolate-and-discard research
probe). Supersedes the §3-step-4 hand-wave about "where the consumer records the edge".

### Mechanism facts (verified file:line)
- `DepRecordingContext::record` (dep-recording-context.cc:62/128): a dep is appended to BOTH the
  global `epochLog` AND the active consumer scope's `ownDeps` **iff** it passes the scope's `seenDeps`
  dedup (`observeRecordedDep`). Same dep, same guard, both buckets.
- `prim_derivationStrict` hook (primops.cc:1605/1686): `epochStart = currentReplayEpochSize()` before
  the force; after `derivationStrictInternal`, `innerDeps = snapshotEpochRange(epochStart, epochEnd)`
  (context.hh:327) is the producer's deps; `recordCAProducer(v, drvPath, innerDeps)` persists the
  producer trace whose `trace_hash` folds in EXACTLY `innerDeps` (recordSync → computeTraceHash over
  that vector). Today the consumer scope KEEPS those same deps flattened (conservative shape).
- `Dep::operator==` (types.hh:1013) compares KEY ONLY (not hash). `Dep::Key::Hash` exists (types.hh:977).
- Edge dep: `Dep::makeValueContext(caKey, traceHash)`, recorded via `TraceAccess::current()->record(...)`
  — the exact shape the replay gate already emits (context.cc:1059).
- Facets (`meta`/`passthru`) live on the OUTER `drv // {meta=…;}` attrset, created AFTER
  derivationStrict returns → forced in the CONSUMER scope OUTSIDE [epochStart,epochEnd) → NOT in
  `innerDeps` (pinned by derivation-observation-facets.cc::DrvOutPath_DoesNotForceSiblingMeta).

### The two builds
**B1 — cold first-consumer edge.** After `recordCAProducer` succeeds, record ONE
`makeValueContext(caKey, traceHash)` into the consumer's active scope. Without this the cold first
forcer (which hits `forceThunkValue`, never the replay gate) records no tie to the producer → the
aggressive shape would stale-serve every singly-forced derivation. (RFC §3-step-4 omitted this site.)

**B2 — filter (flatten-replace).** Remove from the consumer scope's `ownDeps` exactly the deps whose
KEY is in `innerDeps` (build a `Key`-set from the producer snapshot, erase matching ownDeps). The edge
then REPLACES the flattened closure instead of adding to it. This is the benefit.

### Why filter-by-key-set is sound (the subtlety the revert missed)
A key in `innerDeps` was first-seen INSIDE the window → by the dedup guard it was NOT already in the
consumer's `ownDeps` before `epochStart` (if it had been, the in-window record would be dedup-blocked
and absent from the epoch range). So erasing ownDeps-by-key-∈-innerDeps removes ONLY deps the producer
range covers. The producer `trace_hash` folds in `innerDeps`; the edge's verification
(`resolveTraceContextHash` → recursive `verifyTrace` of the producer → recompute trace_hash, compare)
re-checks exactly the removed set against the live FS. Remove-set == producer-trace-hash-covered-set →
sound. The reverted "isolate-and-discard" shape removed the deps and re-attached them NOWHERE (no
producer trace) — THAT was the leak, not the removal itself.

The `__ignoreNulls` dropped-read is in `innerDeps` (forced inside the window, primops.cc:1825) → folded
into trace_hash → the edge invalidates on it. drvPath-routing never enters the soundness decision.

Facet gate is AUTOMATIC: facet reads are outside the window → not in `innerDeps` → never filtered →
stay on the consumer (the C3-hole guard). A facet forced INSIDE derivationStrict (non-nixpkgs
`derivation{meta=readFile…}`) lands in `innerDeps` → folded into trace_hash → filtering it is still
sound (edge re-verifies), only a precision loss.

### Implementation surface
- New `DepRecordingContext::replaceWindowWithEdge(const std::vector<Dep> & producerInnerDeps, const Dep
  & edge)`: build a `Key`-hash-set from producerInnerDeps; erase current-scope `ownDeps` whose key is
  in it; push the edge (subject to the scope dedup). Encapsulates the ownDeps mutation in its owner.
- `recordCAProducer` returns the `{caKey, traceHash}` (or success+out-params) so the primop can build
  the edge.
- primop hook: when the (new) aggressive sub-gate is on AND the producer recorded, call
  `access.depRecordingContext().replaceWindowWithEdge(innerDeps, edge)`.
- Gate: a sub-flag of NIX_ENABLE_CA_PRODUCER (e.g. NIX_CA_PRODUCER_AGGRESSIVE=1) so the conservative
  shape stays the default-on-when-§3b-enabled behaviour and the aggressive shape is independently
  switchable for A/B. Both default OFF.
- Composition: runs on Layer 1+2a deferred recording (the producer recordSync already threads
  deferFlush); getCurrentTraceHash reads in-memory caches so the edge's traceHash is available.

### Soundness pins (tests, B-tasks)
- cold edge present + flat closure absent (the B1 site).
- marker-in-args invalidation (the reproduced stale-serve, now must pass).
- cross-producer memoized-thunk routing (two distinct drvs sharing a thunk → both invalidate).
- facet stays on consumer (nixpkgs `// {meta}` shape).
- no-under-record differential: set of files whose mutation invalidates the consumer under aggressive
  ⊇ under conservative (never fewer). Over-invalidation allowed, counted.
- functional eval-trace-core / eval-trace-deps stay green (the suites the revert failed).

## §10. Aggressive edge-recorder — IMPLEMENTED + BLOCKED on a recovery-resolution gap (2026-05-31)

Implemented §9 B1 (cold first-consumer edge at `recordCAProducer` finalize) + B2 (filter consumer
`ownDeps` by producer key-set), gated behind `NIX_CA_PRODUCER_AGGRESSIVE=1` (independent of, but
implying, `NIX_ENABLE_CA_PRODUCER`). Both default OFF — shipped behaviour unchanged (functional flakes
+ impure suites green with no env; 404 unit tests pass).

### What works (validated end-to-end)
- **Soundness on the reproduced revert case.** The `marker`-in-`args` derivation (readFile folds into
  drvPath) that STALE-SERVED under the old isolate-and-discard shape now correctly INVALIDATES under
  the real edge-recorder (`NIX_ALLOW_EVAL=0` refuses the stale value after the marker changes), and
  HITS when unchanged. The edge's recursive producer verify re-checks `innerDeps` against the live FS.
- **The filter fires + shrinks the consumer.** Direct DB inspection: the consumer trace's `keys_blob`
  shrinks under aggressive (e.g. 43→27 bytes), removing the producer's `StorePathAvailability` deps and
  adding one `TraceValueContext` edge. B2 is real, not a no-op.
- **Impure (`-f`, non-flake) suites pass** under full aggressive.

### The blocker (empirically isolated, code-confirmed): edge resolution is NOT recovery-aware
The **flake** suites `eval-trace-core` / `eval-trace-deps` FAIL under aggressive (both pass
conservative). Symptom: editing an UNRELATED sibling's input (`b-data.txt`) over-invalidates `drvA`
(`drvA` does not read `b-data.txt`). It fails CLOSED (over-invalidation, never a stale serve) — sound
but precision-destroying.

**Bisected to B1, not B2:** `NIX_CA_PRODUCER_EMIT_ONLY=1` (emit the edge, keep flattened deps, NO
filter) ALSO fails the flake suites. So the bug is the EDGE itself, not the dep removal.

**Root cause (confirmed by reading verifier.cc:243-278):** a flake-source edit changes the flake
fingerprint → the eval's **session key changes** → the warm eval is a different session. The consumer
trace (`drvA`) recovers across that boundary via **History bootstrap** (its `stableRecoveryKey` is
source-identity-based, stable across the edit). But the consumer's `TraceValueContext` edge to the
`__ca:<drvHash>` producer is resolved by `resolveTraceContextHash`, which does ONLY
`lookupCurrentNode(producerKey)` + `verifyTrace` — it calls NONE of `scanHistory` /
`lookupLatestHistoryForAttr` / `recovery` (verifier.cc:251,267). The producer's CurrentNode lives
under the OLD session key, so `lookupCurrentNode` returns `nullopt` in the new session →
`resolveTraceContextHash` returns `nullopt` → the edge can't resolve → the consumer misses. The
conservative shape is immune because it keeps the flattened deps, each of which recovers via History
independently; the edge collapses N deps into one resolution path that has no recovery fallback.

Impure suites pass because `-f` non-flake eval keys the session on the absolute file path, which does
NOT change when a sibling data file is edited — so no session-key rotation, no recovery needed, the
producer's CurrentNode is found directly.

### What closing it requires (NOT a quick fix — a design extension)
The `__ca:<drvHash>` producer trace needs to be **recovery-resolvable** across session-key changes:
`resolveTraceContextHash` must fall back to a History/recovery lookup for the producer (keyed by a
stable producer recovery key — e.g. `__ca:<drvHash>` is already content-stable, so a History row keyed
on it would bootstrap correctly). This is the same recovery machinery the consumer already uses, lifted
to the edge-resolution path. Until that lands, the aggressive shape is unusable for flake eval (the
dominant real workload). The conservative shape + Layer 1/2a async recording remains the shippable
state; the aggressive recorder stays in tree as gated scaffolding (mirroring the §3b conservative
disposition).

### Disposition
Aggressive shape: IMPLEMENTED, SOUND, gated default-OFF, BLOCKED on recovery-aware edge resolution.
The earlier "soundness is the only closed gate, perf is the blocker" framing was incomplete: there is
ALSO a precision/recovery-correctness gap that only manifests under session-key rotation (flake edits),
which neither the microcase nor the impure suites exposed. Found by running the functional suites the
original revert failed — the right oracle. Next step is the recovery-aware producer resolution above,
THEN the hot bench (§9 task 5) — benching now would measure a shape that breaks flake re-eval.

## §11. Adversarial pass on §9/§10 (2026-05-31) — root cause CONFIRMED by observation, fix VALIDATED viable, with caveats

Re-examined the §10 implementation, my approach, and the proposed fix. The §10 root cause was an
inference from code-reading; this pass CONFIRMED it by direct instrumentation + DB inspection, and
surfaced concerns the inference glossed.

### Finding 1 [CONFIRMED, was inferred]: the edge fails at H1, not H2/H3.
Instrumented the three failure branches of `resolveTraceContextHash` (verifier.cc:251/267) with
distinguishing warns, ran the failing flake test in the harness. Result: **`H1 lookupCurrentNode(parent=20)
NULL`** fires (twice — drvA + the other consumer); H2 (verifyTrace fail) and H3 (hash null) never fire.
So the edge is unresolvable because the producer's CurrentNode is absent in the warm session — NOT
because the producer verify fails or the hash mismatches. My §10 story holds at the symptom level.

### Finding 2 [CONFIRMED]: session-key rotation is the cause; the producer IS in History under a STABLE key.
DB inspection of the failing run's cache (Sessions + History):
- Sessions has **3 distinct session_keys** (record / warm-A / warm-B evals); the producer (attr_path 20)
  has a Sessions row under **exactly one** (`F67A5FA7…`). The warm eval that failed runs under a
  different key → `lookupCurrentNode(20)` misses. Session-key rotation CONFIRMED (not just inferred).
- The producer IS in **History** (count=1), and — decisively — it shares the **same stableRecoveryKey
  (`89745B…`) as its consumers** (attr_paths 18/19/20/27/… all `89745B…`). The two recovery keys in
  History are the test's TWO repos (core-basic vs core-fine-grained), not a per-eval rotation.
- ⇒ A recovery-aware `resolveTraceContextHash` calling `lookupLatestHistoryForAttr(20)` with the
  consumer's `currentStableRecoveryKey()` WOULD find the producer's History row. **The proposed fix is
  viable — validated against the artifact, not just plausible.** History PK is
  `(recovery_key, attr_path_id, trace_id)` and the producer's attr_path is the per-drvHash
  `__ca:<drvHash>`, so the lookup identity is correct granularity.

### Finding 3 [NEW CONCERN the §10 fix-proposal missed]: the producer's recovery key is the CONSUMER's source identity, not the producer's content.
The producer's History `recovery_key` is whatever session recorded it FIRST — i.e. the source-identity
recovery key of the flake that first forced that derivation. Two soundness/precision questions the fix
must answer BEFORE implementation:
- (a) **Cross-flake collision:** two different flakes producing a byte-identical derivation (same
  `__ca:<drvHash>`) would write History rows under DIFFERENT recovery keys (different sources) — so a
  consumer in flake X looking up by X's recovery key finds only X's producer row. SAFE (correct
  separation), but it means the producer trace is NOT shared cross-flake (a minor amortization loss,
  not unsound).
- (b) **Same-flake rev drift:** flake source rev R1 records producer P@R1 (recovery_key K, since
  stableRecoveryKey is source-identity-stable across revs of the same flake). At rev R2, if the
  derivation is unchanged (same drvHash) it's NOT re-recorded (recordCAProducer dedups on Bindings*
  per-session, and cross-session the History row under K persists) — consumer at R2 bootstraps P@R1 via
  K. The producer trace's innerDeps were captured at R1; verifyTrace re-checks them against R2's live
  FS. SOUND (recompute-and-compare), but if R1's innerDeps referenced an R1-specific store path that R2
  changed, the producer verify correctly fails → consumer re-evals. Correct, possibly imprecise. NEEDS
  A TEST (the cross-rev producer-reuse case).

### Finding 4 [APPROACH CRITIQUE]: I over-relied on hand-rolled repros that didn't match the oracle.
I burned significant effort on dir-flake hand-repros that DON'T reproduce the harness's caching
(TEST_HOME/NIX_STORE_DIR isolation, no substituters) — they missed even under conservative, and an
inserted `eval-info` probe PERTURBED session state into a spurious pass. Only the meson functional test
is faithful. LESSON (reinforces the standing one): for cache-state-dependent behaviour, instrument the
REAL oracle in-place (warns in the code path, DB inspection of the harness's own cache dir) — do not
build a parallel repro whose caching you haven't proven equivalent.

### Finding 5 [IMPLEMENTATION审, minor]: the dedup-hit edgeOut path is correct but untested.
`recordCAProducer` populates `edgeOut` from the side-table on the dedup-hit branch
(trace-session.cc:849) so a SECOND cold consumer of the same producer in one session still emits its
edge. This path is real (the 607× sharing case) but has no unit pin — if it regressed to leaving
`edgeOut` default, the second consumer would emit an edge to caKey=0 (root) and mis-resolve. Add a pin.

### Finding 6 [SCOPE, was already known but worth restating]: soundness is preserved throughout.
Every failure mode observed is fail-CLOSED (over-invalidation / cache miss), never a stale serve. The
H1-null edge yields `std::nullopt` → consumer miss → fresh eval. So shipping the aggressive shape
default-OFF carries zero soundness risk to the default path; the blocker is purely
precision/cache-effectiveness on flake re-eval.

### Synthesized next steps (revised, in dependency order)
1. **Recovery-aware producer-edge resolution (the fix).** In `resolveTraceContextHash`, when
   `lookupCurrentNode(parent)` returns null, fall back to `lookupLatestHistoryForAttr(parent)` (the
   producer's History row under the consumer's stableRecoveryKey), then `verifyTrace` that traceId +
   publish a CurrentNode for it (mirroring the consumer's history-bootstrap in `verify()`,
   verifier.cc:1921-1937). Gated to the aggressive path. This is ~the same bootstrap the consumer
   already does, lifted to the edge.
2. **Tests, BEFORE re-enabling:** (a) the functional flake suites must pass under aggressive (the
   oracle that found the bug); (b) cross-rev producer-reuse pin (Finding 3b); (c) dedup-hit edgeOut pin
   (Finding 5); (d) the no-under-record differential property (§9). 
3. **Only then** the hot bench (§9 task 5). Benching before the fix measures a flake-broken shape.
4. **Open the cross-flake amortization question (Finding 3a)** as explicitly out-of-scope-for-now
   (correctness is fine; it's a sharing-ceiling note).

### Disposition unchanged: aggressive shape IMPLEMENTED, SOUND, gated default-OFF, BLOCKED — but the
blocker is now a CONFIRMED, well-characterized, fixable gap (recovery-unaware edge resolution) with a
validated fix path, not an open mystery.

## §12. Recovery-aware producer-edge resolution — IMPLEMENTED + TESTED (2026-05-31)

Landed the §11-step-1 fix. The §10 blocker (aggressive shape over-invalidates on flake re-eval) is
CLOSED.

### The change (verifier.cc `resolveTraceContextHash`)
When `lookupCurrentNode(parentPathId)` returns null (the parent — typically a `__ca:<drvHash>`
producer — has no CurrentNode in this session, because a source edit rotated the session key), fall
back to `lookupLatestHistoryForAttr(parentPathId)` (the parent's History row under the current
stableRecoveryKey). On a successful nested `verifyTrace`, re-publish the parent's CurrentNode
(`publishStateChange(insertHistory=false)`) so this session — and the `traceContextMemo` — resolve it.
This MIRRORS `verify()`'s own history-bootstrap (verifier.cc ~1920-1937), lifted to edge resolution.

UNCONDITIONAL (not gated to the aggressive path): it also benefits sibling-`TraceValueContext` /
`TraceParentSlot` edges on the default path, and was verified to NOT regress the default path. The fix
increments `nrHistoryBootstraps` when it fires.

### Why it is sound
The bootstrapped parent is still fully VERIFIED: the nested `verifyTrace` recomputes the parent's deps
against the live FS, so a stale bootstrapped producer fails verify exactly as a current-session one
would → the consumer's edge resolves to nullopt → invalidate. History bootstrap changes only WHERE the
candidate trace is found (CurrentNode vs History), never WHETHER it's checked. Confirmed by the
soundness test below (producer-input-changed still invalidates after bootstrap).

### Validation
- **The oracle:** `eval-trace-core` + `eval-trace-deps` (the flake suites that FOUND the blocker) now
  PASS under `NIX_CA_PRODUCER_AGGRESSIVE=1`. Default path: all 16 eval-trace functional suites green;
  786 eval-trace unit tests green.
- **New unit pins (`store/ca-producer-edge-recovery.cc`, 2 tests):**
  `CAProducerEdge_SessionKeyRotation_ResolvesViaHistoryBootstrap` reproduces session-key rotation via
  `SessionConfig::forTest(<distinct policy>, <shared stable key>)` (the recovery.cc cross-session
  pattern) — a producer+consumer-edge recorded in session 1, verified in session 2 (rotated key),
  must hit ONLY because the producer bootstraps from History. **NON-VACUITY PROVEN:** reverting the fix
  makes this test FAIL (the other soundness test still passes — it guards that the fix doesn't break
  invalidation). `CAProducerEdge_SessionKeyRotation_ProducerInputChanged_Invalidates` pins soundness
  (bootstrapped producer with a changed input still invalidates).
- **EdgeOut pin (`dep/trace-session-record-ca-producer.cc` R5):** `recordCAProducer` populates the
  `{caKey, traceHash}` out-param on BOTH the fresh-record AND the per-session dedup-hit paths (Finding
  5 — a 2nd cold consumer of a shared producer). **NON-VACUITY PROVEN:** breaking the dedup-hit
  population makes R5(b) FAIL (it would otherwise emit an edge to caKey=0/root and mis-resolve).

### Still open (deferred, in dependency order)
1. **Cross-rev producer-reuse test (Finding 3b).** The session-key-rotation tests cover the rotation;
   a dedicated same-flake-rev-drift test (producer recorded at rev R1, reused at R2 with the
   derivation unchanged, then R2 changes the producer's input) would pin the recompute-and-compare on
   the bootstrapped producer end-to-end. The R5 + soundness tests cover the mechanism; this is
   additional coverage, not a gap in soundness.
2. **No-under-record differential property (§9).** A differential oracle (files-that-invalidate under
   aggressive ⊇ under conservative) — now buildable since the flake suites pass.
3. **Hot bench (§9 task 5).** NOW MEANINGFUL — the aggressive shape no longer breaks flake re-eval, so
   a Ledger-D hot measurement would measure a correct shape. This is the next major step.

### Disposition: the §10 blocker is CLOSED. Aggressive shape: implemented, sound, default-OFF, flake
re-eval WORKING. Remaining is perf validation (the hot bench) + the two additional test coverages.

## §13. HOT BENCH — the go/no-go measurement (2026-05-31). VERDICT: aggressive shape is a NET LOSS; direction does not pay off on this workload.

Now that the §10 blocker is closed (flake re-eval works under aggressive), the hot bench is meaningful.
Ran the Ledger-D anchor (`closures.gnome`, nixpkgs-release suite) at 25 commits (pilot scale; machine had
concurrent other work, so leading with medians + the component breakdown, not absolute wall), three
configs in an ISOLATED results root (`--nix .aggr-bench`, own result-symlink → fix binary 458e271, so
no collision with prior runs), SEQUENTIAL (no timing contention), `--with-stats` (uniform overhead
across configs → fair relative comparison). Env-passthrough validated first (producerEdges=614 fires
through the harness).

### Wall time (median per commit, ×reference)
| config | cold median | hot median | hot ×ref |
|---|---:|---:|---:|
| reference (no-trace) | 6.79s | — | — |
| no-§3b (cache on) | 1.01s | **0.97s** | 0.14× |
| §3b conservative | 7.94s | 7.95s | 8.2× |
| §3b **AGGRESSIVE** | 16.98s | **16.42s** | **16.9×** |

**The aggressive shape is ~2× WORSE than conservative on BOTH cold and hot, and ~17× worse than
no-§3b on hot.** It does not beat materialize-time re-execution — it makes it worse.

### Why (component breakdown, hot, median per commit)
| counter | no-§3b | conservative | AGGRESSIVE |
|---|---:|---:|---:|
| record.count | 0 | 8,448 | **18,906** |
| record.timeUs | 0 | 0.77s | **1.71s** |
| verify.timeUs | 0.37s | 0.50s | **1.08s** |
| root hits / misses | 7 / 0 | 5 / 2 | **3 / 4** |

The smoking gun: on the HOT path the aggressive shape **records 18,906 traces** (more than conservative's
8,448, vs 0 for no-§3b) and suffers MORE root cache misses (4 vs 0). The edge-emit + producer-record +
(now recovery-aware) edge-resolution all fire DURING materialize-time re-execution — the derivation
thunks re-run on warm serve (the §3b/#23 hot pathology), and the aggressive machinery adds cost on top
of that re-execution instead of replacing it. verify.timeUs also doubles (the History-bootstrap edge
resolution is not free).

### This CONFIRMS the §11/#25 prediction, empirically
The hot regression was always going to be dominated by materialize-time re-execution, NOT by the
flattened-dep verify cost the edge reduces. The edge collapses storage/dep-count (the ~63% number) and
the verify-walk per-item cost (M≈1) — but those are NOT the hot bottleneck on this workload. The hot
bottleneck is that warm-served consumers RE-RUN derivation thunks during materialize, and every re-run
re-fires the producer hook. The edge makes each re-run MORE expensive (emit + filter + bootstrap), not
cheaper. "63% storage reduction" never translated to a hot win here — exactly the conflation trap
flagged in #25/§B.

### SOUNDNESS: PASS (the fix holds at scale)
All 25 commits × {cold, hot} × aggressive produce byte-identical eval output to the no-trace reference
("All outputs match reference"), with producerEdges=614 confirming the gate fired. The recovery-aware
edge resolution (§12) is sound at workload scale — the §10 blocker is genuinely closed. The verdict is
purely PERFORMANCE.

### VERDICT (go/no-go): NO-GO for the aggressive shape on derivation-dense flake re-eval.
The producer-partition direction, as built, is a net loss on the Ledger-D anchor — worse than the
already-net-loss conservative §3b, because it adds cost to the materialize-time-re-execution path that
dominates hot cost. The direction's benefit (storage dedup + verify-walk reduction) is real but
addresses a cost that is NOT the bottleneck on this workload. To ever pay off, the PREREQUISITE is
eliminating materialize-time re-execution of derivation thunks on warm serve (so the producer hook
fires ~once, not ~thousands of times per hot eval) — that is a separate, larger evaluator change
(suppress-on-warm-served, or async/batched producer recording that doesn't block the eval thread), and
until it lands, NO producer-trace shape (conservative or aggressive) is net-positive hot. The aggressive
edge-recorder + recovery fix stay in tree as gated, default-OFF, SOUND scaffolding (mirroring §3b
conservative's disposition), now with a measured hot cost so the next person doesn't re-run this.

### Caveats (honest scope)
- 25-commit pilot, not the documented 100-commit run; medians are stable across the 25 but absolute
  numbers are this-machine/this-rev with concurrent load. The 2× aggressive-vs-conservative and 17×
  vs-no-§3b ratios are far larger than any plausible noise, so the VERDICT is robust; a 100-commit run
  would refine magnitudes, not flip the sign.
- `--with-stats` adds uniform overhead; the relative ratios are unaffected. A `--with-stats`-free run
  would lower all absolute numbers but not the ordering.
- Data + harness: `.aggr-bench/eval-trace-bench-results/{reference,cold,hot}-stats/{1,2,3}`. Reproduce:
  the three `generate` invocations in §13's preamble (isolated root + the two env vars for run 3).

### §13b. Adversarial pass on the §13 verdict — mechanism CORRECTED (the NO-GO stands, stronger)

Before trusting §13, checked whether the bench measured the REAL aggressive shape or "conservative +
dead edge code." Two findings that refine (and strengthen) the verdict:

**Finding A — the filter BARELY changes stored deps on this workload.** Final cold DB: conservative
12,987 traces / 3,300,644 keysblob bytes; aggressive 12,986 / 3,306,944 (aggressive marginally
LARGER). So B2's dep-removal produces ~zero net storage benefit here. Root cause: the producer gate
fires on only ~3% of traces (cold producerEdges=614 vs record.count=18,906) — the §3b/#5 finding that
on `closures.gnome`, consumers force the `commonAttrs //`-wrapper / string `outPath`, NOT the keyed
`strict` value, so the edge rarely replaces anything. The aggressive benefit mechanism is structurally
near-absent on this workload (independent of the §10 fix).

**Finding B — the §13 stated mechanism was IMPRECISE; corrected.** Aggressive records 2.24× MORE
traces than conservative (cold record.count 18,906 vs 8,448; record.timeUs 1.81s vs 0.76s) yet the
final DB has the SAME trace count. The extra ~10,458 record calls are NOT edges (producerEdges only
+308). Mechanism: `replaceWindowWithEdge` mutates consumer `ownDeps` → changes consumer trace HASHES →
consumers that deduped against each other / prior-commit traces under conservative now hash distinct →
~2× more distinct record() calls (which then dedup down in the final DB, but the RECORD WORK is already
paid). So §13's "edge adds cost to the re-execution path" is right in direction but the dominant term
is actually FILTER-INDUCED RECORD-VOLUME INFLATION (cold) + edge-resolution verify cost (hot), not the
edge-emit itself.

**Net effect on the verdict: UNCHANGED and STRENGTHENED.** The aggressive shape pays ~2× the cold
record cost and ~2× the hot cost for a storage/edge benefit that is structurally near-absent on
`closures.gnome` (gate fires ~3%). It is strictly worse than conservative here with ~no offsetting
benefit. NO-GO confirmed, and the reason is now precise: not "the edge is expensive" but "the edge
rarely fires on this workload (the §5 keying problem) AND the filter inflates record volume by
perturbing consumer trace identity." 

**Crucial scope correction this surfaces:** the gate-fires-only-3% problem is the SAME §3b follow-up #5
keying issue (producer keyed on `strict`, consumers force the wrapper) — it was never fixed, and the
aggressive shape inherits it. So this bench does NOT prove "producer-partition can't work"; it proves
"producer-partition built on the CURRENT gate keying doesn't fire enough on closures.gnome to pay for
itself." A workload where consumers DO re-force the keyed value (or a re-keying per #5's "register
identity on the .outPath string / wrapper attrset") could change the benefit side — but that is the
SAME unbuilt re-keying prerequisite §3b already identified, now with a measured confirmation that
without it the edge is inert. The materialize-time-re-execution point from §13 still holds as the hot
ceiling; this adds that even the cold/storage benefit is gated on re-keying.

## §14. ROOT CAUSE of the hot regression — found, validated in code + artifact (2026-05-31)

The §13 NO-GO verdict said "the edge adds cost to materialize-time re-execution." A deeper adversarial
pass (decomposing the hot wall against internal timers) found that was a SYMPTOM, not the cause. The
real cause is a **fixable correctness defect in §3b producer recording**, not an inherent cost.

### The decomposition that cracked it
Hot wall vs eval-trace internal timers (median/commit, 25-commit bench):
| config | wall | record+verify+verifyTrace | UNACCOUNTED (= re-eval) | verify.failed (per-dep) | root hits/misses |
|---|---:|---:|---:|---:|---:|
| no-§3b | 0.97s | 0.73s | 0.24s (24%) | **0** | 7/0 |
| conservative | 7.95s | 1.65s | **6.30s (79%)** | **364** | 5/2 |
| aggressive | 16.42s | 5.06s | **11.36s (69%)** | **25,780** | 3/4 |

no-§3b serves `closures.gnome` hot in ~1s with ZERO dep-verify failures (uniform 0.95–1.01s across all
25 commits — pure cache serve, no re-eval). Enabling §3b introduces hundreds-to-tens-of-thousands of
PER-DEP verify failures (`nrVerificationsFailed`, verifier.cc:625/660/706/734), which cascade to trace
misses → fresh re-eval → the 6–11s of UNACCOUNTED wall. **§3b is DEFEATING the cache, not just adding
overhead.**

### The mechanism (subagent diagnosis, INDEPENDENTLY VALIDATED against code + the bench DB)
The failing dep kind is `TraceValueContext` — the producer EDGE itself, not a perturbed consumer dep
(grep confirmed 0 Normal-dep failures; all failures are `__ca:<drvPath>` edges).

Root defect = TWO violated invariants, both confirmed:
1. **`snapshotEpochRange(epochStart,epochEnd)` (primops.cc:1695, context.hh:327) is NON-DETERMINISTIC
   per caKey.** It returns a slice of the GLOBAL epoch log during the derivationStrict call window —
   NOT the derivation's own input closure. Same drvPath forced when inputs are fresh → captures inputs
   (non-empty); forced when inputs are already memoized → grows the log by nothing (EMPTY). The
   primops.cc:1681 comment already admits the range is a non-reproducible superset.
2. **`recordCAProducer` dedups on `Bindings*`, NOT caKey (trace-session.cc:845).** A re-forced
   derivation gets a fresh `Bindings*` → dedup misses → `recordSync(caKey,…,innerDeps)`
   (trace-session.cc:860) RE-RUNS with a different (often empty) innerDeps → overwrites the caKey's
   CurrentNode last-writer-wins (Traces are content-addressed, so all empty-range recordings collapse
   to ONE zero-dep trace).

Consumers embed the FIRST (real, non-empty) producer trace_hash in their edge; warm
`resolveTraceContextHash` resolves the caKey's CURRENT CurrentNode = the overwritten EMPTY trace →
hash mismatch → edge FAILS → consumer invalidates → re-eval.

**ARTIFACT VALIDATION (I verified in the bench's own hot-stats/2 DB, not just the subagent's word):**
`SELECT t.id, COUNT(*) n, LENGTH(d.keys_blob) FROM Sessions s JOIN Traces t … GROUP BY t.id ORDER BY n
DESC` → **trace_id 12, keysblob_len=0 (zero-dep), is the CurrentNode for 30,629 Sessions rows.** One
empty producer trace became the routing target for 30K+ producer keys via last-writer-wins overwrite.
Exactly the predicted collapse. Dedup-on-Bindings* confirmed at trace-session.cc:845.

### This REFRAMES the §13 NO-GO: the hot regression is a DEFECT, not an inherent cost.
§13 concluded "producer-partition can't pay off because materialize re-execution dominates." Corrected:
the materialize re-execution is CAUSED by §3b's own unverifiable edges defeating the cache. Fix the
edge warm-stability and the re-execution it induces goes away — at which point the real
benefit/cost tradeoff can finally be measured. The §13 numbers measured a BROKEN recorder, not the
direction's ceiling.

## §15. IMPLEMENTING THE PREREQUISITES — scoped by the artifact (2026-05-31)

Before designing, I quantified the defect's shape in the reproduced cold DB (8-package minimal case)
to decide whether the fix is narrow (caKey-dedup) or architectural (sub-scope isolation):

```
caKeys with BOTH empty+non-empty traces in History: 336   (overwrite victims)
caKeys empty-only:                                    0
caKeys non-empty-only:                             1211   (recorded consistently, never overwritten)
caKeys recorded as >1 distinct trace:               341
```

**Decisive finding: EVERY empty producer trace is an overwrite victim (336 both-shapes, 0 empty-only).**
An empty `__ca:` trace never arises on its own — it is always a caKey that ALSO had a legitimate
non-empty recording, then got overwritten (last-writer-wins) by a later empty-range recording. So the
30,629-session zero-dep-trace collapse (§14) is entirely an OVERWRITE artifact, not a "the deps were
genuinely empty" artifact.

### Prerequisite 1 (warm-stability) = two fixes, the first NARROW and primary

**Fix 1a — caKey-keyed, write-once producer recording (NARROW, the primary fix).**
Change `recordCAProducer` (trace-session.cc:845) to dedup on **caKey**, not `Bindings*`, AND to be
WRITE-ONCE per caKey per session: the first recording of a `__ca:<drvHash>` wins; later forces of the
same derivation (fresh `Bindings*`, possibly empty/different range) MUST NOT re-`recordSync` and
overwrite the CurrentNode. Implementation:
- Add a session-scoped `Set<AttrPathId>` (or reuse a caKey→ProducerEntry index) of already-recorded
  caKeys. On entry, if caKey already recorded this session → return the existing edge (from the
  side-table / a caKey→entry map), do NOT recordSync.
- Also register the NEW `Bindings*` → existing entry in `producerMap` (so the gate still fires for this
  re-forced value), but pointing at the FIRST trace.
- Soundness: caKey = `__ca:<drvHash>` is a content address; "same caKey → same derivation" is sound by
  construction (drvHash folds in the derivation's inputs). Write-once is therefore correct: the first
  recording's deps are A valid input-closure for that derivation; later recordings can only be equal-or-
  subset (memoized), so keeping the first (most complete) is the right choice.
- This fixes all 336/2213 overwrite victims (the bulk of the 30,629-session collapse). Estimated NARROW:
  ~one session-scoped set + the dedup-branch rewrite + a unit pin (record same caKey twice with
  different ranges → CurrentNode unchanged, edge stable). NOT architectural.

**Fix 1b — deterministic / self-verifiable producer deps (HARDER, needed for the 1211 non-empty too).**
Even write-once, the FIRST recording captures `snapshotEpochRange` = an ambient global-log slice
(superset: nested derivations + warm-hit replays), which may not fully re-verify warm (the subagent's
"shape 1"). Whether 1a ALONE suffices depends on how many of the 1211 non-empty producers fail warm
re-verify — UNMEASURED yet (next step: apply 1a, re-bench, see if verify.failed drops to ~0 or only to
~1211-scale). If non-empty producers also fail, 1b is required: build the producer trace from the
derivation's OWN input closure (sub-scope isolation at the args-force), not the ambient epoch slice —
the §9/§10 aggressive-recorder machinery, but applied to make the PRODUCER deps deterministic rather
than to filter the consumer. The §9 work + the §12 recovery fix are the substrate for this.

### Prerequisite 2 (gate-fire rate, ORTHOGONAL) — the §3b/#5 re-keying
Independent of warm-stability: even a perfectly warm-stable edge only helps if the gate FIRES. On
closures.gnome it fires on ~3% of traces because consumers force the `commonAttrs //`-wrapper / string
`outPath`, not the keyed `strict` value (§3b follow-up #5). Register producer identity on the value
consumers actually re-force (the `.outPath`/`.drvPath` string identity, or the wrapper attrset). This
is the harder, separately-designed re-keying; it does not block measuring 1a's effect on the
already-firing 3%.

### Recommended implementation order
1. **Fix 1a** (caKey write-once) — narrow, high-confidence, fixes the 30K-collapse. Unit-pin it.
2. **Re-bench hot** under 1a-only. Measure: does verify.failed drop to ~0 (1a sufficient) or to a
   ~1211-scale floor (1b also needed)? This is the cheap experiment that decides 1b's necessity.
3. **Fix 1b** only if step 2 shows non-empty producers also fail warm — and only then is the
   §9 sub-scope-isolation work on the critical path.
4. **Prerequisite 2 (re-keying)** is the separate benefit-side lever, tackled after warm-stability.

The key reframing: the FIRST prerequisite (warm-stability) is mostly a NARROW correctness fix (1a),
not the "architectural re-keying" the §13b/#5 framing implied. The architectural part (1b sub-scope +
2 re-keying) may not even be needed for warm-stability — step 2 decides. This is a much more tractable
path than the NO-GO verdict suggested.

## §16. Fix 1a RE-BENCH — the §13 NO-GO is OVERTURNED. §3b hot cost was 93% a fixable defect.

Re-ran the hot bench with the Fix-1a binary (commit 868038369), isolated root `.aggr-bench2`, same
25-commit closures.gnome anchor, sequential, --with-stats. Compared §3b conservative against both the
no-§3b baseline (this build) and the PRE-FIX conservative numbers (§13's `.aggr-bench` run 2).

### HOT (the bottleneck §13 said was inherent) — median/commit
| config | wall | verify.failed | record.count | hits/misses |
|---|---:|---:|---:|---:|
| no-§3b baseline | 1.00s | 0 | 0 | 7/0 |
| §3b conservative **PRE-FIX** | 7.95s | 364 | 8,448 | 5/2 |
| §3b conservative **FIX 1a** | **1.09s** | **0** | **0** | **7/0** |

**Fix 1a takes §3b conservative hot from 7.95s → 1.09s — matching the no-§3b baseline (1.00s).**
verify.failed 364→0, record.count 8,448→0, hits/misses 5/2→7/0. The cache-defeat is GONE; §3b is now
~free on hot (the ~9% residual is in the noise band). On a clean warm hit the root trace serves the
whole tree from cache, no derivation thunks re-force → no producer hook → §3b correctly inert (hot
producerEdges=0, replay.totalCalls=0).

### COLD — unchanged (the real, separate §3b cost)
no-§3b 1.02s; conservative 7.7s (was 7.94s; cold record.count 8,448→5,755 as write-once stops cold
re-records too). The COLD recording overhead is a genuine, separate cost (the ~1.5ms × thousands of
producer recordSync, the §3b/#4 dominant term) — Fix 1a doesn't address it; Layer 1/2a async recording
(landed) does, partially.

### This OVERTURNS the §13/§14 framing
§13 concluded the producer-partition direction is a NO-GO because "materialize-time re-execution
dominates hot and the edge adds to it." §14 found that re-execution was CAUSED by §3b's own
overwrite bug defeating the cache. §16 confirms: fixing the bug (Fix 1a, ~30 LOC) restores hot to
baseline. **The 16× / 8× hot regression was 93% a fixable correctness defect, NOT an inherent cost of
the direction.** §13's numbers measured a broken recorder.

### Adversarial validation (this is not a "§3b silently disabled" artifact)
- §3b is STILL ACTIVE under Fix 1a: cold records 12,634 non-empty producer caKeys, cold
  producerEdges=306. The gate fires.
- SOUNDNESS: bench reports "All outputs match reference" for conservative Fix-1a hot — byte-identical
  to no-trace eval.
- The bug signature is ELIMINATED: 0 empty producer traces (was 336/2213), 0 Sessions rows pointing at
  an empty trace (was 30,629).
- The residual 176 multi-trace caKeys are BENIGN cross-commit re-records (genuine input changes across
  nixpkgs commits): 0 of them have an empty trace, 0 current Sessions point at empty. Not the
  within-eval overwrite bug (which Fix 1a kills); correct cross-commit behavior.

### Revised disposition (MAJOR change from §13)
The §3b CONSERVATIVE shape, with Fix 1a, is now **hot-neutral** (1.09s vs 1.00s baseline) and sound —
no longer a hot net loss. The remaining cost is COLD recording, which is the original §3b/#4 cost that
async recording (Layer 1/2a) targets. So the path to a net-positive §3b is now: Fix 1a (DONE — hot
neutralized) + finish async cold recording (Layer 2a landed; Layer 2b deferred) — NOT the
architectural sub-scope/re-keying work §13b implied. The AGGRESSIVE shape (edge replaces flattened
deps) is a separate benefit-side lever (storage + verify-walk reduction) gated on the #5 re-keying
(gate fires ~3%); it should be RE-BENCHED under Fix 1a too (the §13 aggressive 16× was also mostly the
overwrite bug — the edge-emit just doubled the re-record volume), but conservative-Fix-1a being
hot-neutral is the headline.

### Caveats (honest)
- 25-commit pilot under concurrent machine load; led with medians + counters. The 7.95→1.09 collapse
  and verify.failed 364→0 are far beyond noise. A 100-commit run refines magnitudes, not the result.
- COLD cost is real and unaddressed by Fix 1a. "Hot-neutral" ≠ "net-positive end-to-end" — cold still
  costs ~6.7s over baseline per first-eval. The win is that hot (the repeated case) is now free.
- The AGGRESSIVE shape was NOT re-benched here (conservative isolates the Fix-1a effect); that's the
  next measurement.
