#!/usr/bin/env bash

# Impure eval cache output tests: nix eval cursor, nix eval invalidation,
# --write-to --file, --json drv/list/outPath, modify-then-revert recovery,
# three-way cycling recovery.

source common.sh

clearStoreIndex() {
    rm -f "$TEST_HOME/.cache/nix/eval-index-v1.sqlite"
}

testDir="$TEST_ROOT/eval-cache-impure"
mkdir -p "$testDir"

cp "${config_nix}" "$testDir/config.nix"

###############################################################################
# Test 1: nix eval --file correctly evaluates via cursor path
###############################################################################
clearStoreIndex

cat >"$testDir/test-eval-cursor.nix" <<'EOF'
{
  myString = "hello from eval";
  myBool = false;
  myInt = 99;
  myPath = ./test-eval-cursor.nix;
}
EOF

# nix eval returns correct values via cursor-based evaluation
nix eval --impure -f "$testDir/test-eval-cursor.nix" myString | grepQuiet "hello from eval"
[[ "$(nix eval --json --impure -f "$testDir/test-eval-cursor.nix" myBool)" == 'false' ]]
[[ "$(nix eval --json --impure -f "$testDir/test-eval-cursor.nix" myInt)" == '99' ]]
nix eval --raw --impure -f "$testDir/test-eval-cursor.nix" myPath | grepQuiet "test-eval-cursor.nix"

# nix eval should serve cached values with NIX_ALLOW_EVAL=0
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-eval-cursor.nix" myString | grepQuiet "hello from eval"
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-eval-cursor.nix" myBool)" == 'false' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-eval-cursor.nix" myInt)" == '99' ]]

echo "Test 1 passed: nix eval --file cursor evaluation + cache read"

###############################################################################
# Test 2: nix eval --file with file modification invalidation
###############################################################################
clearStoreIndex

echo "dep-data-v1" > "$testDir/dep-data.txt"

cat >"$testDir/test-eval-dep.nix" <<'EOF'
{
  depVal = builtins.readFile ./dep-data.txt;
  constVal = "constant";
}
EOF

# Populate cache
nix eval --impure -f "$testDir/test-eval-dep.nix" depVal | grepQuiet "dep-data-v1"
nix eval --impure -f "$testDir/test-eval-dep.nix" constVal | grepQuiet "constant"

# Cache read
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-eval-dep.nix" depVal | grepQuiet "dep-data-v1"
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-eval-dep.nix" constVal | grepQuiet "constant"

# Modify dep-data.txt
echo "dep-data-v2" > "$testDir/dep-data.txt"

# depVal should be invalidated
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure -f "$testDir/test-eval-dep.nix" depVal \
  | grepQuiet "not everything is cached"

# constVal does not depend on dep-data.txt, so it should still be cached
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-eval-dep.nix" constVal | grepQuiet "constant"

# Re-evaluate depVal
nix eval --impure -f "$testDir/test-eval-dep.nix" depVal | grepQuiet "dep-data-v2"

# Now it should be cached again
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-eval-dep.nix" depVal | grepQuiet "dep-data-v2"

echo "Test 2 passed: nix eval --file cache invalidation"

###############################################################################
# Test 3: --write-to with --file (cursor path)
###############################################################################
clearStoreIndex

cat >"$testDir/test-write-to.nix" <<'EOF'
{
  fileData = {
    greeting = "hello from file";
    farewell = "goodbye from file";
  };
}
EOF

# Populate cache
nix eval --impure -f "$testDir/test-write-to.nix" fileData --json > /dev/null

# Use --write-to on warm cache
writeOutDir="$testDir/write-to-out"
nix eval --impure -f "$testDir/test-write-to.nix" fileData --write-to "$writeOutDir"
[[ "$(cat "$writeOutDir/greeting")" == "hello from file" ]]
[[ "$(cat "$writeOutDir/farewell")" == "goodbye from file" ]]

rm -rf "$writeOutDir"

# Use --write-to with NIX_ALLOW_EVAL=0 (fully from cache)
NIX_ALLOW_EVAL=0 nix eval --impure -f "$testDir/test-write-to.nix" fileData --write-to "$writeOutDir"
[[ "$(cat "$writeOutDir/greeting")" == "hello from file" ]]
[[ "$(cat "$writeOutDir/farewell")" == "goodbye from file" ]]

rm -rf "$writeOutDir"

echo "Test 3 passed: --write-to cursor path"

###############################################################################
# Test 4: nix eval --json with derivation (impure, --file)
###############################################################################
clearStoreIndex

cat >"$testDir/test-json-drv.nix" <<'EOF'
let inherit (import ./config.nix) mkDerivation; in
{
  drv = mkDerivation {
    name = "json-drv-impure";
    buildCommand = "echo hello > $out";
  };
}
EOF

# nix eval --json on a derivation should return a JSON string (store path)
drvJson="$(nix eval --json --impure -f "$testDir/test-json-drv.nix" drv)"
[[ "$drvJson" == '"'* ]]  # Must be a JSON string

# From cache with NIX_ALLOW_EVAL=0
drvJsonCached="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-json-drv.nix" drv)"
[[ "$drvJsonCached" == "$drvJson" ]]

echo "Test 4 passed: nix eval --json with derivation (impure)"

###############################################################################
# Test 5: nix eval --json with list of strings (ListOfStrings cache type)
###############################################################################
clearStoreIndex

cat >"$testDir/test-json-list.nix" <<'EOF'
{
  myList = [ "a" "b" "c" ];
}
EOF

# Populate cache
listJson="$(nix eval --json --impure -f "$testDir/test-json-list.nix" myList)"
[[ "$listJson" == '["a","b","c"]' ]]

# Re-eval to verify correctness
listJson2="$(nix eval --json --impure -f "$testDir/test-json-list.nix" myList)"
[[ "$listJson2" == '["a","b","c"]' ]]

# Warm cache should serve without evaluation
listJson3="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-json-list.nix" myList)"
[[ "$listJson3" == '["a","b","c"]' ]]

echo "Test 5 passed: nix eval --json with list of strings"

###############################################################################
# Test 6: nix eval --json with non-derivation outPath
###############################################################################
clearStoreIndex

cat >"$testDir/test-json-outpath.nix" <<'EOF'
{
  withOutPath = { outPath = "/nix/store/fake-path"; foo = 42; };
}
EOF

outPathJson="$(nix eval --json --impure -f "$testDir/test-json-outpath.nix" withOutPath)"
[[ "$outPathJson" == '"/nix/store/fake-path"' ]]

# From warm cache with NIX_ALLOW_EVAL=0
outPathJson2="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-json-outpath.nix" withOutPath)"
[[ "$outPathJson2" == '"/nix/store/fake-path"' ]]

echo "Test 6 passed: nix eval --json with non-derivation outPath"

###############################################################################
# Test 7: Modify-then-revert recovery
###############################################################################
clearStoreIndex

echo '"original"' > "$testDir/revert-dep.txt"

cat >"$testDir/test-revert.nix" <<'EOF'
{
  result = builtins.readFile ./revert-dep.txt;
}
EOF

# Cold cache: evaluate and cache "original"
[[ "$(nix eval --json --impure -f "$testDir/test-revert.nix" result)" == '"\"original\"\n"' ]]

# Warm cache: verify cached
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-revert.nix" result)" == '"\"original\"\n"' ]]

# Modify the dependency
echo '"modified"' > "$testDir/revert-dep.txt"

# Cache miss (dep changed)
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --json --impure -f "$testDir/test-revert.nix" result \
  | grepQuiet "not everything is cached"

# Re-evaluate to cache "modified"
[[ "$(nix eval --json --impure -f "$testDir/test-revert.nix" result)" == '"\"modified\"\n"' ]]

# Verify "modified" is cached
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-revert.nix" result)" == '"\"modified\"\n"' ]]

# Revert the dependency back to original
echo '"original"' > "$testDir/revert-dep.txt"

# Should recover "original" from deterministic output path — cache HIT
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-revert.nix" result)" == '"\"original\"\n"' ]]

echo "Test 7 passed: modify-then-revert recovery"

###############################################################################
# Test 8: Three-way version cycling recovers all versions
###############################################################################
clearStoreIndex

echo '"v1"' > "$testDir/cycle-dep.txt"

cat >"$testDir/test-cycle.nix" <<'EOF'
{
  result = builtins.readFile ./cycle-dep.txt;
}
EOF

# Cache v1
nix eval --json --impure -f "$testDir/test-cycle.nix" result
NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-cycle.nix" result

# Cache v2
echo '"v2"' > "$testDir/cycle-dep.txt"
nix eval --json --impure -f "$testDir/test-cycle.nix" result

# Cache v3
echo '"v3"' > "$testDir/cycle-dep.txt"
nix eval --json --impure -f "$testDir/test-cycle.nix" result

# Revert to v1 — should recover from deterministic output path
echo '"v1"' > "$testDir/cycle-dep.txt"
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-cycle.nix" result)" == '"\"v1\"\n"' ]]

# Switch to v2 — should also recover
echo '"v2"' > "$testDir/cycle-dep.txt"
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-cycle.nix" result)" == '"\"v2\"\n"' ]]

echo "Test 8 passed: three-way version cycling"

###############################################################################
# Test 9: Multi-attr recovery across multiple attributes
###############################################################################
clearStoreIndex

echo '"version-A"' > "$testDir/hash-recovery-dep.txt"

cat >"$testDir/test-hash-recovery.nix" <<'EOF'
let data = builtins.readFile ./hash-recovery-dep.txt; in
{
  attr1 = "first-${data}";
  attr2 = "second-${data}";
  attr3 = "third-${data}";
}
EOF

# Cold cache: evaluate all attributes
nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr1
nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr2
nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr3

# Warm cache
NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr1
NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr2
NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr3

# Modify file -> re-evaluate to create new cached versions
echo '"version-B"' > "$testDir/hash-recovery-dep.txt"
nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr1
nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr2
nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr3

# Revert to version-A -> recovery should find original output paths
echo '"version-A"' > "$testDir/hash-recovery-dep.txt"

# All attrs should be served from recovery (no cold eval needed)
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr1)" == '"first-\"version-A\"\n"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr2)" == '"second-\"version-A\"\n"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-hash-recovery.nix" attr3)" == '"third-\"version-A\"\n"' ]]

echo "Test 9 passed: multi-attr recovery across multiple attributes"

###############################################################################
# Test 10: Three-way cycling with multiple attributes
###############################################################################
clearStoreIndex

cat >"$testDir/diff-dep.txt" <<'EOF'
version-A
EOF

cat >"$testDir/test-diff-recovery.nix" <<'EOF'
{
  result = builtins.readFile ./diff-dep.txt;
}
EOF

# Version A: cold eval
result_a1="$(nix eval --json --impure -f "$testDir/test-diff-recovery.nix" result)"
[[ "$result_a1" == '"version-A\n"' ]]

# Version A: warm cache
result_a2="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-diff-recovery.nix" result)"
[[ "$result_a2" == '"version-A\n"' ]]

# Version B: cold eval
echo "version-B" > "$testDir/diff-dep.txt"
result_b="$(nix eval --json --impure -f "$testDir/test-diff-recovery.nix" result)"
[[ "$result_b" == '"version-B\n"' ]]

# Version C: cold eval
echo "version-C" > "$testDir/diff-dep.txt"
result_c="$(nix eval --json --impure -f "$testDir/test-diff-recovery.nix" result)"
[[ "$result_c" == '"version-C\n"' ]]

# Revert to version A: recovery should find original output path
echo "version-A" > "$testDir/diff-dep.txt"
result_recover="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-diff-recovery.nix" result)"
[[ "$result_recover" == '"version-A\n"' ]]

# Also verify version B can be recovered
echo "version-B" > "$testDir/diff-dep.txt"
result_recover_b="$(NIX_ALLOW_EVAL=0 nix eval --json --impure -f "$testDir/test-diff-recovery.nix" result)"
[[ "$result_recover_b" == '"version-B\n"' ]]

echo "Test 10 passed: three-way cycling recovery"

echo "All eval-cache-impure-output tests passed!"
