# Keyset-Provenance Differential Test Harness

**Status:** design memo (precedes implementation)
**Date:** 2026-05-29
**Branch:** `vibe-coding/file-based-eval-cache`
**Companion memory:** `project_eval_trace_provenance_proof_obligation`
**Companion research (in flight):** multi-EvalState unit-fixture feasibility (see §7)

## 0. One-paragraph summary

The provenance proof (`project_eval_trace_provenance_proof_obligation`) is a
**strict downgrade** of an already-fail-closed recorder: a complete-keyset
observation of an attrset/dir `D` may be downgraded to per-observed-member deps
*iff* the keyset never reaches a result-visible sink. The only thing a bug in
that downgrade can do is **over-reuse** — serve a stale result when the keyset
*did* matter. This memo specifies the differential harness that **detects
over-reuse**, so the downgrade can be prototyped safely. **The harness must
exist and pass against the current (pre-downgrade) recorder before any
downgrade code is written.** Tests lead.

## 1. Why a harness, and why first

Three facts from the research record force "tests first, code second":

1. **No prior art.** No production system reuses across an unobserved-member
   change (`project_eval_trace_glob_research_review`: decisive null). The
   soundness burden is entirely ours; there is nothing to copy and nothing to
   diff against except ground-truth re-evaluation.
2. **The work log repeatedly falsified "sound" shortcuts at the 10-commit
   gate.** v7 exists *because* a plausible heuristic (positive `hasAttr`
   overriding a complete-keyset observation) was unsound for the mixed
   enumeration+point case. The discriminator is subtle; intuition is not
   trustworthy here.
3. **The failure mode is silent.** Over-reuse produces a *correct-looking*
   value (the stale one). Only a comparison against an independent ground truth
   catches it. That comparison *is* the harness.

## 2. The differential oracle (2×2)

Per keyset mutation, cross two independent observations:

- **Ground-truth value:** evaluate the expression through a **non-cached**
  path, before and after the mutation, and compare with `assertValuesEqual`
  (already in `property/expr-gen.hh`). This is the source of truth for "did the
  result actually change?"
- **Cache decision:** `PathCountersSnapshot` (`helpers.hh:441`) — `hit` via
  `primaryCacheServedOnly()`, `miss` via `deltaTraceCacheMisses() >= 1`.

```
                        cache HIT            cache MISS
ground-truth value  ┌─────────────────┬──────────────────┐
   UNCHANGED         │  ✓ precision     │  precision LOSS   │  ← tolerable; log, don't fail
                     ├─────────────────┼──────────────────┤
   CHANGED           │  ✗ SOUNDNESS BUG │  ✓ sound          │
                     └─────────────────┴──────────────────┘
```

The downgrade can only ever push a cell **upward** (miss → hit). The oracle's
job is to fail loudly on the top-right→top-left transition (the soundness cell)
and to *measure* the bottom-right→bottom-left transition (the precision win the
downgrade is for). A precision *loss* (top-right) is tolerable and only logged —
it never fails a test, because the fail-closed baseline lives there by design.

## 3. The gap in the current suite (why nothing already covers this)

The existing property invariants are the right skeleton but deliberately
exclude exactly our case:

- `property/invariant/precision.cc` — mutates an **unrelated file** (never
  referenced by the expression). It proves "untracked file change → still
  hits." It does **not** touch the keyset of a *tracked* structured source.
- `property/invariant/invalidation.cc:58` — `RC_PRE(expectedKind != Attrset &&
  != List)` and `:76-94` restricts to `Kind::File`, explicitly excluding
  `Kind::JsonFile` because the JSON mutation is **keyset-preserving**
  (`expr-gen.cc:89-122` changes only values/types, never key *names*). The
  comment at `:76-82` says the SC override *correctly* serves those — which is
  true, and is exactly why that test cannot exercise a keyset *change*.
- `traced-data/dep-precision/attrkeys.cc` has deterministic
  `AttrNamesOnly_KeySetChange_CacheMiss` (key added → miss). That is the
  soundness *ceiling* for one hand-written expression, but it is not
  differential (no ground-truth cross-check) and not generated.

**Nobody drives a keyset-*changing* mutation (add/remove/rename a key) against a
ground-truth re-eval comparison.** That intersection is the harness.

## 4. What is reused vs. new

| Piece | Status | Source |
|---|---|---|
| `TraceCacheFixture`, `simulateWarmRestart`, per-iter fingerprint rotation | reuse | §N.4 pattern in test CLAUDE.md |
| `PathCountersSnapshot` hit/miss observable (`primaryCacheServedOnly`, `deltaTraceCacheMisses`) | reuse | `helpers.hh:441` |
| `assertValuesEqual` (ground-truth comparison) | reuse | `expr-gen.hh:211` |
| `rc::detail::checkGTestWith` body shape | reuse | every `invariant/*.cc` |
| `TempJsonFile` / `TempDir` RAII + `invalidateFileCache` | reuse | `helpers.hh` |
| **Keyset-mutation generator** (add / remove / rename key — NOT value-preserving) | **new** | inverse of `expr-gen.cc:89-122` |
| **Ground-truth differential** (uncached double-eval crossed with cache decision in one iteration) | **new** | no existing test crosses them |
| **Sink-classification corpus** (paired must-miss / may-hit expressions) | **new** | encodes the obligation's discriminator as data |
| **Uncached eval path** (a second evaluation that bypasses the cache for ground truth) | **new — see §5.1** | — |

## 5. Component design

### 5.1 Ground-truth (uncached) evaluation

The oracle needs the *true* post-mutation value, independent of the cache. Two
candidate mechanisms, in preference order:

1. **Pre-mutation cold value as ground truth for "unchanged", post-mutation
   re-eval for "changed".** The cleanest: the cold eval (which records) already
   forces the real value via the loader. Capture the cold `Value` *before*
   mutation. After mutation, force a *fresh, uncached* eval of the same source.
   The non-cached evaluation can be obtained by evaluating the raw Nix
   expression through a throwaway `EvalState` path that does not consult the
   trace store — i.e. the same thing `makeCache`'s loader does internally, but
   invoked directly. **Verify in implementation** whether `TraceCacheFixture`
   exposes a loader-only eval; if not, the loader lambda passed to `makeCache`
   *is* the uncached path and can be called directly to get ground truth.
2. **Fingerprint-isolated re-record.** Force the post-mutation value through a
   *different* `testFingerprint` so the cache necessarily misses and re-evaluates
   — the re-evaluated value is ground truth. Cheaper to write, slightly less
   pure (still goes through the recording path, but with a guaranteed-cold
   slot). Acceptable because recording does not alter the produced value.

Decision deferred to implementation; option 2 is the fallback if option 1's
direct-loader call is awkward. Either way the ground-truth value must come from
a path that the cache-under-test cannot have served.

### 5.2 Keyset-mutation generator

The current JSON mutation (`expr-gen.cc:89-122`) is **keyset-preserving** by
construction. The harness needs the inverse: a mutation that changes the **key
set** of a structured source while keeping it valid JSON. Three operations:

- **add-key:** insert a fresh key (name not already present) with a scalar value.
- **remove-key:** delete an existing key (only when ≥2 keys, so the object stays
  non-empty and parseable).
- **rename-key:** delete one key and add a differently-named one (keyset changes,
  cardinality may stay equal — stresses the "same length, different names" case
  that a naive length-only dep would miss).

This is a **new** generator method (or a free helper alongside
`generateMutation`), not a modification of the existing one — the existing
keyset-preserving mutation is load-bearing for `invalidation.cc`/`precision.cc`
and must not change.

### 5.3 Sink-classification corpus

The obligation's discriminator (`memory §Result-visible SINK`) becomes test
data: paired expressions over the *same* structured source, differing only in
whether the keyset reaches a sink.

**Sink cases (keyset reaches result ⇒ MUST miss on any keyset change):**
- `builtins.attrNames x` — keyset materialized into result.
- `builtins.length (builtins.attrNames x)` — cardinality into result.
- `builtins.concatStringsSep "," (builtins.attrNames x)` — keyset stringified.
- `if builtins.hasAttr "k" x then A else B` where the *branch* reaches the result
  — control flow (this overlaps the v7 case: a point observation that is
  nonetheless result-visible).

**Non-sink cases (keyset point-observed only ⇒ MAY hit when an *unobserved*
member changes):**
- `x.a` — single point access; sibling key add/remove must not matter to the
  *value* at `a`.
- `x ? a` then use only the boolean for a key that is unaffected by the mutation.
- `(builtins.mapAttrs (k: v: v) x).a` — keyset-preserving transform, point-read
  of the output (still non-sink *iff* the mapped keyset is not itself read).

Each corpus entry carries an explicit `mustMiss: bool` tag. The oracle then
asserts:
- `mustMiss && groundTruthChanged` ⇒ require `deltaTraceCacheMisses() >= 1`.
- `!mustMiss && !groundTruthChanged` (unobserved sibling changed) ⇒ this is the
  precision *target*: today it will MISS (fail-closed over-recording per
  work-log:218-225); after the downgrade it should HIT. See §6.

### 5.4 The two properties

**`KeysetReachesSink_MutateKeyset_AlwaysMatchesGroundTruth`** (live now)
For sink-corpus expressions: mutate the keyset, compute ground truth, and assert
the oracle's soundness cell. Ground-truth-changed ⇒ `deltaTraceCacheMisses() >= 1`.
This **must pass against the current recorder** — it proves the fail-closed
baseline is sound — and must **keep passing after the downgrade** — it proves the
downgrade did not over-reach. This is the regression floor.

**`KeysetPointObservedOnly_MutateUnobservedMember_StillHits`** (red pin now)
For non-sink-corpus expressions: change an *unobserved* sibling key, compute
ground truth (which is **unchanged** — the point-read value didn't move), and
assert `primaryCacheServedOnly()`. **Against the current recorder this MISSES**
(fail-closed over-recording of the broader source keyset). So this test lands
`DISABLED_` with a comment stating it is the **acceptance test for the
downgrade**: it flips green exactly when the provenance proof lands and not
before. The red→green flip is the proof the downgrade did something real and
did not merely loosen the soundness test.

## 5.5 IMPLEMENTED — empirical corrections (2026-05-29)

The harness landed as
`src/libexpr-tests/eval-trace/property/invariant/keyset-provenance.cc` (8
enabled tests + 1 DISABLED pin, all built and run). Two design hypotheses from
§5.3 were **falsified by the harness itself** and corrected:

1. **`(removeAttrs j ["b"]).a` does NOT over-record at the unit level.** The
   hypothesized fail-closed precision loss (work-log:218-225) does **not**
   manifest for minimal removeAttrs/`//`/intersectAttrs/mapAttrs *point-access*
   shapes: they record **no** `StructuredProjection #keys` dep on the source
   (only FileBytes + the point dep), so a sibling-add **already hits**. The
   work-log's broader-source-keyset over-recording is gated behind the
   Nixpkgs-callsite / command-JSON path, which these fixtures do not exercise.
   The original DISABLED pin on this shape was therefore **vacuous (already
   green)** — caught by running it with `--gtest_also_run_disabled_tests`.

2. **The genuine construction-only red-pin is
   `builtins.seq (builtins.attrNames j) (j.a)`.** `attrNames` is forced (records
   the `SP#keys` complete-keyset dep — verified via the vacuity guard) but its
   value is discarded by `seq`; only `j.a` reaches the result. Empirically: a
   sibling-add leaves ground truth unchanged yet the cache **misses** (the
   `SP#keys` dep invalidates) — fail-closed over-recording at exactly the dep
   the downgrade removes. This is now the DISABLED pin
   (`DISABLED_SeqDiscardedAttrNames_UnobservedSiblingAdded_StillHits`),
   **verified red today** and vacuity-guarded so it cannot pass for the wrong
   reason. Its companion `SeqDiscardedAttrNames_AccessedValueChanged_Invalidates`
   (enabled) pins that the point dep on `j.a` must always invalidate on a value
   change — the downgrade removes the keyset dep, never the point dep.

**Lesson (this is the harness earning its keep):** the differential oracle
caught that the intuitive precision-loss shape was already precise, and
redirected the pin to a shape where the over-recording is real and at the
correct dep. A pin chosen by reasoning alone would have been a no-op acceptance
test.

## 5.6 ADVERSARIAL PASS — 11 probes, oracle promoted to value-comparing (2026-05-29)

A three-round adversarial diagnostic (`DISABLED_AdversarialProbe`, since folded
into real tests and removed) interrogated the harness for "test the part, not
the seam" defects. The headline finding **changed the oracle itself**:

**The counter is UNSOUND as a stale-serve detector for non-scalar results.**
For `builtins.attrValues j` with a keyset-preserving VALUE change, the path
counter reports `primaryHit=1, miss=0` — but the deep-forced served value equals
the NEW ground truth (`servedEqOld=0, servedEqNew=1`). A root-spine "primary
hit" still re-derives the element thunks per-leaf (DEF-6 lazy verification), so
the value is correct despite the "hit". The original counter-only
`assertNoStaleServe` (which treated `groundTruthChanged && primaryHit` as stale)
would have **false-failed** this — a defect in the harness, not the cache.

**Fix:** soundness is now a **deep-forced VALUE comparison** (`servedStale =
served != post-mutation ground truth`); the counter is retained only as the
**precision** signal. `runOracle` deep-forces `pre`/`post`/`served`. A generic
`runOracleWith(expr, mutate)` core handles non-JSON sources (readDir,
multi-file `//`).

**Probe findings (all folded into enabled tests unless noted):**

| Probe | Shape | Finding | Test |
|---|---|---|---|
| GAP1 | `attrNames`, value-only change | HITS — miss is keyset-specific, not blanket FileBytes | `SinkAttrNames_ValueChangeKeepingKeys_StillHits` |
| GAP6 | negative `j ? "z"`, absent→present | invalidates; **SP#keys=0**, point `#has(z)` only | `NegativeHasAttr_ProbedAbsentKeyAppears_Invalidates` |
| GAP7 | `concatStringsSep(attrNames j) + (j ? "a")` | TRUE v7 floor: sink + point on same set; unobserved-add invalidates | `SinkAttrNamesPlusHasAttr_UnobservedKeyAdded_Invalidates` |
| GAP8/9/10 | enumeration sinks (`attrValues`, `map (k: j.${k}) (attrNames j)`, `length∘attrValues`) | all SP#keys=1, invalidate on key-add — the glob-enumeration trap is covered | `EnumSink*_KeyAdded_Invalidates` (×3) |
| GAP8d | `attrValues`, value-change, value-compared | NOT a stale serve (served==new); drove the oracle promotion | `EnumSinkAttrValues_ValueChangedKeepingKeys_NoStaleServe` |
| GAP11 | `attrNames(removeAttrs j ["b"])` | derived-keyset sink; **SP#keys=0**, key-add invalidates, value-change hits | `DerivedSinkAttrNamesOfRemoveAttrs_*` (×2) |
| GAP4 | `attrNames(readDir dir)`, entry-add | invalidates — the by-name motivating case, no FileBytes backstop | `SinkReadDirAttrNames_EntryAdded_Invalidates` |
| GAP5 | `attrNames(a // b)`, add key to a | invalidates | `SinkMergeAttrNames_KeyAddedToOperand_Invalidates` |
| GAP3 | `seq(attrNames j)(j ? "a")` | MISLABELED in round 1 as a v7 floor — attrNames is seq-DISCARDED, so this is a construction-only PRECISION case, not a floor. Subsumed by the existing seq-discard red pin. | (not added; corrected) |

The suite is now **19 enabled tests + 1 DISABLED red pin**. The two
`removeAttrs` derived-sink tests are deliberately NOT vacuity-guarded (they
record no SP#keys on the source); their soundness rests on ImplicitStructure
#keys + content, which the SP#keys-targeting downgrade does not touch.

## 6. Acceptance criteria (the contract this harness enforces)

1. **Before the downgrade lands:**
   - `KeysetReachesSink_*` passes (current fail-closed recorder is sound).
   - `KeysetPointObservedOnly_*` is `DISABLED_` (documented red pin); if enabled
     it would fail with a cache *miss*, which is over-recording, not unsoundness.
2. **After the downgrade lands:**
   - `KeysetReachesSink_*` still passes (no soundness regression — this is the
     non-negotiable: a downgrade that flips this green-to-red is rejected).
   - `KeysetPointObservedOnly_*` is enabled and passes (the precision win is
     real and measurable).
3. **At all times:** a top-right precision *loss* is logged via `log`-equivalent
   diagnostic (gtest `RecordProperty` / `<<` on the assertion), never a test
   failure — the fail-closed baseline is allowed to be imprecise.

## 7. Cross-trace residual (OR-4) — RESOLVED: use synthetic records

The obligation's hardest sink — **"keyset crosses into another cached trace"
(TraceValueContext / TraceParentSlot; OR-4 / GHC orphan-instance territory)** —
was investigated by a dedicated research pass (§0). **Verdict: a multi-EvalState
unit fixture is neither needed nor sufficient. Use synthetic raw-`TraceStore`
`record()` calls.** Three load-bearing findings:

1. **Multiple `EvalState`s already coexist fine** in one process —
   `nix_api_expr.cc` runs three simultaneously and shares `Value`s across them on
   the shared (idempotent `initGC`) Boehm heap. `SymbolTable`/`PosTable` are
   per-`EvalState` members, not globals. The only thing the skipped
   `concurrent-eval.cc` test actually needs is *per-thread GC registration* for
   **concurrent** drive — a thread-safety concern unrelated to cross-trace
   soundness. So "single-EvalState can't do it" was the wrong framing.

2. **Cross-trace keyset deps are already pinned synthetically**, via
   `db->record(ea, pathId, value, deps)` with `Dep::makeValueContext(...)` /
   `Dep::makeParentSlot(...)` whose hash comes from the live sibling/parent via
   `db->getCurrentTraceHash(ea, parentPath)`. A single `TraceStore` holds many
   traces at once and a single `VerificationSession` verifies across them — this
   is exactly the "one trace's bit crosses into another" model. Existing
   coverage: `store/trace-parent-slot.cc`, `store/trace-value-context.cc`
   (incl. a depth-3 A←B←C chain), `store/cross-session-subsumption.cc`
   (`_SubsumptionLeaks_*`), and — directly on point — **`verify/integration.cc::
   Integration_ParentSlot_DoesNotCaptureKeySetRemoval`**, which builds
   parent/child via raw `record()`, re-records the parent without the child, and
   asserts cross-trace verification behavior on a keyset removal.

3. **Real-eval reproduction is impossible**, per the production OR-4 analysis
   (`src/libexpr/eval-trace/CLAUDE.md:536-586`): the `ExprParseFile::eval`
   FileBytes backstop (`eval.cc:1422-1435`) always records `FileBytes(source)`
   and catches the source edit before any orphan can be served — "Condition 1 is
   the hard part… I cannot construct a shape that bypasses it." A real-eval
   (multi-EvalState or otherwise) harness cannot even *reach* the gap.

**Consequence for this harness.** The keyset-provenance file (§5/§8) stays
real-eval and single-`EvalState` for the in-trace sink/non-sink cases (§5.3).
The cross-trace sink is **out of scope for the real-eval property file** — but
it is NOT silently omitted (per "no silent caps"): the corpus carries a comment
pointing at the synthetic-record pins above, and any *new* cross-trace
keyset-provenance soundness case is added as a synthetic `TraceStoreFixture` +
`db->record()` test alongside `Integration_ParentSlot_DoesNotCaptureKeySetRemoval`,
**not** as a multi-EvalState fixture. No new fixture infrastructure is needed.

## 8. File layout

- `src/libexpr-tests/eval-trace/property/invariant/keyset-provenance.cc`
  - fixture `EvalTraceProperty_KeysetProvenance : TraceCacheFixture`
  - the keyset-mutation helper (local to this file unless a second consumer
    appears, in which case promote to `expr-gen.{hh,cc}`)
  - the sink corpus (static table with `mustMiss` tags)
  - the two properties from §5.4
- Register in `src/libexpr-tests/meson.build` under the
  `# eval-trace/property/invariant/` section.
- `git add -f` the new file before any `nix build` (sandbox gate; per
  `feedback_verify_build_exit_not_pipe`).

## 9. Risks / things to verify during implementation

- **Ground-truth purity (§5.1):** confirm the chosen uncached path genuinely
  bypasses the cache-under-test. If option 1 (direct loader call) is awkward,
  fall back to option 2 (fingerprint-isolated re-record). Do not let the
  "ground truth" secretly be a cache hit.
- **Shared-fixture isolation (§N.4):** rotate `testFingerprint` per iteration
  and call `simulateWarmRestart()` between phases, or iteration N's row poisons
  N+1's "cold" eval.
- **`hasAnyTracedDataLayer` gate (memory §70-74):** keyset deps fire only for
  TracedData attrsets. The corpus sources must be `builtins.fromJSON
  (builtins.readFile <json>)` (or `readDir`), not plain Nix attrset literals, or
  no keyset dep is recorded and the test is vacuous. Assert the dep is present
  (via a `DepPrecisionTest`-style `evalAndCollectDeps` check, or a dedicated
  precondition) so a vacuous pass is impossible.
- **Case-insensitive FS (§N.11):** if keyset mutations generate filesystem
  entries (readDir corpus), route names through `makeNixFilesystemIdentifierGen`.
  For pure-JSON corpus this does not apply (JSON keys are case-sensitive
  regardless of host FS).
