# Eval-Trace Review: Current Unstaged Diff

This review is based on the actual unstaged changes in the worktree, not just the prior
summary. The main conclusion is that the previous summary was directionally right about
some narrow points, but incomplete as a review of the whole diff.

## Overall assessment

The worktree does include the advertised `listIdentityMap` removal and debug cleanup, but
those are not the only meaningful changes:

- The old `origExpr` / sibling-wrapping path has been removed.
- Real-tree navigation now registers sibling identity without mutating real-tree cells.
- Sibling dep tracking changed shape to include retry/fallback logic for untraced siblings.
- A new functional regression test was added for the `asciidoc.nativeBuildInputs` case.
- An unrelated `search.cc` change is still mixed into the same review surface.

Because of that, the prior summary understated both the scope of the changes and the
remaining risk areas.

## What the summary got right

### 1. `listIdentityMap` is gone

That part is accurate.

- `context.hh`: `listIdentityMap` is removed.
- `context.cc`: list lookup/reset logic is removed.
- `trace-cache.cc`: list registration is gone; `registerBindings` now early-returns for
  non-attrsets.

The two list-via-copy tests were intentionally flipped from `EXPECT_TRUE` to `EXPECT_FALSE`:

- `PointerEquality_ViaCopy_ListWithFunction_Aliased`
- `PointerEquality_ViaCopy_SmallListWithFunction_Aliased`

The new negative tests for the old false-positive case are also present:

- `PointerEquality_ListIdentity_SameFirstElem_DifferentSecond`
- `PointerEquality_ListIdentity_SameFirstElem_DifferentLater`
- `PointerEquality_ListIdentity_SameFirstElem_DifferentLater_ViaCopy`

### 2. The debug cleanup is real

The noisy platform-specific debug scaffolding has been removed from:

- `src/libexpr/eval.cc`
- `src/libexpr/eval-trace/context.cc`

That part of the summary matches the diff.

### 3. The new pointer-equality coverage is meaningful

The added tests around:

- same-parent alias recovery,
- command-path equality,
- and list false positives

do cover real failure shapes that were not previously exercised.

## Where the previous summary was inaccurate or incomplete

### 1. It did not describe the biggest architectural change

The summary presented the work mostly as cleanup around list identity and logging. The
actual diff also removes the old `origExpr` / sibling-wrapping model and replaces it with
“register real siblings, do not mutate the real tree.”

That is visible in:

- `src/libexpr/eval-trace/cache/traced-expr.hh`
- `src/libexpr/eval-trace/cache/materialize.cc`
- `src/libexpr/eval-trace/cache/trace-cache.cc`

This is a material behavior change, not just cleanup. Any review should say so explicitly.

### 2. “GC safety unchanged” is too optimistic

The raw-pointer problem remains unresolved, but the risk surface changed.

`SiblingIdentity` still stores raw:

- `TracedExpr * tracedExpr`
- `TracedExpr * parentExpr`

in maps that are not GC-traced. Those pointers are now used in more places:

- equality recovery in `EvalTraceContext::haveSameResolvedTarget`
- sibling dep recording in `SiblingAccessTracker`

Since `navigateToReal()` now registers more real siblings into `siblingIdentityMap`, pointer
reuse is no longer just an equality concern. It can also perturb sibling dep isolation.

So the right conclusion is not “unchanged”; it is “still unresolved, and now relevant to
more code paths.”

### 3. “First-touch sibling precision loss unchanged” is misleading

The tradeoff still exists, but the implementation around it changed:

- `SiblingAccessTracker` now carries `hasUntracedAccess` and `untracedSiblings`.
- `appendParentContextDeps` now retries missing sibling hashes and falls back when needed.
- `StandaloneSC_CrossScope_KeyChange` no longer insists on zero Content deps.
- `StandaloneIS_NestedValueChange_Survives` now forces `d` explicitly so it gets a stored trace.

That is not a no-op. The tradeoff may still be accepted, but the review should describe it
as an active behavioral compromise with new machinery around it, not as “unchanged.”

### 4. It omitted the added functional regression test

The diff adds a substantial new regression test in:

- `tests/functional/eval-trace-impure-regression.sh`

This test models the `asciidoc.nativeBuildInputs` / `pythonImportsCheckHook` case more
faithfully than the earlier boolean-only reproducer. That is important and should be part
of the review narrative.

### 5. It omitted the unrelated `search.cc` change

The worktree still includes:

- `src/nix/search.cc`

That change should remain split from the eval-trace review surface.

## Additional causes for concern

### 1. Stale comments remain in multiple places

There are still references to the removed model:

- `src/libexpr/include/nix/expr/eval-trace/deps/recording.hh`
  still says `SuspendDepTracking` is used in `ExprOrigChild::eval()`.
- `tests/functional/eval-trace-impure-advanced.sh`
  still describes the old `origExpr` model in several test headers.
- `src/libexpr-tests/eval-trace/traced-data/materialization/pointer-equality.cc`
  still documents failures in terms of `resolveClean()` / contamination from wrapping,
  even though the implementation has moved away from that mechanism.

These comments now misdescribe the code and should be cleaned up before relying on the
review text as durable documentation.

### 2. There is a coverage gap around mixed sibling states

The new first-touch fallback logic handles:

- already-traced siblings,
- and fully untraced siblings,

but I do not see a focused test for the mixed case where one sibling already has a trace
hash and another does not. That looks like the most obvious missing precision/soundness
test around the new fallback logic.

### 3. The review should not claim broad verification it does not show

The previous version included broad claims about full test counts and benchmark results.
Those claims were not derivable from the current diff itself and should not remain in a
review file unless they are reproducibly tied to the current worktree state.

## Cleanup and improvement areas

In priority order:

1. **Describe the full behavioral change honestly** — DONE
   Updated comments to remove `origExpr`/`ExprOrigChild` references and describe
   the current real-sibling registration model.

2. **Tighten the GC-safety language** — DONE
   `haveSameResolvedTarget` comment in `context.cc` now says Tier 1/2 compare
   pointers without deref (won't crash, but pointer reuse could cause false
   positives). Tier 3 deref is explicitly labeled not GC-safe.

3. **Add a mixed sibling-state test** — DONE
   Added `MixedSiblingState_TracedAndFirstTouch` and
   `MixedSiblingState_BothPreforced_PerSibling` in `standalone-verify.cc`.

4. **Clean stale comments** — DONE
   - `deps/recording.hh`: Updated `SuspendDepTracking` to reference
     `haveSameResolvedTarget` Tier 3 instead of `ExprOrigChild::eval()`.
   - `tests/functional/eval-trace-impure-advanced.sh`: All `origExpr` and
     `ExprOrigChild` references replaced with current terminology.
   - `pointer-equality.cc`: Group 12 rewritten for three-tier equality model.
     Group 13 rewritten for same-parent alias case. In-test contamination
     comments updated. Group 8 made explicit about accepted regression.

5. **Split `search.cc`**
   The `baseAttrPath` change (2 lines) remains in the unstaged diff. Should be
   committed separately from the eval-trace changes.

6. **Be explicit about the accepted regression** — DONE
   Group 8 in `pointer-equality.cc` now explicitly documents the list-via-copy
   regression, explains why `listIdentityMap` was removed (false positives from
   first-element keying), and notes the fix is blocked on a safer identity model.

## Verification notes for this review

What was verified:

- `nix build -L .` succeeds for the current worktree.
- 123 relevant `nix-expr-tests` pass (2 new mixed-sibling tests, 65 pointer
  equality tests, 24 per-sibling tests, 12 child-range exclusion tests,
  11 dep stability tests, 9 standalone/materialization tests).

Remaining:

- The `search.cc` change should be split into a separate commit.
- Full functional test suite (`eval-trace-impure-regression.sh`,
  `eval-trace-impure-advanced.sh`) not run outside Meson test harness.
