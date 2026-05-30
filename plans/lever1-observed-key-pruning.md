# Lever 1 — observed-key / unobserved-change pruning, sound by construction

**Status:** DESIGN DRAFT. No production code. Companion to the diagnostic run
(Step 1 below) that must land before any prototype.

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
