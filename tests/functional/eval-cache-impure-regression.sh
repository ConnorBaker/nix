#!/usr/bin/env bash

# Regression tests for specific eval cache bugs.
# Each test targets a single bug's mechanism to prevent reintroduction.

source common.sh

clearStoreIndex() {
    rm -f "$TEST_HOME/.cache/nix/eval-index-v1.sqlite"
}

testDir="$TEST_ROOT/eval-cache-regression"
mkdir -p "$testDir"

cp "${config_nix}" "$testDir/config.nix"

###############################################################################
# Test 1: readFile dep inside derivation buildCommand tracked at drv level
#
# Bug: derivation.nix uses `let strict = derivationStrict drvAttrs` lazily.
# Forcing a derivation attrset to WHNF does NOT call derivationStrict, so
# deps from env var processing (e.g., readFile via buildCommand string
# interpolation) were not captured at the derivation attribute's level.
#
# Fix: After forceValue in evaluateCold, detect derivation attrsets and
# force drvPath to trigger derivationStrict.
###############################################################################
clearStoreIndex

echo "drv-dep-v1" > "$testDir/drv-dep.txt"

# readFile is directly in buildCommand — NOT in a let binding at the top level.
# This means it's evaluated inside derivationStrict's env processing, which is
# lazy. Without the drvPath forcing fix, the dep would be at drv.drvAttrs.buildCommand
# level, not at the drv level, and `nix build ... drv` would not see it invalidate.
cat >"$testDir/test-drv-lazy-dep.nix" <<'EOF'
let inherit (import ./config.nix) mkDerivation; in
{
  drv = mkDerivation {
    name = "drv-lazy-dep";
    buildCommand = "echo '${builtins.readFile ./drv-dep.txt}' > $out";
  };
}
EOF

# Cold build
nix build --no-link --impure -f "$testDir/test-drv-lazy-dep.nix" drv

# Warm cache
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-drv-lazy-dep.nix" drv

# Modify the file that readFile depends on
echo "drv-dep-v2" > "$testDir/drv-dep.txt"

# Without the fix, this would succeed (stale cache hit) because the readFile
# dep was captured at drv.drvAttrs.buildCommand, not at drv itself.
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-drv-lazy-dep.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 1 passed: readFile in buildCommand tracked at drv level"

###############################################################################
# Test 2: storeForcedSibling does not overwrite first-eval sibling's deps
#
# Bug: When evaluating {a, b, c} with --json, a is evaluated first with
# correct deps. When b's navigateToReal runs, a is already forced (not a
# thunk), so storeForcedSibling is called for a with empty deps {},
# overwriting a's index entry. Result: a's deps were lost, and it would
# never invalidate.
#
# Fix: storeForcedSibling checks if the attribute already exists in the
# index before calling coldStore — skips if already stored.
###############################################################################
clearStoreIndex

echo "alpha-v1" > "$testDir/alpha.txt"
echo "beta-v1" > "$testDir/beta.txt"
echo "gamma-v1" > "$testDir/gamma.txt"

# Three attributes, each with an independent dep file.
# When --json evaluates all three:
#   1. a is evaluated: a's deps recorded, b and c wrapped by navigateToReal
#   2. b is evaluated: navigateToReal sees a is forced → storeForcedSibling(a, {})
#      Without fix: overwrites a's index entry with dep-less eval-a.drv
#   3. c is evaluated: navigateToReal sees a,b are forced → storeForcedSibling for both
cat >"$testDir/test-sibling-overwrite.nix" <<EOF
{
  a = "alpha-\${builtins.readFile $testDir/alpha.txt}";
  b = "beta-\${builtins.readFile $testDir/beta.txt}";
  c = "gamma-\${builtins.readFile $testDir/gamma.txt}";
}
EOF

# Evaluate all three in one session
nix eval --impure -f "$testDir/test-sibling-overwrite.nix" --json

# Each should be independently cached
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" a
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" b
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" c

# Modify only alpha.txt
echo "alpha-v2" > "$testDir/alpha.txt"

# a should invalidate (Bug 2: storeForcedSibling would have erased a's deps)
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" a \
  | grepQuiet "not everything is cached"

# b and c should still be cached (their deps are unchanged)
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" b
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" c

# Restore alpha, modify beta only
echo "alpha-v2" > "$testDir/alpha.txt"  # keep modified
echo "beta-v2" > "$testDir/beta.txt"

# b should invalidate
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" b \
  | grepQuiet "not everything is cached"

# c should still be cached
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-sibling-overwrite.nix" c

echo "Test 2 passed: storeForcedSibling does not overwrite deps"

###############################################################################
# Test 3: ExprCached wrapper from navigateToReal does not steal deps
#
# Bug: When a's navigateToReal wraps b with ExprCached in the real tree,
# b's cold eval encounters the wrapper thunk. forceValue on the wrapper
# triggers ExprCached::eval() which creates a nested FileLoadTracker,
# capturing b's deps in the inner tracker instead of the outer one.
# Result: b's eval drv has 0 inputSrcs, so b never invalidates.
#
# Fix: In evaluateCold, after navigateToReal, detect ExprCached-wrapped
# targets and unwrap them by evaluating the original expression directly
# in the current tracker's scope.
###############################################################################
clearStoreIndex

echo "shared-v1" > "$testDir/shared-dep.txt"

# Two attributes reading the same file. After a is evaluated, b's real-tree
# value is wrapped with ExprCached by a's navigateToReal. When b is then
# evaluated, the unwrap code must ensure b's readFile dep is captured in
# b's own FileLoadTracker, not in the wrapper's nested tracker.
cat >"$testDir/test-wrapper-steal.nix" <<EOF
{
  a = "first-\${builtins.readFile $testDir/shared-dep.txt}";
  b = "second-\${builtins.readFile $testDir/shared-dep.txt}";
}
EOF

# Evaluate both in one session (a first, then b — alphabetical order)
nix eval --impure -f "$testDir/test-wrapper-steal.nix" --json

# Both should be cached
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-wrapper-steal.nix" a
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-wrapper-steal.nix" b

# Modify the shared dep
echo "shared-v2" > "$testDir/shared-dep.txt"

# a should invalidate (first-evaluated, its deps are correct)
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-wrapper-steal.nix" a \
  | grepQuiet "not everything is cached"

# b should ALSO invalidate (Bug 3: without unwrap, b's eval drv would
# have 0 inputSrcs because readFile dep was stolen by the nested tracker)
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-wrapper-steal.nix" b \
  | grepQuiet "not everything is cached"

echo "Test 3 passed: ExprCached wrapper does not steal deps"

###############################################################################
# Test 4: Derivation with multiple env-level readFile deps
#
# Variant of Bug 1 with multiple readFile calls in different env attributes.
# Ensures ALL deps from derivationStrict processing are captured, not just
# the first one.
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

# Cold build
nix build --no-link --impure -f "$testDir/test-multi-env-dep.nix" drv

# Warm cache
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-multi-env-dep.nix" drv

# Modify only the name dep
echo -n "name-v2" > "$testDir/name-dep.txt"

# Should invalidate (name attribute also processed by derivationStrict)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-multi-env-dep.nix" drv \
  | grepQuiet "not everything is cached"

# Re-populate cache after invalidation
nix build --no-link --impure -f "$testDir/test-multi-env-dep.nix" drv

# Restore name, modify cmd dep
echo -n "name-v2" > "$testDir/name-dep.txt"  # keep modified
echo "cmd-v2" > "$testDir/cmd-dep.txt"

# Should also invalidate (buildCommand attribute processed by derivationStrict)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-multi-env-dep.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 4 passed: multiple env-level readFile deps tracked"

###############################################################################
# Test 5: Many siblings — storeForcedSibling preserves all deps
#
# Stress variant of Bug 2 with 5 attributes. The last-evaluated attribute
# triggers storeForcedSibling for the 4 preceding ones. All must retain
# their original deps.
###############################################################################
clearStoreIndex

for i in 1 2 3 4 5; do
  echo "dep-$i-v1" > "$testDir/dep-$i.txt"
done

# Five attributes, each with an independent dep file.
cat >"$testDir/test-many-siblings.nix" <<EOF
{
  a1 = "v1-\${builtins.readFile $testDir/dep-1.txt}";
  a2 = "v2-\${builtins.readFile $testDir/dep-2.txt}";
  a3 = "v3-\${builtins.readFile $testDir/dep-3.txt}";
  a4 = "v4-\${builtins.readFile $testDir/dep-4.txt}";
  a5 = "v5-\${builtins.readFile $testDir/dep-5.txt}";
}
EOF

# Evaluate all in one session
nix eval --impure -f "$testDir/test-many-siblings.nix" --json

# All cached
for i in 1 2 3 4 5; do
  NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-many-siblings.nix" "a$i"
done

# Modify dep-1 only — a1 should invalidate, others stay cached
echo "dep-1-v2" > "$testDir/dep-1.txt"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-many-siblings.nix" a1 \
  | grepQuiet "not everything is cached"
for i in 2 3 4 5; do
  NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-many-siblings.nix" "a$i"
done

# Modify dep-5 only — a5 should invalidate, a2-a4 stay cached
echo "dep-5-v2" > "$testDir/dep-5.txt"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-many-siblings.nix" a5 \
  | grepQuiet "not everything is cached"
for i in 2 3 4; do
  NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-many-siblings.nix" "a$i"
done

echo "Test 5 passed: many siblings all retain their deps"

echo "All eval-cache-impure-regression tests passed!"
