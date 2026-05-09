#!/usr/bin/env bash

# Soundness tests for eval-trace: each test verifies that a specific class of
# dependency is correctly tracked so that cache invalidation is never silently
# skipped.  Tests are ordered by severity; Test 1 is the only test that covers
# BUG-3, which previously had zero regression coverage.
# Terminology follows Build Systems à la Carte (BSàlC) and Adapton.

source common.sh

testDir="$TEST_ROOT/eval-trace-impure-soundness"
mkdir -p "$testDir"

cp "${config_nix}" "$testDir/config.nix"

###############################################################################
# Test 1: BUG-3 — coerceToString sub-thunk dep survives PublicationWarmupScope
#
# Root cause: mergeIntoParent in PublicationWarmupScope called
# rollbackReplayEpoch, which erased epochMap entries for sub-thunks that were
# forced during the warmup scope.  The fix removed the rollback.
#
# The bug manifests when builtins.toString (which goes through
# coerceToContextObjectForUnsafeDiscard → PublicationWarmupScope) forces a
# sub-thunk that reads from a file.  If the epochMap entry is lost, the
# sub-thunk's dep on the file is silently dropped, and future verification
# falsely passes even when the file changes.
#
# Mechanism exercised here:
#   data     = builtins.fromJSON (builtins.readFile ./data.json)   -- file dep
#   sub      = { x = data.inner_dep; }                             -- sub-thunk
#   toString sub.x                                                  -- triggers
#                                                                      warmup scope
#
# Without the BUG-3 fix, changing data.json would not invalidate the trace;
# "value-v1" would be served stale.  With the fix, the file dep survives
# mergeIntoParent and the cache is correctly invalidated.
###############################################################################
clearStoreIndex

cat >"$testDir/data-bug3.json" <<'EOF'
{"inner_dep": "value-v1"}
EOF

cat >"$testDir/test-bug3.nix" <<'EOF'
let
  data = builtins.fromJSON (builtins.readFile ./data-bug3.json);
  sub  = { x = data.inner_dep; };
in
builtins.toString sub.x
EOF

# Cold evaluation — records trace; sub-thunk's file dep must be captured inside
# PublicationWarmupScope and survive mergeIntoParent (BSàlC: trace recording).
result1_cold="$(nix eval --impure -f "$testDir/test-bug3.nix")"
[[ "$result1_cold" == '"value-v1"' ]] \
  || { echo "BUG-3 cold eval FAILED: got '$result1_cold'"; exit 1; }

# Warm evaluation — trace valid, served from cache (BSàlC: verifying trace succeeds).
result1_warm="$(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-bug3.nix")"
[[ "$result1_warm" == '"value-v1"' ]] \
  || { echo "BUG-3 warm eval FAILED: got '$result1_warm'"; exit 1; }

# Mutate the file — the sub-thunk's dep on inner_dep must dirty the trace.
cat >"$testDir/data-bug3.json" <<'EOF'
{"inner_dep": "value-v2"}
EOF

# Critical assertion: NIX_ALLOW_EVAL=0 must now FAIL because the file changed.
# If BUG-3 were reintroduced (rollbackReplayEpoch erasing the epochMap entry),
# the sub-thunk's dep would be lost, verification would falsely pass, and
# stale "value-v1" would be served.
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-bug3.nix" \
  | grepQuiet "not everything is cached"

# Fresh evaluation after invalidation — must produce the new value.
result1_fresh="$(nix eval --impure -f "$testDir/test-bug3.nix")"
[[ "$result1_fresh" == '"value-v2"' ]] \
  || { echo "BUG-3 fresh re-eval FAILED: got '$result1_fresh'"; exit 1; }

# Warm hit on the newly recorded trace (BSàlC: verifying trace succeeds).
result1_warm2="$(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-bug3.nix")"
[[ "$result1_warm2" == '"value-v2"' ]] \
  || { echo "BUG-3 second warm eval FAILED: got '$result1_warm2'"; exit 1; }

echo "Test 1 passed: BUG-3 — coerceToString sub-thunk dep survives PublicationWarmupScope"

###############################################################################
# Test 2: BUG-5 — untracked file change invalidates trace in impure mode
#
# In impure mode without git tracking, all input files are verified by content
# (FileBytes hash).  A change to any imported file must cause a verify miss.
# This test exercises the simplest end-to-end soundness path: read a plain
# text file, cache the result, modify the file, assert cache miss.
###############################################################################
clearStoreIndex

echo "hello-v1" > "$testDir/bug5-dep.txt"

cat >"$testDir/test-bug5.nix" <<'EOF'
builtins.readFile ./bug5-dep.txt
EOF

# Cold evaluation — records trace with FileBytes dep on bug5-dep.txt.
result2_cold="$(nix eval --impure -f "$testDir/test-bug5.nix")"
[[ "$result2_cold" == '"hello-v1\n"' ]] \
  || { echo "BUG-5 cold eval FAILED: got '$result2_cold'"; exit 1; }

# Warm hit — trace valid (BSàlC: verifying trace succeeds).
result2_warm="$(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-bug5.nix")"
[[ "$result2_warm" == '"hello-v1\n"' ]] \
  || { echo "BUG-5 warm eval FAILED: got '$result2_warm'"; exit 1; }

# Modify the file — FileBytes hash changes.
echo "hello-v2" > "$testDir/bug5-dep.txt"

# Verify miss — content dependency changed (Adapton: demand-driven dirtying).
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-bug5.nix" \
  | grepQuiet "not everything is cached"

# Fresh re-evaluation records new trace.
result2_fresh="$(nix eval --impure -f "$testDir/test-bug5.nix")"
[[ "$result2_fresh" == '"hello-v2\n"' ]] \
  || { echo "BUG-5 fresh re-eval FAILED: got '$result2_fresh'"; exit 1; }

# Warm hit on new trace.
result2_warm2="$(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-bug5.nix")"
[[ "$result2_warm2" == '"hello-v2\n"' ]] \
  || { echo "BUG-5 second warm eval FAILED: got '$result2_warm2'"; exit 1; }

echo "Test 2 passed: BUG-5 — untracked file change invalidates trace in impure mode"

###############################################################################
# Test 3: Precision — independent sibling invalidation
#
# Two attributes with independent file dependencies.  Changing one file must
# invalidate exactly that attribute's cache and leave the other attribute's
# cache intact (Adapton: selective dirtying, precision property).
###############################################################################
clearStoreIndex

echo "left-v1"  > "$testDir/sibling-left.txt"
echo "right-v1" > "$testDir/sibling-right.txt"

cat >"$testDir/test-siblings.nix" <<'EOF'
{
  left  = builtins.readFile ./sibling-left.txt;
  right = builtins.readFile ./sibling-right.txt;
}
EOF

# Cold evaluation — record traces for both attributes.
nix eval --impure -f "$testDir/test-siblings.nix" left  > /dev/null
nix eval --impure -f "$testDir/test-siblings.nix" right > /dev/null

# Both warm hits — traces valid.
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-siblings.nix" left  | grepQuiet "left-v1"
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-siblings.nix" right | grepQuiet "right-v1"

# Modify only the left file — dirties left's trace, right's trace untouched.
echo "left-v2" > "$testDir/sibling-left.txt"

# left: verify miss (its dep changed).
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-siblings.nix" left \
  | grepQuiet "not everything is cached"

# right: still a warm hit (its dep is unchanged — precision).
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-siblings.nix" right | grepQuiet "right-v1"

# Re-record left's trace and verify both attributes are warm.
nix eval --impure -f "$testDir/test-siblings.nix" left | grepQuiet "left-v2"
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-siblings.nix" left  | grepQuiet "left-v2"
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-siblings.nix" right | grepQuiet "right-v1"

echo "Test 3 passed: Precision — independent sibling invalidation"

###############################################################################
# Test 4: Precision — comment-only change in imported .nix file is a cache miss
#
# FileBytes tracks raw file content, so even a comment-only change in an
# imported .nix file produces a different hash and triggers a verify miss.
# This is expected and documented behaviour: the system is conservative about
# .nix source changes (no AST-level deduplication).
# After re-recording the trace with the new comment, the cache is warm again.
###############################################################################
clearStoreIndex

cat >"$testDir/imported-with-comment.nix" <<'EOF'
# Version: initial
{ value = "from-import"; }
EOF

cat >"$testDir/test-comment.nix" <<'EOF'
let mod = import ./imported-with-comment.nix; in mod.value
EOF

# Cold evaluation — records trace including FileBytes dep on imported-with-comment.nix.
result4_cold="$(nix eval --impure -f "$testDir/test-comment.nix")"
[[ "$result4_cold" == '"from-import"' ]] \
  || { echo "Comment test cold eval FAILED: got '$result4_cold'"; exit 1; }

# Warm hit — trace valid.
result4_warm="$(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-comment.nix")"
[[ "$result4_warm" == '"from-import"' ]] \
  || { echo "Comment test warm eval FAILED: got '$result4_warm'"; exit 1; }

# Change only a comment — raw bytes differ so FileBytes hash changes.
# BUT: `import ./file.nix` + `.value` access produces NixBinding
# StructuredProjection deps. The StructuredProjection dep on `.value`
# still passes (binding unchanged), so ValidViaStructuralOverride fires.
# This is the CORRECT precision behavior: comment-only changes in .nix
# files that don't affect accessed bindings ARE cache hits.
cat >"$testDir/imported-with-comment.nix" <<'EOF'
# Version: after-comment-edit
{ value = "from-import"; }
EOF

# Must be a cache HIT via structural override (precision property).
# Pin the path: warm verify must record 0 new traces AND see at
# least one contentSubsumptionSkip (the FileBytes dep on the
# comment-edited file fails; SC override on the .value binding
# subsumes it). Without this, a silent re-record would also produce
# the right value.
result4_override="$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/impure-t4-override.json" \
    env NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-comment.nix")"
[[ "$result4_override" == '"from-import"' ]] \
  || { echo "Comment test structural override FAILED: got '$result4_override'"; exit 1; }
t4_override_records="$(readEvalTraceCounter "$TEST_HOME/impure-t4-override.json" evalTrace.record.count)"
if [[ "$t4_override_records" -ne 0 ]]; then
    echo "Test 4 regression: comment-only change caused re-record (records=$t4_override_records)"
    echo "Structural override is not subsuming the FileBytes change."
    cat "$TEST_HOME/impure-t4-override.json" >&2
    exit 1
fi

# Now change the ACCESSED binding — this must be a cache MISS.
cat >"$testDir/imported-with-comment.nix" <<'EOF'
# Version: after-comment-edit
{ value = "changed-value"; }
EOF

# The StructuredProjection dep on `.value` fails → cache miss.
# Pin via NIX_SHOW_STATS that the miss actually re-records, rather
# than silently serving a cached wrong value and then being masked
# by the string comparison.
result4_changed="$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/impure-t4-miss.json" \
    nix eval --impure -f "$testDir/test-comment.nix")"
[[ "$result4_changed" == '"changed-value"' ]] \
  || { echo "Comment test value-change FAILED: got '$result4_changed'"; exit 1; }
t4_miss_records="$(readEvalTraceCounter "$TEST_HOME/impure-t4-miss.json" evalTrace.record.count)"
if [[ "$t4_miss_records" -lt 1 ]]; then
    echo "Test 4 regression: value-change did not drive a fresh record (records=$t4_miss_records)"
    cat "$TEST_HOME/impure-t4-miss.json" >&2
    exit 1
fi

echo "Test 4 passed: Precision — comment-only .nix change is subsumed; value change re-records"

###############################################################################
# Test 5: BUG-8 documentation — failed eval does not corrupt subsequent
#         verification of a previously-cached expression
#
# A failing evaluation (builtins.throw) must not corrupt the trace store.
# After the failure, a previously-cached expression must still serve correctly
# from cache without any stale or missing trace entries.
###############################################################################
clearStoreIndex

cat >"$testDir/test-good.nix" <<'EOF'
{ stable = "stable-value"; }
EOF

cat >"$testDir/test-throw.nix" <<'EOF'
builtins.throw "intentional-failure-for-bug8-test"
EOF

# Cold evaluation of the good expression — records trace.
result5_cold="$(nix eval --impure -f "$testDir/test-good.nix" stable)"
[[ "$result5_cold" == '"stable-value"' ]] \
  || { echo "BUG-8 cold eval FAILED: got '$result5_cold'"; exit 1; }

# Warm hit — trace valid before the failing eval.
result5_warm="$(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-good.nix" stable)"
[[ "$result5_warm" == '"stable-value"' ]] \
  || { echo "BUG-8 pre-throw warm eval FAILED: got '$result5_warm'"; exit 1; }

# Trigger a failing evaluation — must not corrupt the trace store.
# expectStderr captures $@ with stderr→stdout; piping to /dev/null discards the
# error message while still asserting the exit code is 1.
expectStderr 1 nix eval --impure -f "$testDir/test-throw.nix" > /dev/null

# Good expression must still serve from cache after the failed eval.
# BUG-8: if the failing eval left behind a partial or corrupted trace entry,
# this warm eval could fail or return wrong data.
# Pin: primary-session hit (no recovery, no re-record) proves the
# existing trace row survived the failing eval intact. Without the
# counters, a regression that re-recorded the good expr from scratch
# would still return "stable-value" and pass the string check.
result5_post="$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/impure-t5-post.json" \
    env NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-good.nix" stable)"
[[ "$result5_post" == '"stable-value"' ]] \
  || { echo "BUG-8 post-throw warm eval FAILED: got '$result5_post'"; exit 1; }
t5_post_records="$(readEvalTraceCounter "$TEST_HOME/impure-t5-post.json" evalTrace.record.count)"
t5_post_attempts="$(readEvalTraceCounter "$TEST_HOME/impure-t5-post.json" evalTrace.recovery.attempts)"
t5_post_hits="$(readEvalTraceCounter "$TEST_HOME/impure-t5-post.json" evalTrace.hits)"
if [[ "$t5_post_records" -ne 0 || "$t5_post_attempts" -ne 0 || "$t5_post_hits" -lt 1 ]]; then
    echo "Test 5 regression: post-throw warm did not hit primary cache"
    echo "  records=$t5_post_records attempts=$t5_post_attempts hits=$t5_post_hits"
    cat "$TEST_HOME/impure-t5-post.json" >&2
    exit 1
fi

echo "Test 5 passed: BUG-8 — failed eval does not corrupt subsequent cache verification"

###############################################################################
# Test 6: OR-2 — bare `import` of an attrset with no binding access records
#         a FileBytes backstop dep (POSITIVE SOUNDNESS TEST after OR-2 fix)
#
# Root cause and fix (src/libexpr/eval-trace/CLAUDE.md §OR-2).  Before the
# fix, ExprParseFile::eval and scopedImport only emitted a FileBytes dep
# on the non-NixBinding-eligible branch.  NixBinding-eligible files
# (direct `{ ... }` attrset body after walking lambda/let/with/update/call
# wrappers) ran `registerNixBindings` but skipped the FileBytes backstop,
# relying on access-time `maybeRecordNixBindingDep` — which only fires for
# Nix-level ExprSelect.  `attrNames` / `deepSeq` / `seq` / formals /
# findAlongAttrPath consumers never triggered that path, so the trace had
# zero deps on the imported file and stale-served on any edit.
#
# The fix moves the `maybeRecordImportedFileContent` call out of the
# `else` branch so it runs unconditionally under `state.traceActiveDepth`.
# The FileBytes backstop now fires for every traced import; access-time
# emission dedups against it; pass-2 override lets per-binding SP deps
# subsume it for accessed bindings.
#
# This test exercises the shape the bug originally exposed:
#   lib.nix v1 — { greeting = "hello-v1"; }
#   top.nix    — import ./lib.nix, then deepSeq + length (attrNames x)
#
#   Cold nix eval                            → returns "1"
#   Edit lib.nix to 3 attrs
#   Warm NIX_ALLOW_EVAL=0 nix eval           → must FAIL (not everything cached)
#   Warm nix eval                            → must return "3"
#
# Unit-level regression coverage lives at
# `src/libexpr-tests/eval-trace/dep/oracle-nested.cc::BareImport_*`.
###############################################################################
clearStoreIndex

mkdir -p "$testDir/or2"
cat >"$testDir/or2/lib.nix" <<'EOF'
{ greeting = "hello-v1"; }
EOF

cat >"$testDir/or2/top.nix" <<'EOF'
let x = import ./lib.nix;
in builtins.deepSeq x (builtins.toString (builtins.length (builtins.attrNames x)))
EOF

# Cold evaluation — records trace.  The OR-2 fix emits a FileBytes
# backstop on lib.nix at ExprParseFile::eval, in addition to the
# registerNixBindings registration.
result6_cold="$(nix eval --impure -f "$testDir/or2/top.nix")"
[[ "$result6_cold" == '"1"' ]] \
  || { echo "OR-2 cold eval FAILED: got '$result6_cold'"; exit 1; }

# Warm hit before mutation — precision pre-condition.
result6_warm_precheck="$(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/or2/top.nix")"
[[ "$result6_warm_precheck" == '"1"' ]] \
  || { echo "OR-2 warm precondition FAILED: got '$result6_warm_precheck'"; exit 1; }

# Mutate lib.nix to a 3-attr attrset.  The FileBytes dep on lib.nix
# must fail verification — there is no per-binding StructuredProjection
# to cover it (no ExprSelect fired), so determineOutcome returns Invalid.
cat >"$testDir/or2/lib.nix" <<'EOF'
{ greeting = "hello-v2"; other = 42; moreAttr = "x"; }
EOF

# NIX_ALLOW_EVAL=0 must REJECT the warm verify — the trace is no longer
# valid.  Before the fix this silently served "1".
expectStderr 1 env NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/or2/top.nix" 2>&1 \
  | grepQuiet "not everything is cached"

# Unrestricted eval must re-record and produce the correct post-mutation
# value "3".  Before the fix this also served "1" silently.
result6_warm="$(nix eval --impure -f "$testDir/or2/top.nix")"
[[ "$result6_warm" == '"3"' ]] \
  || { echo "OR-2 warm-after-mutation FAILED: got '$result6_warm' (expected '\"3\"'); the FileBytes backstop is not invalidating the trace on content change"; exit 1; }

# Reference cross-check with eval-trace disabled — must also produce "3".
result6_reference="$(nix eval --no-eval-trace --impure -f "$testDir/or2/top.nix")"
[[ "$result6_reference" == '"3"' ]] \
  || { echo "OR-2 reference eval FAILED: got '$result6_reference'"; exit 1; }

echo "Test 6 passed: OR-2 — bare-import FileBytes backstop correctly invalidates"

###############################################################################
# Test 7: OR-2 precision companion — comment-only edit via bare import
#         re-records (coarse-FileBytes conservative) but preserves value.
#
# The OR-2 fix records only the coarse FileBytes backstop; it deliberately
# does NOT add a finer-grained `ImplicitStructure(Nix, Keys)` dep (see
# §OR-2 design-space discussion).  Consequently, comment-only edits to a
# bare-imported file with no binding access cause re-evaluation.  That is
# acceptable: soundness takes priority over precision in the bare-import
# path; the precision bonus for comment-only edits is still available on
# the ExprSelect path via the existing NixBindingSCOverride mechanism.
#
# This test pins the coarse-but-sound behaviour explicitly:
#   (a) a comment-only edit must REJECT the warm verify under
#       NIX_ALLOW_EVAL=0 (the FileBytes backstop fails), and
#   (b) the subsequent unrestricted re-eval must produce the same
#       observed value (precision of the consumer, since the key set
#       and observable output are unchanged).
#
# The two halves together prove "FileBytes catches the edit even
# though the observable value is unchanged" — i.e., the fix is
# conservative but correct.  A future precision improvement (a
# per-file ImplicitStructure(Nix, Keys) dep) would let this warm
# verify hit cache; flipping (a) to expect a cache hit would be the
# signal that such an extension landed.
###############################################################################
clearStoreIndex

mkdir -p "$testDir/or2-precision"
cat >"$testDir/or2-precision/lib.nix" <<'EOF'
{ greeting = "hello-v1"; }
EOF

cat >"$testDir/or2-precision/top.nix" <<'EOF'
let x = import ./lib.nix;
in builtins.deepSeq x (builtins.toString (builtins.length (builtins.attrNames x)))
EOF

result7_cold="$(nix eval --impure -f "$testDir/or2-precision/top.nix")"
[[ "$result7_cold" == '"1"' ]] \
  || { echo "OR-2 precision cold FAILED: got '$result7_cold'"; exit 1; }

# Comment-only edit: key set unchanged, observable value unchanged,
# but raw bytes differ so FileBytes hash changes.
cat >"$testDir/or2-precision/lib.nix" <<'EOF'
# just a comment change
{ greeting = "hello-v1"; }
EOF

# (a) Warm verify under NIX_ALLOW_EVAL=0 must REJECT — FileBytes
#     backstop catches the content change, no SP dep subsumes it.
expectStderr 1 env NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/or2-precision/top.nix" 2>&1 \
  | grepQuiet "not everything is cached"

# (b) Unrestricted eval re-records and produces the same observable.
result7_warm="$(nix eval --impure -f "$testDir/or2-precision/top.nix")"
[[ "$result7_warm" == '"1"' ]] \
  || { echo "OR-2 precision warm FAILED: got '$result7_warm' (expected '\"1\"'); comment-only edit should not change the observed value"; exit 1; }

echo "Test 7 passed: OR-2 — comment-only edit invalidates conservatively but preserves observable value"

###############################################################################
# Test 8: OR-2 precision pin — unrelated-file edit MUST hit cache.
#
# The FileBytes backstop introduced by the OR-2 fix is scoped to the
# imported file.  Editing a sibling file that the import does not depend
# on must still warm-hit under NIX_ALLOW_EVAL=0 — otherwise the fix
# over-invalidated.  This is the negative-of-a-positive that guards
# against a future "emit FileBytes on every file in the working tree"
# regression.
###############################################################################
clearStoreIndex

mkdir -p "$testDir/or2-scope"
cat >"$testDir/or2-scope/lib.nix" <<'EOF'
{ greeting = "hello-v1"; }
EOF
cat >"$testDir/or2-scope/unrelated.nix" <<'EOF'
{ noise = "v1"; }
EOF

cat >"$testDir/or2-scope/top.nix" <<'EOF'
let x = import ./lib.nix;
in builtins.deepSeq x (builtins.toString (builtins.length (builtins.attrNames x)))
EOF

result8_cold="$(nix eval --impure -f "$testDir/or2-scope/top.nix")"
[[ "$result8_cold" == '"1"' ]] \
  || { echo "OR-2 scope cold FAILED: got '$result8_cold'"; exit 1; }

# Edit unrelated.nix; top.nix does not import it.
cat >"$testDir/or2-scope/unrelated.nix" <<'EOF'
{ noise = "v2-different-bytes"; other = 42; }
EOF

result8_warm="$(NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/or2-scope/top.nix")"
[[ "$result8_warm" == '"1"' ]] \
  || { echo "OR-2 scope FAILED: got '$result8_warm' (expected '\"1\"'); FileBytes backstop over-invalidated on an unrelated-file edit"; exit 1; }

echo "Test 8 passed: OR-2 — FileBytes backstop is per-file; unrelated edits hit cache"

echo "All eval-trace-impure-soundness tests passed!"
