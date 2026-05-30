# Lever 1 — observed-key / unobserved-change pruning, sound by construction

**Status:** DESIGN DRAFT — **PARTIALLY REDIRECTED by the 2026-05-30 diagnostic
(§0). Read §0 first.** The Step-1 diagnostic has run; its evidence falsifies
this draft's original assumption that by-name directory churn drives the cold
outliers. Lever 1 as written (enumerated-set pruning) is **not** the main prize
on the `closures.gnome` workload. No production code. 

## 0. Step-1 diagnostic results (2026-05-30, `cold-stats/2`/`hot-stats/2`, HEAD `9f7311129`)

Ran `generate --with-stats` (writes to `cold-stats/<n>` / `hot-stats/<n>` — a
separate namespace, so stats never contaminate clean wall runs) + `classify`.
**This redirects the lever.**

What the 23 cold outliers actually are (`classify` B0–B4):
- They split into `partial-miss` (15 commits, 11.75 s mean, 47.6% hit rate,
  **55% recovery failure**) and `full-miss` (7 commits, 12.14 s, 36.7% hit,
  **100% recovery failure**). The 78 fast commits are `hit-only`/`no-recovery`
  at ~1.0–1.8 s.
- **`nrThunks` is the smoking gun:** outliers evaluate **~20.9M thunks vs ~268K**
  for fast commits — a **78× re-eval blowup**. The cost is dominated by the
  **re-evaluation itself** (CPU 13–14 s), NOT trace machinery: the miss-cost
  decomposition shows recovery 329–615 ms, structural-variant 170–380 ms, verify
  ~580–844 ms — all *milliseconds* against *seconds* of miss cost.
- **Hit-path distribution:** primary cache **0%**; DirectHash recovery 98.4%,
  StructVariant recovery 37.7%, history bootstrap 67.2%. Even fast commits are
  served via *recovery*, never primary. The outliers are where recovery *fails*
  and cascades to full re-eval.

What the outlier commits actually change (checked against nixpkgs):
- Worst outlier `9b9f7241` (17.6 s): touches **only `nixos/.../autossh-ng.nix`**
  — one NixOS module, **zero by-name files, not even a package**. Yet it
  re-evals ~20M thunks.
- Others are mixed: `8824e633` (element-call bump, 2 by-name files),
  `dfbd61a9` (streamz bump, **0 by-name**), `b1f9d94c` (opencode bump, 1 by-name).
- **The workload is `closures.gnome` = 2 NixOS *system closures*
  (`aarch64-linux`, `x86_64-linux`).** A change anywhere reachable by the GNOME
  system closure (a module, a transitively-included package) defeats recovery
  for that whole closure and forces near-full re-eval.

**Conclusion / redirect:** the outliers are **recovery failures on the deep
NixOS system-closure structure**, not enumerated-set (by-name / `#keys`) churn.
The original §1–§6 framing (prune coarse `DirectoryEntries`/`#keys` when only
unobserved members changed) does **not** address `9b9f7241` — there is no
enumerated set whose unobserved member changed; a module deep in the closure
changed and the recovery path couldn't localize it. This points at **lever 2
(semantic derivation-boundary / closure-localization)**, not lever 1, as the
outlier lever for this workload.

**Open question — RESOLVED 2026-05-30 (stats.json drill-down on `9b9f7241`).**
The two hypotheses were:
1. **Over-coarse top-level dep** (one `nixos/modules` listing any edit
   invalidates → enumerated-set lever at module-set granularity).
2. **Recovery can't localize** (precision gap; re-evals more than the changed
   subtree).

**Verdict: H1 REJECTED, H2 CONFIRMED and sharpened into a distinct lever.**

Evidence from the outlier's `cold-stats/2` stats.json:
- The eval records **16,352 trace scopes** (`depTracker.scopes`) but verifies
  only **7 at the top level** (`loadTrace.count=7`, `verify.count=7`). Those 7
  are the monolithic `closures.gnome` roots (2 system closures + sub-attrs),
  averaging **~1,278 thunks each** (20.9M / 16,352).
- **`verify.failed=222` of `verify.depsChecked=233481` = 0.10%.** Those 222
  failed deps are spread (not one coarse listing → H1 rejected;
  `depTracker.ownDepsMax=48738` shows fine-grained per-trace deps), and they
  fail **4 of the 7 roots**.
- Each failed root attempts recovery (DirectHash 3 hits; the 4 failures fall
  through gitIdentity @ 294 µs and structVariant @ 0.48 s) and **all 4 fail** →
  each re-evals its **entire** subtree. Machinery is cheap (recovery 0.75 s +
  verify 0.99 s); the **~13 s is raw re-eval** (`cpuTime` 14.6 s).
- Contrast the fast commit `f37d`: 7/7 roots verify clean, **`nrThunks=1`**,
  `verify.failed=0`, 0 recovery attempts. Binary per root: verify clean → ~free;
  one dep fails under a root → whole root re-evals.

**Root cause (CORRECTED 2026-05-30 after checking `record.count`): the cache
records only ~7 COARSE traces for the whole `closures.gnome` eval — there is no
fine-grained sub-trace granularity to reuse.** Earlier wording here ("16K
recorded sub-traces are unreachable") was WRONG. The numbers: the full-cold
first commit has `record.count=7` against `depTracker.scopes=16357`. The 16,352
scopes are *transient `DepCaptureScope` frames* that open/close during the deep
force and **collapse into the 7 recorded traces** — they are NOT independently
addressable sub-traces. So a change touching 0.10% of deps pays a full closure
re-eval because the cache's recording granularity is the whole closure root: a
single failed dep invalidates one of only 7 monolithic traces, and there is
nothing finer recorded to fall back to.

**This is neither lever 1 (enumerated-set pruning) nor classic lever 2
(derivation-boundary digest). It is a THIRD lever: incremental sub-trace reuse /
partial recovery** — on root-trace verify failure, descend and re-verify only the
changed sub-traces instead of re-evaluating the whole root. See the redesign-plan
2026-05-30 "Step-1 diagnostic — RESOLVED" section.

### Design-question 1 RESOLVED (2026-05-30, code feasibility study)

*Why aren't the 16,352 recorded sub-traces independently verifiable on the warm
path, and can a failed root re-enter per-child cache lookups?*

Traced through `trace-session.cc` + `materialize.cc`:
- **Cache-routing = `TracedExpr` child thunks.** A child verifies per-leaf (the
  OR-3 contract) ONLY when its `Value` is a `TracedExpr` thunk. Those are
  installed by `installChildThunk` → `TracedExpr::makeChild`, called from
  **`materialize.cc` only** (lines 414/486), i.e. only when a trace VERIFIES and
  its `CachedResult` is materialized — iterating the *cached* `attrs->entries`.
- **A failed root produces plain thunks.** Root verify miss →
  `evaluateFresh` (trace-session.cc:236-239) → `getRealRoot()` →
  `rootLoader()` (trace-session.cc:448-455) = the **plain Nix evaluator**. Its
  attrset children are ordinary Nix thunks, NOT `TracedExpr`. The benchmark's
  `--json` deep-forces the whole structure with the ordinary evaluator, which
  **never touches the cache** — so the 16K sub-traces are unreachable not because
  they're un-verifiable but because the *values aren't cache-routed*.

**Verdict (CORRECTED): Lever 5 is a RECORDING-granularity change, not a
warm-path reuse change.** The original framing assumed 16K recorded sub-traces
existed to reuse; they don't (record.count≈7). So the lever is two-sided:
1. **Record finer:** the cold pass must record more than 7 coarse traces — it
   must persist addressable sub-traces at intermediate attr-path nodes
   (`makeChild` already builds the pathIds; the question is which nodes get a
   *recorded trace* vs collapse into a transient `DepCaptureScope`).
2. **Reuse finer:** on root verify miss, wrap the fresh `rootLoader()` attrset's
   children as `TracedExpr` thunks (same `makeChild`/`installChildThunk`, driven
   by fresh keys) so each child force re-enters the cache and warm-hits its own
   recorded sub-trace.

Both are needed: (2) without (1) finds nothing to hit; (1) without (2) records
sub-traces that the fresh-walk still bypasses.

The likely reason only ~7 traces record today (to confirm next): a trace is
recorded per `TracedExpr` that is *forced through the cache*, and on the cold
pass only the 7 roots are `TracedExpr`s — the deep `--json` force of each root
runs the ordinary evaluator over plain thunks, opening 16K transient
`DepCaptureScope`s that all fold into the one root trace. So **finer recording
ALSO requires installing `TracedExpr` children on the cold pass**, not just the
warm path. That makes (1) and (2) the same mechanism applied in both passes:
wrap attrset children as `TracedExpr` whenever an attrset is produced (cold) or
re-entered (warm-miss).

Open sub-questions (the NEXT step, needs prototyping + measurement):
1. **Granularity/overhead tradeoff.** Recording a trace per intermediate node
   across a NixOS system closure could be a LOT of traces (the 16K scope count
   is the upper bound). Recording + storing 16K traces/commit has cold-write and
   DB cost — the v53 capsule rewrite failed exactly by storing too much. What
   intermediate granularity captures the reuse win without exploding storage?
   (e.g. record at module / derivation boundaries, not every attr.) This ties
   Lever 5 to lever 2's derivation-boundary idea.
2. **`rootLoader` shallow-force.** To wrap children we force the fresh root one
   level (enumerate keys) without deep-forcing. Is a shallow force cheap, or does
   the `closures.gnome` root force children eagerly? (Measure: shallow
   `forceValue(root, noPos)` thunk count.)
3. **pathId stability.** A re-entered child must get the same `AttrPathId` the
   cold pass recorded under. Confirm `makeChild`'s parent-chain pathId is stable
   across cold-record vs warm-fresh-reentry.
4. **Soundness vs the failed parent (design-q 2).** A child served while its
   parent failed verify: validity must rest on the child's OWN deps. Exactly the
   keyset-escape / cross-trace concern — a child depending on a parent-mediated
   value (TraceValueContext / ParentSlot) must still re-verify that context. An
   analogous "parent-failed, child-served" test is owed.
5. **Recursion.** Naturally recursive if every `TracedExpr` child wraps its own
   children on force — confirm it falls out for free.

Cost model: machinery is ~ms/trace; reusing N unchanged sub-traces at ~50 µs
each must beat the ~13 s re-eval. BUT the recording side adds cold-write cost for
the finer traces, and we have NO datapoint for how many sub-traces a real outlier
would verify (only 7 roots are entered today). Both the reuse win AND the
recording cost must be measured on a prototype before committing — this is the
gating experiment, and it is squarely a hot/cold-path build (needs sign-off).

The §1–§7 below are **retained as the enumerated-set design** (still valid IF
hypothesis 1 holds at module-set granularity, and still the right home for the
2a `#keys` / keyset-downgrade work), but they are no longer claimed as the
outlier fix until a follow-up diagnostic (per-commit `logs` on an outlier)
resolves the open question above.

---

**Goal.** Reduce the cold-eval tail: authorize serving a cached result when only
*unobserved* members of an enumerated set changed — unobserved attrset bindings,
or unobserved `pkgs/by-name/` directory children. This is the single biggest
lever in the prior research (work-log v6→v11: cold 3.32→1.88 s, hot 0.88→0.38 s),
but it must be rebuilt here: it does **not exist in the current tree**.

**Goal.** Reduce the cold-eval tail: authorize serving a cached result when only
*unobserved* members of an enumerated set changed — unobserved attrset bindings,
or unobserved `pkgs/by-name/` directory children. This is the single biggest
lever in the prior research (work-log v6→v11: cold 3.32→1.88 s, hot 0.88→0.38 s),
but it must be rebuilt here: it does **not exist in the current tree**.

**Authoritative context:** `doc/eval-trace-cache-README.md` (canonical lever
list), `doc/eval-trace-cache-redesign-plan.md` (2026-05-29 CORRECTION finding 2;
2026-05-30 Ledger-D baseline). Soundness scaffolding already landed:
`plans/keyset-provenance-differential-harness.md`,
`plans/keyset-downgrade-sound-by-construction.md`,
`src/libexpr-tests/eval-trace/store/keyset-escape.cc`.

---

## 1. What the baseline says this is worth (be honest about the prize)

Ledger-D (HEAD `9f7311129`, 100-commit `closures.gnome`):

- cold **mean 3.72 s**, **median 1.11 s** — bimodal. ~23 commits at 7–17.6 s
  (worst +1481% over median); the other ~77 at ~1.0 s.
- hot flat ~0.95 s.

So lever 1 is a **tail-rescue, not a median or hot mover**. Best realistic
outcome: collapse the ~23 outliers toward the ~1.1 s median → cold mean roughly
3.72 → ~1.3 s. The median and hot are untouched. **This must be stated up front
in any go/no-go**: a sound-by-construction rebuild is justified only if halving
the cold *mean* by killing outliers is worth it for the intended consumers
(`nix-eval-jobs`, CI eval), since the typical-commit and hot experience does not
change.

**Gate before committing to build:** the Step-1 diagnostic must confirm the
outliers are actually dominated by the phase this lever addresses (structural-
variant recovery / dep-hash over enumerated sets), not by record/writeback or
whole-file `FileBytes` re-eval (which would redirect to lever 2 or a writeback
lever). Do not build on the prior lineage's diagnosis — that was a different,
abandoned code state.

## 2. Two facts that reframe the work vs the work-log

1. **It is greenfield, not "lift it out of command-JSON."** The v6→v11 proof
   lived in the `nix eval --json` action-cache layer (`libcmd`
   `tryServeJsonOutput` / `JsonInstallableOutput` / `proof-json-action-*`),
   **removed 2026-05-27**. A code search of the current tree finds none of the
   observed-key / by-name / construction-only machinery. So this is a fresh
   `libexpr`-transparent implementation, not a refactor. (Verified 2026-05-30.)

2. **Two sub-levers, only one of which the existing keyset doc covers.**
   - **2a — attrset `#keys`** (`StructuredProjection`, `ShapeSuffix::Keys`,
     recorded at `maybeRecordAttrKeysDep`, `deps/shape-deps.cc:79`). The
     construction-only-vs-result-visible pruning of this is exactly
     `plans/keyset-downgrade-sound-by-construction.md`. **Reuse that design; do
     not duplicate it here.**
   - **2b — by-name `DirectoryEntries`** (recorded via
     `deps/input-resolution.cc`; resolved in `dep-resolution-service.cc` /
     `verifier.cc`). This is the nixpkgs `pkgs/by-name/<shard>/<pkg>/` case and
     is the likely driver of the cold outliers (adding/removing a package
     changes the directory listing → whole-listing `DirectoryEntries` mismatch →
     re-eval, even though the demanded package's own subtree is unchanged).
     **No existing doc covers 2b. It is the net-new design surface of lever 1.**

## 3. The soundness problem (same shape for 2a and 2b)

Authorize reuse when the enumerated set (attr key set / directory listing)
changed *only* in members the result never observed. The danger, proven by the
keyset-escape tests and by the build-system survey (redesign-plan 2026-05-29
scoped-research section): **no production build system (Bazel, Buck2, Shake)
does unobserved-member-changed reuse** — they all invalidate on any
directory-entry-set change. So the soundness burden is entirely ours and there
is no battle-tested precedent. The whole proof reduces to a provenance
distinction with zero external validation:

> A "construction-only" enumeration (a listing/keyset used only to *build* an
> index/structure, never to drive a result-visible decision) is distinguishable
> from a "result-visible" enumeration (one whose membership reaches the result,
> control flow, a stringify/hash, `attrNames`, negative membership, or another
> cached trace).

Fail-closed default: treat any enumeration as result-visible (keep the coarse
dep) unless proven construction-only. The cross-trace escape
(`keyset-escape.cc`) is the residual that makes a single-trace reachability
check unsound — a `#keys` / listing can escape via `TraceValueContext` /
`TraceParentSlot` to a consumer that does not exist at the producer's
finalization. **The 2b directory case needs its own escape test** before any
prune (see §6).

Hard boundary from the work-log (lines 969–974), carried forward: the proof is
sound ONLY for the observed-member universe. `attrNames`, missing-attr
suggestions, and complete negative membership are NOT covered and must stay
fallback (keep the coarse dep). This caps how far 2a/2b can go.

## 4. Design sketch (sound by construction)

Mirror the keyset-downgrade design (sealed-capability prune at the recording /
finalization seam), extended to directory listings:

- **Taint at observation.** When an enumeration is forced, tag whether its
  membership flows to a result-visible sink. Default = result-visible. A
  whitelist of provably-safe consumers (start with: index construction where
  the built structure is itself only navigated by observed key — the
  `pkgs/by-name` lookup shape) may downgrade to construction-only.
- **Prune at finalization**, where both the captured deps and the `CachedResult`
  are in hand (the keyset-downgrade doc's seam, `trace-session.cc:164-168`):
  replace a construction-only coarse dep (`#keys` / `DirectoryEntries`) with the
  finer per-observed-member deps (point `StructuredProjection` / per-child
  `DirectoryEntries` on the observed subdirs only). If no finer deps were
  recorded → not prunable → keep coarse.
- **Sealed `…LivenessReview` capability** is the only authorizer of a prune; its
  factory enforces (construction-only ∧ not cross-trace-escaped ∧ finer-deps-
  exist). Fail-closed everywhere else.
- **Transparent to `libexpr`:** the prune operates on recorded deps, so every
  consumer (`nix-eval-jobs`, `nix eval`, CI) benefits — unlike the removed
  command-JSON layer.

The directory (2b) specifics to resolve *after* the diagnostic:
- What is the finer dep for a by-name listing? Candidate: a per-observed-child
  `DirectoryEntries`/existence dep on the specific `<shard>/<pkg>` paths the
  eval actually descended into, plus a "no *observed* sibling added/removed"
  guard — NOT the whole-shard listing.
- Where is the by-name listing forced, and is its membership ever
  result-visible (e.g. `builtins.readDir` of the shard reaching the result vs.
  used only to resolve one package path)? This determines whether the
  construction-only tag is ever justified for real nixpkgs `by-name`.

## 5. Open questions the diagnostic (Step 1) must answer before §4 is real

1. Are the 23 outliers dominated by structural-variant recovery / dep-hash over
   enumerated sets (→ lever 1 applies), by record/writeback (→ different lever),
   or by whole-file `FileBytes` re-eval (→ lever 2)?
2. Of the outliers, how many are attrset-`#keys` (2a) vs by-name-directory (2b)
   driven? (Determines whether the existing keyset doc covers most of the prize
   or whether 2b is the bulk.)
3. Does `sv` report a meaningful early-exit opportunity on these commits?
4. Is the by-name listing membership ever result-visible in real `closures.gnome`
   eval, or is it provably construction-only? (If the latter is rare, the prune
   rarely fires and the lever is not worth it.)

## 6. Test obligations (extend the existing harness; tests lead)

Before any prune lands:
- A **by-name `DirectoryEntries` escape test** (analogue of `keyset-escape.cc`):
  a directory listing that escapes via `TraceValueContext`/`ParentSlot` must
  invalidate a consumer on an *observed*-child change, and (the prune's win)
  hit on an *unobserved*-child add/remove.
- A **value-comparing differential** (the harness's
  `Oracle::servedStale` deep-forced comparison, NOT a path counter — the counter
  is confounded by per-leaf lazy re-derivation) over by-name churn: add/remove an
  unobserved sibling package, assert served == post-mutation ground truth.
- The failure-mode catalog (redesign-plan 2026-05-29): missing dependency edge,
  untracked empty-dir existence, watcher under-report, symlink-following set
  expansion, child-state independent of parent. Each is a real documented build-
  system bug; our by-name slicing must be tested against all five.

## 7. Recommended sequence (each gated by 10-commit correctness + pairwise vs Ledger D)

1. **Step 1 diagnostic (in flight):** `--with-stats` run + `classify`/`sv` to
   confirm the outlier phase and the 2a/2b split. **Decision gate.**
2. If 2a dominates → execute the existing keyset-downgrade design; this doc's 2b
   work is deferred.
3. If 2b dominates → design the by-name finer-dep + construction-only tag
   (§4 directory specifics), write the §6 tests first, then prototype.
4. Prototype the sealed-capability prune; enable the keyset red pin; gate on
   correctness + `pairwise hot/2b vs hot/1` and `cold/2b vs cold/1` paired
   medians (need 2–3 baseline repeats first to establish noise bands).
5. Revert if the gate fails or the keyset-escape / by-name-escape tests go red.

**Do not start the prototype (steps 3–4) without the Step-1 evidence and explicit
sign-off** — it touches the hot recorder/finalization path for a tail-only prize.
