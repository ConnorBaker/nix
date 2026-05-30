# Keyset-dep downgrade: sound-by-construction design

**Status:** DESIGN ONLY. No production code. Decision artifact for whether
to build the narrow prototype (which touches the hot recorder path).

**Companion docs:**
- `plans/keyset-provenance-differential-harness.md` — the differential
  oracle + the v6→v11 provenance-proof obligation this implements.
- Test guards already landed:
  `src/libexpr-tests/eval-trace/property/invariant/keyset-provenance.cc`
  (19 enabled + 1 DISABLED red pin) and
  `src/libexpr-tests/eval-trace/store/keyset-escape.cc` (5 cross-trace
  escape tests).

---

## 1. What the downgrade is

When a trace observes a **complete key set** of an attrset
(`StructuredProjection #keys`, recorded eagerly at
`maybeRecordAttrKeysDep`, `src/libexpr/eval-trace/deps/shape-deps.cc:79`),
the recorder pins that key set as a dependency: any add/remove of a key in
the source invalidates the trace. That is sound but **over-records** when
the observed key set never actually flows into the trace's result. The
downgrade replaces the coarse `#keys` dep with the finer per-observed-member
deps that *did* flow — IFF the key set "never reaches a result-visible sink."

The canonical motivating shape (the DISABLED red pin
`DISABLED_SeqDiscardedAttrNames_UnobservedSiblingAdded_StillHits`):

```nix
builtins.seq (builtins.attrNames j) (j.a)
```

`attrNames j` forces the key set (records `#keys`), `seq` discards it, only
`j.a` reaches the result. Adding a sibling key to `j` leaves the result
unchanged, yet today the `#keys` dep invalidates → fail-closed over-record.

---

## 2. The recon facts (verified, do not re-derive)

1. **Eager, liveness-blind recording.** `maybeRecordAttrKeysDep` records
   `#keys` at observation time via the precomputed-keys fast path
   (shape-deps.cc:117-126), gated only by `markBindingsScanned` dedup. There
   is NO result-visible-sink / reachability / liveness / taint mechanism
   anywhere in eval-trace (Explore sweep of `eval-trace/`, headers, `deps/`
   — only dedup signals: `markBindingsScanned`, `observeRecordedDep`,
   no-active-context drop).

2. **The finalization seam exists and has both inputs.** At
   `src/libexpr/eval-trace/cache/trace-session.cc:164-165`:
   ```cpp
   auto directDeps = depCapture.finalizeAndTakeDeps();   // 164: all captured deps
   CachedResult attrValue = buildCachedResult(st, *target); // 165: the result
   ```
   Both the captured dep set and the built result are in hand locally,
   before `publishTrace.publish(attrValue, std::move(directDeps))` at 168.
   A prune pass could run between 165 and 168 and mutate `directDeps`.

3. **Benefit is narrow (already proved).** `§5.5` of the harness plan proved
   that `removeAttrs` / `//` / `intersectAttrs` / `mapAttrs` point-access
   shapes are ALREADY precise — they record NO `#keys` dep, only FileBytes +
   point deps. The genuine headroom is the seq-discard shape, which is
   uncommon. The original v6→v11 win was a *tail-rescue* killing ~9
   catastrophic by-name outliers; median/p90 barely moved, and it was
   command-JSON-only.

4. **ImplicitStructure `#keys` must be IGNORED.** `IS#keys`
   (json-to-value.cc:289, a creation-time recovery guard) carries the same
   `canonicalKeysHash` but a different `CanonicalQueryKind`. Only
   `StructuredProjection #keys` is the demand to downgrade; `IS#keys` is
   structural-override machinery and pruning it would break recovery.

---

## 3. The decisive constraint: the cross-trace escape (OR-4 residual)

A naive implementation runs a **single-trace reachability check** at the
finalization seam: "does the keyset-bearing attrset `A` appear in *this*
trace's `CachedResult`? If not, prune `#keys`." This is WRONG, and
`store/keyset-escape.cc` is the guard.

**Why.** A `#keys` dep lives on a *parent* trace (the trace at the attrset's
attr-path node). Other traces consume that parent through the **trace-hash
identity channel**: a sibling/child records a `TraceValueContext` /
`TraceParentSlot` dep storing the parent's *trace hash*. The `#keys` dep
folds into that trace hash (`StructuredProjection contributesToTraceHash`),
so today a key change to the source recomputes a different parent hash →
`resolveTraceContextHash` fails → the consumer invalidates
(verifier.cc:243-278).

If the prune removes `#keys` from the parent's trace, the parent's trace
hash no longer reflects the key set. A later key change does **not** change
the parent hash → the parent stays "valid" → **the consumer stale-serves a
value that depended on the old key set.** This is the GHC-orphan-instance
analogue: the liveness obligation discharges in the parent's module but is
observed in another.

**The check cannot be made complete at the parent's finalization.** A
consumer's `TraceValueContext` dep requires the parent's trace hash to
already exist (`getCurrentTraceHash`), so the parent always records *before*
the consumer. At the moment the parent finalizes, **no consumer exists yet** —
the reachability pass is blind to a future escape. Therefore the cross-trace
escape is **undecidable locally** and the rule must be **fail-closed**: treat
any keyset-bearing attrset that is exposed as a cross-trace observable as a
sink.

**What makes an attrset a cross-trace observable.** Its trace's pathId can be
the target of a `TraceValueContext` / `TraceParentSlot`. That is true for any
*named attr-path node* whose result is (or contains) the keyset-bearing
attrset — i.e., essentially every intermediate attrset in a real flake eval.

---

## 4. The locally-decidable safe domain

Combining §3 with the seq-discard motivation, the **only** keyset dep that is
provably safe to prune at the finalization seam is one where:

- **(S1)** the keyset-bearing attrset `A` does **not** appear in this trace's
  `CachedResult` (not the root value, not nested in any `attrs_t`/`list_t`
  the result materializes); AND
- **(S2)** this trace's pathId is a **leaf** computation whose result is a
  scalar (or otherwise non-attrset) that nothing can context-navigate into
  for `A`'s keys.

S1 is directly checkable from `attrValue` (the `CachedResult` at line 165).
S2 is the conservative over-approximation that closes the undecidable
cross-trace gap: if the result is a scalar and `A` is absent from it, no
consumer can recover `A`'s key set through this trace — not through
materialization (A isn't in the result) and not through the trace-hash
channel in a way that matters (a context consumer of a scalar leaf observes
the scalar identity, not A's keys).

`seq (attrNames j) (j.a)` satisfies both: `A = j` is not in the result
(`j.a`, a scalar), and the trace is a scalar leaf. The shape is exactly the
red pin.

**Everything else fails closed** — keep `#keys`. In particular: any trace
whose result is or contains the attrset, any attrset-valued node, any node a
sibling could context-depend on for its keys.

---

## 5. Why this is "sound by construction," not a heuristic

Per the subsystem's design principle (make illegal states unrepresentable),
the prune must be expressed so that violating soundness does not compile /
cannot be reached, not "remember to check the result."

Proposed shape (sketch, not committed):

- A `KeysetLivenessReview` sealed capability (mirrors `VerifiedFileDep` /
  `ShapePreservingReview`): the prune function consumes one, and the only
  factory is a function that takes `(const CachedResult & result, const
  Dep & keysetDep)` and returns the capability **only** when S1 ∧ S2 hold.
  No other path can authorize a prune.
- The prune runs at trace-session.cc between 165 and 168, operating on the
  local `directDeps` vector before `publish`. It replaces a pruned `#keys`
  dep with the per-observed-member deps the trace actually recorded (the
  point `StructuredProjection` deps already present in `directDeps` for the
  members that flowed). If no per-member deps were recorded for `A`, the
  `#keys` dep is NOT prunable (nothing finer to fall back to → keep it).
- Recording-site change at `maybeRecordAttrKeysDep` is **not** needed and
  should be avoided: the recorder cannot see the result, and moving liveness
  upstream reintroduces the blindness §3 warns about. Keep recording eager;
  prune at finalization where the result is known.

The capability makes the soundness condition the *only* way to reach the
prune; the fail-closed default is "keep `#keys`."

---

## 6. Test guards already in place (the prototype must keep these green)

- **Red pin (flips green when the prune lands):**
  `keyset-provenance.cc::DISABLED_SeqDiscardedAttrNames_UnobservedSiblingAdded_StillHits`
  — enable + assert `primaryHit` once the prune is implemented.
- **Soundness floor (must stay green):** the 5 `store/keyset-escape.cc`
  tests. If the prune ever removes `#keys` from a context-observable parent,
  `KeySetEscape_*_KeyAdded/Removed_InvalidatesConsumer` go red. These are the
  fail-closed guard for §3.
- **Precision controls (must stay green):** the enumeration-sink and
  value-change-keeping-keys tests in `keyset-provenance.cc` — they prove the
  prune does not over-prune (enumeration sinks keep `#keys`; value-only
  changes still hit).
- **Differential oracle:** any prototype run must compare the deep-forced
  served value against post-mutation ground truth (`Oracle::servedStale`),
  not the path counters — the counter is confounded by per-leaf lazy
  re-derivation (DEF-6).

---

## 7. Recommendation

The narrow safe domain (§4) is genuinely narrow: real flake eval rarely
produces a scalar-leaf trace that forced a full key set and discarded it,
because the consumers that would force `attrNames` get their own keyset deps
(§3 — the cross-trace escape is not independently reachable in real eval,
which is why the keyset-escape tests are synthetic). The measured v6→v11 win
was a command-JSON tail-rescue, not a hot-path median mover.

**Decision options:**
- **(A) Build the prototype now** — implement §5, enable the red pin, run the
  10-commit correctness gate + no-debug/no-stats benchmark, revert if the
  gate or the keyset-escape tests fail. One behavioral change at a time.
- **(B) Keep deferred** — the architecture (sealed capability + finalization
  prune + per-member fallback) is sound, but the gain is a narrow tail; the
  guards are in place, so it can be built later without re-deriving any of
  this. This matches the prior "large architecture for narrow gain" verdict.

Either way, the cross-trace escape is now pinned, so a future prototype
cannot silently regress the residual we know is hardest. Do not start (A)
without explicit sign-off: it touches the hot recorder/finalization path and
is gated by the correctness + benchmark protocol above.
