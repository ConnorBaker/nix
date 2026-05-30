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

- **O1 (keying, the big one):** derivation traces need content-addressed,
  path-independent keys, unlike today's `AttrPathId` model. Does the existing
  trace store support a non-attr-path key kind, or is this a schema change?
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
  currently excludes it), likely needs a new trace key kind (O1), and risks the
  v53 storage-explosion failure (O4). It is not a small prototype.
- It is, however, the lever the evidence points to for the *only* measured pain
  on this workload (the cold outliers). Lever 1 (enumerated-set) and the original
  Lever 5 (attrset sub-trace) do not fit `closures.gnome`'s computation-heavy,
  data-shallow shape.
- **Cheapest de-risking step before any build: O5** — one more `--with-stats`
  run + per-commit `logs`/profiler on outlier `9b9f7241` to confirm the 20 M
  thunks are derivation evaluation and that a handful of derivations actually
  changed (vs. a module-fixpoint blow-up that no derivation boundary localizes).
  Do this before committing to Slice A. Needs sign-off for the build itself.
