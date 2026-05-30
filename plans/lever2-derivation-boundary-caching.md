# Lever 2 — derivation-boundary caching (the `closures.gnome` outlier fix)

**Status:** IMPLEMENTATION SKETCH. No production code yet. This is the lever the
2026-05-30 evidence points to for the cold outliers; it supersedes the earlier
"Lever 5 (sub-trace reuse)" framing for the `closures.gnome` workload (see
`plans/lever1-observed-key-pruning.md` §"Design-question 1 RESOLVED — THE
REFRAME" for why: the outlier cost is a deep *computation* behind ~7 string
leaves, not a deep attrset; there are no intermediate attrset sub-traces to
reuse — the only reuse boundary inside the computation is the derivation).

**Authoritative context:** `doc/eval-trace-cache-README.md`,
`doc/eval-trace-cache-redesign-plan.md` (2026-05-29 CORRECTION finding 3 named
this; 2026-05-30 diagnostic + reframe). Soundness scaffolding:
`src/libexpr-tests/eval-trace/store/keyset-escape.cc`,
`plans/keyset-provenance-differential-harness.md`.

---

## 1. The measured problem this addresses

Ledger-D (HEAD `9f7311129`, 100-commit `closures.gnome`): cold mean 3.72 s,
median 1.11 s; ~23 outliers at 7–17.6 s. Diagnosis (stats.json + DB):
- `closures.gnome.<system>` is a **string** (a store path). Producing it forces
  the whole NixOS system derivation: **~20 M thunks**.
- The cache records **~7 coarse traces** for the eval (DB: `Traces=93` across all
  100 commits, each `values_blob` 100 KiB–1 MiB ≈ ~233 K flat deps). The 2 system
  strings are 2 of those 7 leaves.
- A commit touching **0.10 %** of a leaf's deps (e.g. one NixOS module —
  outlier `9b9f7241` changes only `autossh-ng.nix`) fails that leaf's trace.
  Recovery fails. The **entire ~10 M-thunk system computation re-runs** because
  there is no finer reuse unit inside it.

The derivation is the natural finer unit: a system closure is a DAG of thousands
of `derivation` calls; a one-module change perturbs a handful. If each
derivation's evaluation result were independently cacheable, an unchanged
derivation subtree would be reused instead of re-evaluated.

## 2. What exists today (verified in code)

- **`derivationStrictInternal`** (`primops.cc:1624`) is the single choke point —
  `derivation` → `derivationStrict` → here. It forces all input attrs, builds the
  `Derivation`, computes `drvPath`, and returns `{ drvPath, <outputs> }`.
- It is **already eval-trace-aware but only for invalidation:** at
  `primops.cc:2003-2011`, when `traceActiveDepth>0`, it records a
  `StorePathAvailability` dep on the `.drv` path (detects GC removal → re-eval).
  There is **no trace recorded that could SERVE this derivation's result**.
- `drvHashes.insert_or_assign(drvPath, h)` (`primops.cc:2020`) — Nix's
  within-process drv-hash memo. Not persisted, not a cross-process cache.
- **CLAUDE.md "Known gaps":** `derivationStrict` is explicitly listed as
  "outside the plan's stated scope (store writes / legacy builtins)." So the
  derivation boundary is uninstrumented for reuse *by design so far* — this lever
  is the decision to bring it in scope.
- `DerivedStorePath` CQK exists (`input-resolution.cc:143`) but is an input dep
  (a parent depending on a child's output path), not an output-serving record.

## 2b. O1 (keying) RESOLVED by schema study (2026-05-30) — more favorable than feared

Read the DDL (`sqlite-trace-storage-lifecycle.cc:149-243`) and lookup paths. The
trace store is **already a two-layer design that separates content-addressed
trace bodies from attr-path routing** — exactly what a derivation needs:

- **`Traces`** (body layer) is content-addressed and **attr-path-independent**:
  `id, trace_hash, full_hash UNIQUE, dep_key_set_id → DepKeySets, values_blob`.
  No `attr_path_id` column. `getOrCreateTrace` (sqlite-trace-storage.cc:540)
  dedups by `full_hash` via `traceByFullHash`, so **two attr-paths that produce
  the identical derivation result ALREADY share one Traces row.** The body layer
  needs no change for derivations.
- **`Sessions(session_key, attr_path_id) → trace_id`** and
  **`History(recovery_key, attr_path_id, …)`** are the ROUTING layer. This is the
  only place `attr_path_id` appears, and it is the sole obstacle: lookup is
  driven per-attr-path (`lookupCurrentNode` binds `(session_key, pathId)`,
  lifecycle.cc:361-364), so a derivation reached at a different path — or the
  same path after the parent's deps shifted — can't FIND the shared body trace.
- **`AttrPathId`** (attr-vocab-store.hh) is a trie node `(parent AttrPathId,
  child AttrNameId)` interned to a dense int — structurally a *tree-path*
  identity, positional not content-addressed. A derivation reached from many
  paths cannot be one `AttrPathId`. So the routing key must be generalized.

**Two viable designs (O1 is a routing change, NOT a body-table change):**

- **Design A — synthetic derivation namespace in `AttrPathId`.** Mint a reserved
  vocab subtree (e.g. root child `"__drv"`) and intern each derivation identity
  (content hash of observed inputs) as an `AttrNameId` under it, yielding a real
  `AttrPathId`. Then derivation traces flow through the EXISTING
  `Sessions`/`History` routing unchanged — `lookupCurrentNode`, recovery, history
  bootstrap all work as-is. **Cheapest: zero schema change, reuses every existing
  query.** Cost: `AttrNames` grows by one row per distinct derivation identity
  (6,419/commit — but interned/deduped, and `Strings`/`DataPaths` already hold
  27 K / 68 K rows, so the order of magnitude is in line). Risk: pollutes the
  attr-path vocab with non-attr-path entries; `displayPath`/`parentPath`
  semantics must tolerate the synthetic subtree. **Mechanically feasible:**
  `internName(std::string_view)` (attr-vocab-store.cc) interns arbitrary strings
  via `nameTable.intern`, so `"__drv:<contenthash>"` is a valid `AttrNameId`
  directly. **Caveat to verify:** the `internName(Symbol)` overload maintains a
  `symbolToName`/`nameToSymbol` mapping, and `childSymbol(AttrPathId)` assumes a
  backing Nix `Symbol`; a raw-string-interned derivation name has no Symbol, so
  any routing/recovery code that calls `childSymbol`/`displayPath` on a synthetic
  node must be audited (it may only need `feedPath` for hashing, which takes IDs
  not Symbols — confirm during prototype).
- **Design B — parallel derivation routing table.** Add
  `DerivationTraces(deriv_key BLOB, session_key BLOB, trace_id, …)` keyed by
  content hash. Cleaner separation, but a schema epoch bump + parallel
  lookup/recovery/history code paths (the verifier's whole
  Sessions/History/recovery machinery would need a derivation-keyed twin).
  **More code, more correct-by-construction.**

**Recommendation: prototype Design A.** It reuses the entire existing routing +
recovery + history pipeline by making a derivation "look like" an attr-path node
in a reserved namespace. The content-addressed body layer already does the
dedup. This drops O1 from "likely a schema change" (my earlier pessimistic
sketch) to "a vocab-namespacing change + a recording-site hook in
`derivationStrictInternal`." If Design A's vocab pollution proves problematic,
fall back to B. Either way, **O1 is no longer the blocker I feared** — the
two-layer schema was already built for content-addressed bodies.

Remaining true blockers are §3 (facet mask) and §5-O4 (storage budget /
compact records), not keying.

## 3. The hard obstacle: a derivation result is not soundly reusable by drvPath alone

The repeatedly-falsified shortcut (work-log runs 87/88/114, redesign-plan
2026-05-29 finding 3): "the changed package's own drv is unchanged, so serve the
parent." Rejected because **a parent can observe a child through non-output
facets** — `meta`, `passthru`, arbitrary attrs, `attrNames`, stringification,
absence. Reusing on output-identity alone serves stale output when the parent
read a non-output facet that changed.

So derivation-boundary caching needs a **facet mask**: record *which facets of
the derivation result the consumer actually observed* (just `outPath`/`drvPath`?
or `meta`, `passthru`, a specific attr?), and reuse only when the observed
facets are unchanged. This is the Bazel/Skyframe "strict dependency capture +
output-equality relation" idea (redesign-plan finding 3), specialized to the
derivation result attrset. The eval-trace machinery for "which facets were
observed" already exists in spirit — it is exactly the `StructuredProjection` /
attrset-shape dep recording — but it is not currently scoped to derivation
results.

## 4. Implementation sketch (smallest sound slice first)

The goal is NOT to cache every derivation (that risks the v53 "store too much"
failure — DB would explode from 93 traces to ~thousands/commit). The goal is to
make a derivation's evaluation a **reusable trace node** so an unchanged subtree
warm-hits. Sketch, in dependency order:

### Slice A — make `derivationStrict` a trace boundary (recording side)
- At `derivationStrictInternal` return (`primops.cc:~2023`), when
  `traceActiveDepth>0`, open a recording scope keyed by a **derivation identity**
  (candidate key: `drvName` + the hash of the *input attrs as observed*, i.e. the
  trace's own dep set — NOT `drvPath`, which is an output and circular for the
  result we want to serve). Record the result attrset `{drvPath, outputs}` as a
  `CachedResult` under that key.
- Reuse the existing `TracedExpr`/`record` machinery if possible by treating the
  derivation result like a child trace node, OR add a derivation-keyed trace
  table if the attr-path-keyed model doesn't fit (derivations aren't at stable
  attr-paths — the same `mkDerivation` is called from many paths). **Open
  question O1: keying.** Derivation identity must be path-independent (content of
  inputs), unlike the current `AttrPathId`-keyed traces. This is the biggest
  design fork — it may need a new trace-key kind.

### Slice B — facet-masked reuse (verification side)
- When the same derivation is reached again, before re-forcing its inputs, look
  up its recorded trace. Verify by the trace's own deps (the inputs it read) +
  the **facet mask** (which result fields the consumers observed).
- If the observed inputs are unchanged → serve the recorded `{drvPath, outputs}`
  without re-evaluating the derivation's input attrs. **This is where the ~10 M
  thunks are saved:** an unchanged sub-derivation's inputs are never re-forced.
- **Soundness gate:** serve only if every observed facet's dep verifies. Fall
  back to fresh eval (current behavior) on any miss. The keyset-escape /
  cross-trace tests are the floor: a derivation result that escapes via
  `TraceValueContext` must still re-verify.

### Slice C — measure before widening
- Prototype Slice A+B for derivations only; gate on the 10-commit correctness
  run (byte-identical output) + `pairwise cold/<new> vs cold/1`.
- Key metrics: does the DB stay bounded (not v53-explosion)? Do the ~23 outliers
  drop toward the 1.1 s median? What is the cold-record overhead of the extra
  derivation traces on the 77 fast commits (must not regress them)?

## 5. Open questions (must resolve before/during prototype)

- **O1 (keying) — RESOLVED (see §2b).** NOT a body-table change: `Traces` are
  already content-addressed (dedup by `full_hash`) and attr-path-independent.
  Only the `Sessions`/`History` ROUTING layer is attr-path-keyed. Recommended
  fix = Design A (synthetic `__drv` namespace in the attr-vocab, derivation
  identity interned as an `AttrNameId`), reusing the entire existing routing +
  recovery pipeline with zero schema change. Fallback = Design B (parallel
  derivation routing table, schema bump). O1 is no longer the primary blocker.
- **O2 (facet mask granularity):** is per-result-attr `StructuredProjection`
  recording already emitted when a consumer does `drv.outPath` / `drv.meta`? If
  so, the mask is mostly free; if not, it must be added at the derivation-result
  attrset.
- **O3 (memo interaction):** Nix already memoizes within a process
  (`drvHashes`); the cross-process cache must not double-count or fight it.
- **O4 (storage budget):** how many distinct derivations does `closures.gnome`
  evaluate, and what is the per-derivation trace size? If it is thousands × the
  current 100 KiB–1 MiB blobs, the storage model needs the compact-result work
  (redesign-plan "Compact Proof/Result Authority") FIRST.
- **O5 (is this even the dominant cost?):** confirm via a follow-up `--with-stats`
  run that the outlier re-eval is dominated by derivation evaluation (vs.
  module-system fixed-point overhead that is NOT per-derivation). If the cost is
  the NixOS module fixpoint rather than discrete derivations, the derivation
  boundary won't localize it and a different boundary (module options?) is
  needed.

  **O5 PARTIALLY MEASURED (2026-05-30, nix-instantiate + nix-store -qR, no
  build needed):**
  - `closures.gnome.x86_64-linux` instantiates **6,419 distinct derivations**.
  - Between outlier `9b9f7241` and its predecessor `d9e9954c`: **6,397 of 6,419
    derivations are byte-identical (99.66 %); only 22 changed** (22 added, 22
    removed, by drvPath). The change footprint is tiny — exactly the localization
    opportunity, and it confirms the cache re-evals ~20 M thunks to reproduce a
    result that differs in 0.34 % of its derivations.
  - **This makes O4 (storage) the binding constraint, not reuse potential.**
    6,419 derivations/commit at the current 100 KiB–1 MiB/trace blob size is
    GB-scale per commit — the v53 failure. Lever 2 is therefore **gated on
    compact per-derivation records** (drvPath + observed-facet digest, NOT a fat
    flat-dep blob). The compact-result work (redesign-plan "Compact Proof/Result
    Authority") is a PREREQUISITE, not a follow-on.
  - **O5 RESIDUAL MEASURED (2026-05-30, flamegraph profiler, no build needed):**
    ran `nix-instantiate ... -A closures.gnome.x86_64-linux --option eval-profiler
    flamegraph` (443 leaf samples), classified by LEAF frame (where the sample
    actually was, not anywhere in the deep nested stack):
    - **`pkgs/stdenv/generic/make-derivation.nix` = 35.0 %** (largest single
      cost) + other `pkgs/` package exprs ~20.8 % → **~58 % of eval cost is in
      derivation / package evaluation** — exactly the per-derivation boundary
      this lever caches.
    - **`lib/modules.nix` + `types.nix` + nixos modules = ~24.6 %** → the module
      fixpoint is significant but NOT dominant.
    - **Verdict: favorable but bounded.** A derivation-boundary cache could reuse
      the ~58 % derivation-eval cost for the 6,397/6,419 unchanged derivations.
      The ~25 % module-fixpoint cost is the residual that likely still re-runs on
      any change (it is the `evalModules` that produces the derivation set). So
      lever 2's realistic ceiling on the outliers is **~halving** the re-eval
      cost (the derivation share), not eliminating it — the 99.66 %-unchanged-drv
      figure is the reuse OPPORTUNITY, but the module-fixpoint floor caps the
      realized win. Still worth it (an outlier ~12 s → maybe ~5–7 s), but state
      the ceiling honestly; do not promise the naive 99.66 %.
    - This also means lever 2 must NOT try to cache below the module fixpoint;
      the cache boundary is the derivation result *after* the fixpoint has
      produced its inputs. The fixpoint itself is a separate (harder, probably
      out-of-scope) lever.

## 6. Honest assessment

- This is a **large** lever — it brings `derivationStrict` into scope (CLAUDE.md
  currently excludes it) and risks the v53 storage-explosion failure (O4). It is
  not a small prototype. (O1/keying turned out NOT to need a new trace-key kind —
  §2b — which lowers the cost somewhat, but the facet mask and storage budget
  remain.)
- It is, however, the lever the evidence points to for the *only* measured pain
  on this workload (the cold outliers). Lever 1 (enumerated-set) and the original
  Lever 5 (attrset sub-trace) do not fit `closures.gnome`'s computation-heavy,
  data-shallow shape.
- **Cheapest de-risking step before any build: O5** — one more `--with-stats`
  run + per-commit `logs`/profiler on outlier `9b9f7241` to confirm the 20 M
  thunks are derivation evaluation and that a handful of derivations actually
  changed (vs. a module-fixpoint blow-up that no derivation boundary localizes).
  Do this before committing to Slice A. Needs sign-off for the build itself.

---

## 7. Adversarial pass over the code (2026-05-30) — what nearly breaks the design, and the mechanism it reveals

A deliberate hunt for reasons Lever 2 *won't* work. Two findings invalidate
parts of the earlier sketch; the net is a STRONGER foundation than §4 assumed,
plus three real hazards.

### Finding A — dep attribution is per-thunk via the epoch log, NOT per-trace-scope (this is GOOD)

The earlier sketch assumed the recording boundary had to be *added* at
`derivationStrict`. Reading the recording machinery shows it already exists at a
finer level:

- `DepRecordingContext::Scope` (dep-recording-context.hh:160) holds `ownDeps`;
  `record()`/`replayMemoizedRange()` write ONLY to `currentScope()` (the back of
  the stack). `popScope()` (dep-recording-context.hh:348) **discards** the scope
  — child scope deps do NOT merge upward through scope nesting.
- So how does the root trace accumulate ~233 K flat deps? Via the **epoch log**,
  not scope nesting. `forceThunkValue` (eval.cc:1677-1705):
  1. snapshots `epochStart = traceCtx->currentReplayEpochSize()` BEFORE eval
     (eval.cc:1683),
  2. evaluates the thunk (deps append to the per-EvalState epoch log),
  3. `recordThunkDeps(v, epochStart)` (eval.cc:1694) records the range
     `[epochStart, epochEnd)` keyed by the thunk's `Value*`
     (`MemoReplayStore::recordThunkDeps`, memo-replay-store.hh:93 →
     `epochMap[&v] = DepRange{...}`).
- **Consequence: every forced thunk already has a precise, isolated dep range =
  exactly the deps that computation read.** The string thunk that computes a
  derivation's `outPath` already owns a clean `DepRange` of just that
  derivation's input observations. The 16 K `depTracker.scopes` are these
  transient capture frames; the flattening into 7 fat root traces happens when
  the root scope replays child ranges via `replayMemoizedRange`
  (dep-recording-context.hh:307-310).
- **So Lever 2 does NOT need to invent dep attribution at `derivationStrict`.**
  The per-derivation dep set already exists as an epoch range. What is missing is
  (i) *persisting* a derivation-keyed trace from that range and (ii) *looking it
  up* on re-eval to skip the re-force. This is a smaller change than §4's
  "open a recording scope" framing.

### Finding B — `derivationStrict` is the WRONG hook point; the thunk-force boundary is right

§2/§4 named `derivationStrictInternal` (primops.cc:1624) as the choke point. The
adversarial read shows the inputs are already forced by `forceAttrs(args[0])` at
primops.cc:1552 *before* `derivationStrictInternal` runs, and the result attrset
is returned directly (not behind a thunk the primop controls). So hooking inside
`derivationStrict` would capture deps recorded AFTER the expensive input force —
too late to skip it. The reuse decision must happen at the **thunk that produces
the derivation value**, i.e. at the `forceThunkValue` boundary that already
snapshots `epochStart`, BEFORE the input attrs are forced. This realigns the
lever onto existing machinery: it is a specialization of the per-thunk epoch
capture, gated to "this thunk is a derivation".

### Hazard 1 — identifying the derivation BEFORE forcing it (the chicken-and-egg)

To skip forcing a derivation's inputs, we must recognize "this thunk will produce
derivation D" *without* forcing it — but the derivation's identity (input
content hash) is only known AFTER forcing the inputs. This is the core tension:
- The current cache keys a thunk by its `AttrPathId` (position), which IS known
  before forcing. A position-keyed derivation cache would work for "same attr
  path as last eval" but NOT for "same derivation reached at a new path" — and
  the outlier benefit needs the latter (a moved/renamed module shifts paths).
- Resolution options: (a) accept position-keying (`AttrPathId`) and get partial
  benefit — likely still large, since most of the 6,397 unchanged derivations
  ARE at stable paths commit-to-commit; (b) a two-phase scheme: force inputs
  once to get the identity, then short-circuit *downstream* re-derivation —
  but that doesn't save the input force, which is the cost. **(a) is the
  realistic first slice.** This also dissolves O1: position-keying means the
  EXISTING `AttrPathId` routing works unchanged, and Design A's synthetic `__drv`
  namespace is only needed if/when we pursue cross-path reuse (b).
- **This is the biggest correction to the lever:** the realistic, soundly-keyable
  win is "an unchanged derivation AT THE SAME ATTR PATH skips re-forcing its
  inputs" — which is just the existing per-`TracedExpr` trace working at
  derivation granularity. The question collapses back to: *why isn't the
  derivation's own `TracedExpr` trace already being reused?* Because there is no
  `TracedExpr` at the derivation — the closure has only 7. See Hazard 2.

### Hazard 2 — there is no `TracedExpr` at the derivation, so no per-derivation trace is recorded

Confirmed earlier (record.count=7). `TracedExpr` nodes are created only by
`materialize.cc` for attrset/list CHILDREN of a cached result. A derivation
reached deep inside a string computation is not an attrset child of any cached
node, so no `TracedExpr` wraps it, so no trace is recorded for it, so there is
nothing to reuse. **This is the actual root cause and the actual work:** make
the derivation-producing thunk a trace boundary (record a trace keyed by its
`AttrPathId`, with its epoch-range deps as the trace's deps), so next eval can
verify-and-skip it. Mechanism reuse: `recordThunkDeps` already has the range;
the new code path turns "thunk at a derivation" into a `publishTrace.publish`
with that range, and a verify-before-force lookup.

### Hazard 3 — soundness: a derivation value escaping via context (the keyset-escape analogue)

A served derivation result must re-verify if a consumer observed a non-output
facet (`meta`/`passthru`/`attrNames`). The epoch-range deps capture what the
derivation's OWN eval read, but NOT what consumers later read off the result
attrset. The facet mask (§3) is still required, and the keyset-escape tests are
still the soundness floor: a derivation trace consumed via `TraceValueContext`
must re-verify the context. Finding A does not remove this obligation.

## 8. Refined implementation sketch (mechanism-aligned)

Supersedes §4. Smallest sound slice, reusing existing machinery:

### Slice A' — record a trace at the derivation-producing thunk (position-keyed)
1. In `forceThunkValue` (or a TracedExpr-aware wrapper), detect when the thunk's
   result is a derivation (the result attrset has `type = "derivation"` / a
   `drvPath` attr — checkable right after `expr->eval`).
2. On that boundary, instead of only `recordThunkDeps(v, epochStart)`, also
   `publish` a trace keyed by the thunk's `AttrPathId` whose deps are the epoch
   range `[epochStart, epochEnd)` and whose `CachedResult` is the derivation
   result attrset (drvPath + outputs — small, NOT the 233 K-dep monster). This
   reuses `getOrCreateTrace` (content-addressed body dedup is automatic) and the
   existing `Sessions` routing.
   - **Why this is small:** the dep range and the result already exist in hand at
     this point; this is a `publish` call + a derivation-detect predicate, not new
     dep-tracking.
3. Gate behind a new setting (default off) so it is A/B-measurable and revertible.

### Slice B' — verify-before-force at the derivation thunk
1. Before forcing a derivation-producing thunk, look up its `AttrPathId` trace
   (existing `lookupCurrentNode` / verify path).
2. If it verifies (its epoch-range input deps all match current state) → serve
   the recorded result attrset, skipping the input force. This is where the
   per-derivation input-eval (~the 58 % `make-derivation` cost) is saved for the
   6,397 unchanged derivations.
3. Facet-mask gate (Hazard 3): only serve if consumer-observed facets verify;
   else fall back to fresh force. Reuse the StructuredProjection/keyset machinery
   for the mask (O2 — confirm it already fires on `drv.<attr>` access).

### Slice C' — measure, bounded
- Storage (Hazard/O4): position-keying means ~one trace per derivation-bearing
  attr path. Measure DB growth: if it approaches 6,419 × current blob size, the
  result MUST be the compact `{drvPath, outputs}` (small), NOT a fat dep blob —
  the deps live in the shared `DepKeySets`/epoch range, and the per-trace
  `values_blob` for a derivation result is tiny. Verify the blob size is small
  before scaling.
- Gate: 10-commit correctness (byte-identical) + `pairwise cold/<new> vs cold/1`
  on the outliers; the 77 fast commits must not regress from added recording.

### What changed from §4 (honesty ledger)
- §4 said "hook `derivationStrict`" → wrong; the hook is the thunk-force boundary
  (Finding B), and dep attribution is already done (Finding A).
- §2b's Design-A synthetic namespace is NOT needed for the first slice
  (position-keying via existing `AttrPathId` works — Hazard 1); it is deferred to
  a possible cross-path-reuse follow-on.
- The lever is therefore SMALLER than first sketched for the same-path case
  (reuse `recordThunkDeps`'s range + `publish` + verify-before-force), but its
  CEILING is also bounded: position-keying captures the same-path unchanged
  derivations (likely most of the 6,397) but not derivations that move paths.
- Still gated on sign-off: it touches `forceThunkValue` (the hottest eval path),
  so the fast-commit non-regression measurement (Slice C') is mandatory before
  it can land.

## 9. Decisive feasibility finding — Slice B' has a chicken-and-egg (verified)

A deeper read of the interception mechanism exposes a fundamental obstacle that
§8's Slice B' glossed over.

**How the cache intercepts before forcing:** `TracedExpr::eval` (the `Expr::eval`
override) is the ONLY pre-force hook — it runs verify → materialize-or-fresh
*instead of* the real evaluator, and it works ONLY because the value is a
`TracedExpr` thunk (`v.mkThunk(env, tracedExpr)`). Plain Nix thunks have
`expr = ExprCall/ExprAttrs/...` and `forceThunkValue` dispatches straight to the
real evaluator — no interception possible.

**The chicken-and-egg:** to verify-before-force a derivation (and thereby skip
its input force — the actual ~58 % saving), the derivation's thunk must BE a
`TracedExpr`. But `TracedExpr` thunks are installed ONLY by `materialize.cc`
(`installChildThunk`), and ONLY for attrset/list children of an
already-materialized cached result. A derivation reached deep inside a
string-producing computation is NOT an attrset child of any cached node — so
nothing installs a `TracedExpr` there, so there is no pre-force hook, so Slice B'
cannot intercept it.

**What Slice B' actually requires:** installing `TracedExpr` wrappers at
derivation-producing thunks *during the cold/prior eval* — i.e. intercepting
derivation thunk CREATION in the core evaluator (where `mkThunk` builds the
derivation's thunk), not in the eval-trace materialize layer. That is a far more
invasive change: it touches `EvalState`'s thunk allocation / `ExprCall` handling
for `derivation`/`derivationStrict`, threading a `TracedExpr` wrapper into a hot,
central evaluator path used by ALL evaluation, not just warm cache hits.

**Honest consequence:** Lever 2 is bigger than §8 implied.
- Slice A' (RECORD a derivation-keyed trace from the existing epoch range) is
  still feasible and small — but recording alone yields NO speedup (it is the
  passive-metadata trap the work-log already hit: a recorded trace that nothing
  consumes). Recording is only worth doing if B' can consume it.
- Slice B' (the speedup) needs core-evaluator interception to put a `TracedExpr`
  at the derivation, which is the genuinely hard, high-blast-radius part. The
  v53-era work and the CLAUDE.md "derivationStrict outside scope" note are
  consistent with this being deliberately avoided.
- **Alternative worth considering instead of evaluator interception:** rather
  than per-derivation `TracedExpr` wrapping, attack the SAME outlier via the
  existing 7-trace structure made finer at the ATTRSET boundaries that DO exist
  between the root and the string leaves (the `closures`/`gnome`/`<system>` attr
  path has a few real attrset levels). But the diagnostic showed those levels are
  shallow (the cost is below them, in the string computation), so this likely
  does not reach the derivation cost. Confirm with a per-attr-level trace count
  before pursuing.

**Revised recommendation.** Do NOT start Lever 2 as a quick prototype. The
recording half is cheap but inert; the consuming half requires core-evaluator
surgery (TracedExpr at derivation creation) that is the largest-blast-radius
change considered in this whole research arc. Before any build, the decisive
cheap experiment is: **a throwaway spike that installs a `TracedExpr` wrapper at
derivation thunks and counts how many derivations become cache-verifiable** —
measuring feasibility + the fast-path non-regression on the 77 fast commits —
WITHOUT yet wiring full verify/serve. If that spike shows the evaluator
interception is tractable and non-regressing, proceed; if it perturbs the hot
path measurably (likely, given the vptr-in-hot-loop lesson from the
rearchitecture reversal), Lever 2 is not worth it and the cold tail stays a
known, bounded cost (23 outlier commits, soundness-correct, just slow).

This is the most important finding of the adversarial pass: **the lever's
speedup half is gated on core-evaluator interception, not an eval-trace-layer
change — reclassifying Lever 2 from "large but localized" to "high-blast-radius
core-evaluator work," and strengthening the case to leave the cold tail as-is
unless a cheap feasibility spike proves otherwise.**

## 10. Change-locality structure (2026-05-30, measured) — sharpens the target AND the obstacle

Inspected WHAT the 22 changed derivations are (one-module `autossh-ng` change,
outlier `9b9f7241` vs predecessor `d9e9954c`):

- **All 22 changed derivations are the top of the DAG** — `nixos-system` (the
  root) + its direct system-assembly constituents: `activate`, `etc`,
  `system-path`, `system-units`, `user-units`, `set-environment`, `dbus-1`,
  `unit-*.service`, `X-Restart-Triggers-*`, plus doc derivations
  (`options.json`, `nixos-manual-html`, `nixos-help`). **Zero packages changed.**
  These are exactly the outputs of the NixOS module fixpoint.
- **The 6,397 unchanged derivations are the dependency closure below** — packages,
  patches, build inputs (sampled: hundreds of `*.patch`, package drvs). This is
  the cacheable bulk and aligns with the ~58 % `make-derivation`/pkgs flamegraph
  cost.

**What this sharpens:**
- The localization opportunity is real and well-shaped: a change ripples through
  a THIN system-assembly cap (22 drvs = fixpoint outputs) while the BROAD package
  base (6,397 drvs, ~58 % of cost) is untouched. Reusing the package base is the
  prize.
- **But it confirms §9's obstacle is exactly where the value is.** The 6,397
  unchanged package derivations are reached THROUGH the module fixpoint's
  evaluation (`system-path` forces `environment.systemPackages` → each package's
  `mkDerivation`). To skip re-forcing a package, interception must happen BEFORE
  the fixpoint pulls it in — i.e. the core-evaluator `TracedExpr`-at-derivation
  hook (§9), on the hot path. The cheap boundaries (attrset levels between root
  and string leaves) are ABOVE the fixpoint and do not localize this.
- Corollary: the ~25 % module-fixpoint cost is an unavoidable floor (the 22
  changed drvs ARE its outputs; it must re-run to produce them). So the realistic
  ceiling stands at ~halving the outlier, consistent with §O5.

**Net:** the target is confirmed (reuse the 6,397-package base) and so is the
single hard obstacle (core-evaluator interception to verify-before-force a
package derivation pulled in by the fixpoint). No cheaper boundary reaches it.
The decision is unchanged: the next step is the hot-path feasibility spike (§9),
which requires sign-off; absent that, the cold tail stays as-is (bounded,
soundness-correct).

## 11. FEASIBILITY SPIKE (2026-05-30, branch `spike/lever2-derivation-feasibility`) — MATERIAL BLOCKER HIT

Built incrementally, measurement-first. Result: **hit a material architectural
blocker; the spike stops here.** Findings are durable; the code is throwaway.

### Step 1 (landed, measured): how many derivations does a traced GNOME eval evaluate?
Added throwaway counters `nrSpikeDrvStrictTraced/Untraced` at `prim_derivationStrict`
(guarded on `Counter::enabled`, off the hot inner loop), emitted under
`evalTrace.spike`. Built (meson, clang) and ran `nix eval -f release.nix
closures.gnome.x86_64-linux` with `NIX_SHOW_STATS`:
- **`drvStrictTraced = 10,455`** derivation evaluations on the eval path
  (`drvStrictUntraced = 0`).
- Same run: **`record.count = 6`** traces, `depTracker.scopes = 8,183`,
  `hits=0 misses=6`.
- So **~10,449 derivations evaluated with NO recorded trace.** Confirms the
  Lever-2 target size empirically (vs the 6,419 *distinct* instantiated drvs —
  the ~1.6× gap is within-eval repeat `derivationStrict` calls that Nix's
  `drvHashes` dedups at the .drv-write level but still re-invokes).

### Step 2 (blocked): can a derivation thunk become a cache-routed `TracedExpr`?
To verify-before-force a derivation (the actual speedup), its thunk must be a
`TracedExpr` (the only pre-force interception hook, §9). Reading the identity
model (`traced-expr.hh:100-184`, `trace-session.cc::makeChild`):

- `TracedExpr` has exactly two kinds: **Root** (one per session, `pathId=0`,
  value from `rootLoader`) and **Child** (`parentExpr` + `Symbol name` +
  `pathId = extendPath(parent.pathId, name)`).
- A Child's `navigateToReal()` **walks up the parentExpr chain and re-traverses
  the real root through named attr/list selectors** (`traverseRealTree`). Its
  whole identity is *a position in the attr-path tree rooted at the eval root.*
- A `derivationStrict` thunk is created deep inside `make-derivation.nix` via
  function application / `let` / `map` / the module fixpoint
  (`derivation.nix`: `strict = derivationStrict drvAttrs`). It has **no
  parent-chain of `TracedExpr`s and no attr-path of named selectors from the
  root.** `makeChild` cannot be called for it (no parent `TracedExpr`, no name,
  no path); `navigateToReal` could not reconstruct it.

**THE MATERIAL BLOCKER (architectural, not incidental):** the entire `TracedExpr`
cache is premised on cacheable nodes being addressable by an **attr-path from the
evaluation root**. Derivations have no such identity — they are values produced
by arbitrary computation. Caching them requires a SECOND, content-addressed
`TracedExpr` identity model (parentless; keyed by derivation-input hash;
`navigateToReal` replaced by "re-invoke `derivationStrict`"; recording keyed by
that hash instead of `pathId`), threaded through evaluator thunk creation in
`derivation.nix`/`ExprApp`. That is a foundational redesign of `TracedExpr`'s
identity, not a localized prototype — and it lands on the hottest evaluator path.

This is consistent with, and now concretely explains, the CLAUDE.md note that
`derivationStrict` is "outside the plan's stated scope": the cache's addressing
model structurally excludes non-attr-path values.

### Verdict
- The cold tail (23 outlier commits, ~halving ceiling) is real but its fix
  requires a content-addressed `TracedExpr` identity — a foundational change to
  the cache's addressing model, on the hot eval path, with the v53/vptr precedent
  warning that hot-path perturbation tends to eat such wins.
- **Recommendation: do NOT pursue Lever 2 as currently scoped.** Leave the cold
  tail as a known, bounded, soundness-correct cost. If revisited, the prerequisite
  is a design for content-addressed (derivation-keyed) trace nodes — a separate
  RFC-scale effort, not an incremental lever.
- The Step-1 counter is the only code; it is measurement-only and will be reverted
  (kept on the throwaway branch for reproducibility).
