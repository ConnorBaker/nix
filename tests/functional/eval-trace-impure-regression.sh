#!/usr/bin/env bash

# Regression tests for specific eval trace bugs.
# Each test targets a single bug's mechanism to prevent reintroduction.
# Terminology follows Build Systems à la Carte (BSàlC) and Adapton.

source common.sh

clearStoreIndex() {
    rm -rf "$TEST_HOME/.cache/nix/eval-trace"* "$TEST_HOME/.cache/nix/attr-vocab.sqlite"* "$TEST_HOME/.cache/nix/stat-hash-cache"*
}

testDir="$TEST_ROOT/eval-trace-regression"
mkdir -p "$testDir"

cp "${config_nix}" "$testDir/config.nix"

###############################################################################
# Test 1: readFile dependency inside derivation buildCommand tracked at drv level
#
# Bug: derivation.nix uses `let strict = derivationStrict drvAttrs` lazily.
# Forcing a derivation attrset to WHNF does NOT call derivationStrict, so
# dependencies from env var processing (e.g., readFile via buildCommand string
# interpolation) were not recorded in the derivation attribute's trace.
#
# Fix: After forceValue in evaluateFresh, detect derivation attrsets and
# force drvPath to trigger derivationStrict, ensuring all dependencies are
# captured in the trace (BSàlC: complete trace recording).
###############################################################################
clearStoreIndex

echo "drv-dep-v1" > "$testDir/drv-dep.txt"

# readFile is directly in buildCommand -- NOT in a let binding at the top level.
# This means it's evaluated inside derivationStrict's env processing, which is
# lazy. Without the drvPath forcing fix, the dependency would be recorded in
# the wrong trace (drv.drvAttrs.buildCommand, not drv), and `nix build ... drv`
# would get a false verification hit.
cat >"$testDir/test-drv-lazy-dep.nix" <<'EOF'
let inherit (import ./config.nix) mkDerivation; in
{
  drv = mkDerivation {
    name = "drv-lazy-dep";
    buildCommand = "echo '${builtins.readFile ./drv-dep.txt}' > $out";
  };
}
EOF

# Fresh evaluation — records trace with buildCommand readFile dependency
nix build --no-link --impure -f "$testDir/test-drv-lazy-dep.nix" drv

# Verification hit — traced result valid
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-drv-lazy-dep.nix" drv

# Modify the file that readFile depends on — dirties the trace dependency
echo "drv-dep-v2" > "$testDir/drv-dep.txt"

# Without the fix, this would be a false verification hit (stale traced result)
# because the readFile dependency was recorded in the wrong trace.
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-drv-lazy-dep.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 1 passed: readFile in buildCommand dependency tracked at drv trace level"

###############################################################################
# Test 2: recordSiblingTrace does not overwrite first-eval sibling's trace
#
# Bug: When evaluating {a, b, c} with --json, a is evaluated first with
# correct trace dependencies. When b's navigateToReal runs, a is already
# forced (not a thunk), so recordSiblingTrace is called for a with empty
# deps {}, overwriting a's trace. Result: a's trace had no dependencies,
# and verification would always succeed (false verification hit).
#
# Fix: recordSiblingTrace checks if the attribute already has a recorded
# trace before calling record -- skips if already stored.
###############################################################################
clearStoreIndex

echo "alpha-v1" > "$testDir/alpha.txt"
echo "beta-v1" > "$testDir/beta.txt"
echo "gamma-v1" > "$testDir/gamma.txt"

# Three attributes, each with an independent dependency file.
# When --json evaluates all three:
#   1. a is evaluated: a's trace recorded, b and c wrapped by navigateToReal
#   2. b is evaluated: navigateToReal sees a is forced -> recordSiblingTrace(a, {})
#      Without fix: overwrites a's trace with dep-less one (false verification hits)
#   3. c is evaluated: navigateToReal sees a,b are forced -> recordSiblingTrace for both
cat >"$testDir/test-sibling-overwrite.nix" <<EOF
{
  a = "alpha-\${builtins.readFile $testDir/alpha.txt}";
  b = "beta-\${builtins.readFile $testDir/beta.txt}";
  c = "gamma-\${builtins.readFile $testDir/gamma.txt}";
}
EOF

# Record traces for all three in one session
nix eval --impure -f "$testDir/test-sibling-overwrite.nix" --json

# Each independently passes trace verification
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" a
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" b
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" c

# Modify only alpha.txt — dirties a's trace
echo "alpha-v2" > "$testDir/alpha.txt"

# a: verify miss (Bug 2: recordSiblingTrace would have erased a's trace dependencies)
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" a \
  | grepQuiet "not everything is cached"

# b and c: verification hits — their trace dependencies are unchanged
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" b
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" c

# Keep alpha modified, modify beta only
echo "alpha-v2" > "$testDir/alpha.txt"  # keep modified
echo "beta-v2" > "$testDir/beta.txt"

# b: verify miss — trace verification fails (beta.txt changed)
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" b \
  | grepQuiet "not everything is cached"

# c: verification hit — its trace dependencies unchanged
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" c

echo "Test 2 passed: recordSiblingTrace does not overwrite sibling traces"

###############################################################################
# Test 3: TracedExpr wrapper from navigateToReal does not steal trace dependencies
#
# Bug: When a's navigateToReal wraps b with TracedExpr in the real tree,
# b's fresh evaluation encounters the wrapper thunk. forceValue on the wrapper
# triggers TracedExpr::eval() which creates a nested DependencyTracker,
# capturing b's dependencies in the inner tracker instead of the outer one.
# Result: b's trace has 0 dependencies, so verification always succeeds
# (false verification hit — stale traced result served).
#
# Fix: In evaluateFresh, after navigateToReal, detect TracedExpr-wrapped
# targets and unwrap them by evaluating the original expression directly
# in the current tracker's scope (correct trace recording).
###############################################################################
clearStoreIndex

echo "shared-v1" > "$testDir/shared-dep.txt"

# Two attributes reading the same file. After a is evaluated, b's real-tree
# value is wrapped with TracedExpr by a's navigateToReal. When b is then
# evaluated, the unwrap code must ensure b's readFile dependency is recorded
# in b's own trace, not in the wrapper's nested tracker.
cat >"$testDir/test-wrapper-steal.nix" <<EOF
{
  a = "first-\${builtins.readFile $testDir/shared-dep.txt}";
  b = "second-\${builtins.readFile $testDir/shared-dep.txt}";
}
EOF

# Record traces for both in one session (a first, then b -- alphabetical order)
nix eval --impure -f "$testDir/test-wrapper-steal.nix" --json

# Both verification hits — traces valid
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-wrapper-steal.nix" a
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-wrapper-steal.nix" b

# Modify the shared dependency — dirties both traces
echo "shared-v2" > "$testDir/shared-dep.txt"

# a: verify miss (first-evaluated, its trace dependencies are correct)
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-wrapper-steal.nix" a \
  | grepQuiet "not everything is cached"

# b: also verify miss (Bug 3: without unwrap, b's trace would have 0 dependencies
# because readFile dependency was stolen by the nested tracker — false verification hit)
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-wrapper-steal.nix" b \
  | grepQuiet "not everything is cached"

echo "Test 3 passed: TracedExpr wrapper does not steal trace dependencies"

###############################################################################
# Test 4: Derivation with multiple env-level readFile trace dependencies
#
# Variant of Bug 1 with multiple readFile calls in different env attributes.
# Ensures ALL dependencies from derivationStrict processing are recorded
# in the trace, not just the first one (BSàlC: complete trace recording).
###############################################################################
clearStoreIndex

echo -n "name-v1" > "$testDir/name-dep.txt"
echo "cmd-v1" > "$testDir/cmd-dep.txt"

cat >"$testDir/test-multi-env-dep.nix" <<'EOF'
let inherit (import ./config.nix) mkDerivation; in
{
  drv = mkDerivation {
    name = "multi-env-dep-${builtins.readFile ./name-dep.txt}";
    buildCommand = "echo '${builtins.readFile ./cmd-dep.txt}' > $out";
  };
}
EOF

# Fresh evaluation — records trace with both name and cmd dependencies
nix build --no-link --impure -f "$testDir/test-multi-env-dep.nix" drv

# Verification hit — trace valid
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-multi-env-dep.nix" drv

# Modify only the name dependency — dirties that dependency in the trace
echo -n "name-v2" > "$testDir/name-dep.txt"

# Verify miss — trace verification fails (name dependency changed)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-multi-env-dep.nix" drv \
  | grepQuiet "not everything is cached"

# Re-record trace after verify miss
nix build --no-link --impure -f "$testDir/test-multi-env-dep.nix" drv

# Keep name modified, modify cmd dependency
echo -n "name-v2" > "$testDir/name-dep.txt"  # keep modified
echo "cmd-v2" > "$testDir/cmd-dep.txt"

# Also verify miss — trace verification fails (cmd dependency also changed)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-multi-env-dep.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 4 passed: multiple env-level readFile dependencies in trace"

###############################################################################
# Test 5: Many siblings — recordSiblingTrace preserves all trace dependencies
#
# Stress variant of Bug 2 with 5 attributes. The last-evaluated attribute
# triggers recordSiblingTrace for the 4 preceding ones. All must retain
# their original traces with correct dependencies (BSàlC: trace integrity).
###############################################################################
clearStoreIndex

for i in 1 2 3 4 5; do
  echo "dep-$i-v1" > "$testDir/dep-$i.txt"
done

# Five attributes, each with an independent dependency file recorded in its trace.
cat >"$testDir/test-many-siblings.nix" <<EOF
{
  a1 = "v1-\${builtins.readFile $testDir/dep-1.txt}";
  a2 = "v2-\${builtins.readFile $testDir/dep-2.txt}";
  a3 = "v3-\${builtins.readFile $testDir/dep-3.txt}";
  a4 = "v4-\${builtins.readFile $testDir/dep-4.txt}";
  a5 = "v5-\${builtins.readFile $testDir/dep-5.txt}";
}
EOF

# Record traces for all in one session
nix eval --impure -f "$testDir/test-many-siblings.nix" --json

# All verification hits — traced results valid
for i in 1 2 3 4 5; do
  NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-many-siblings.nix" "a$i"
done

# Modify dep-1 only — a1's trace verification fails, others pass
echo "dep-1-v2" > "$testDir/dep-1.txt"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-many-siblings.nix" a1 \
  | grepQuiet "not everything is cached"
for i in 2 3 4 5; do
  NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-many-siblings.nix" "a$i"
done

# Modify dep-5 only — a5's trace verification fails, a2-a4 pass
echo "dep-5-v2" > "$testDir/dep-5.txt"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-many-siblings.nix" a5 \
  | grepQuiet "not everything is cached"
for i in 2 3 4; do
  NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-many-siblings.nix" "a$i"
done

echo "Test 5 passed: many siblings all retain their trace dependencies"

echo "All eval-trace-impure-regression tests passed! (BSàlC: trace integrity under sibling recording)"
