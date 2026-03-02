#!/usr/bin/env bash

# Core impure eval trace tests: --file trace recording, --expr trace recording,
# file modification trace invalidation, selective trace verification,
# auto-called function traces, --argstr context isolation (separate trace keys),
# --expr file import trace dependencies.
# Terminology follows Build Systems à la Carte (BSàlC) and Adapton.

source common.sh

clearStoreIndex() {
    rm -rf "$TEST_HOME/.cache/nix/eval-trace"* "$TEST_HOME/.cache/nix/attr-vocab.sqlite"* "$TEST_HOME/.cache/nix/stat-hash-cache"*
}

testDir="$TEST_ROOT/eval-trace-impure"
mkdir -p "$testDir"

cp "${config_nix}" "$testDir/config.nix"

###############################################################################
# Test 1: Basic --file trace recording and verification (BSàlC: verifying trace)
###############################################################################
clearStoreIndex

cat >"$testDir/test.nix" <<'EOF'
let inherit (import ./config.nix) mkDerivation; in
{
  drv = mkDerivation {
    name = "file-cached";
    buildCommand = "echo hello > $out";
  };
}
EOF

# Fresh evaluation records trace (BSàlC: trace recording)
nix build --no-link --impure -f "$testDir/test.nix" drv

# Verification hit — traced result served without re-evaluation (BSàlC: verifying trace succeeds)
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test.nix" drv

echo "Test 1 passed: basic --file trace recording and verification"

###############################################################################
# Test 2: File modification — trace verification fails (BSàlC: verifying trace)
###############################################################################
clearStoreIndex

cat >"$testDir/data.txt" <<'EOF'
version 1
EOF

cat >"$testDir/test-readfile.nix" <<'EOF'
let
  inherit (import ./config.nix) mkDerivation;
  data = builtins.readFile ./data.txt;
in
{
  drv = mkDerivation {
    name = "readfile-cached";
    buildCommand = "echo '${data}' > $out";
  };
}
EOF

# Fresh evaluation records trace (BSàlC: trace recording)
nix build --no-link --impure -f "$testDir/test-readfile.nix" drv

# Verification hit — trace valid
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-readfile.nix" drv

# Modify the data file — dirties the trace dependency
echo "version 2" > "$testDir/data.txt"

# Verify miss — trace verification fails because content dependency changed
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-readfile.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 2 passed: file modification — trace verification fails"

###############################################################################
# Test 3: Selective trace invalidation (Adapton: demand-driven dirtying)
###############################################################################
clearStoreIndex

echo "file-a-v1" > "$testDir/file-a.txt"
echo "file-b-v1" > "$testDir/file-b.txt"

cat >"$testDir/test-selective.nix" <<'EOF'
let
  inherit (import ./config.nix) mkDerivation;
  dataA = builtins.readFile ./file-a.txt;
  dataB = builtins.readFile ./file-b.txt;
in
{
  drv-a = mkDerivation {
    name = "selective-a";
    buildCommand = "echo '${dataA}' > $out";
  };
  drv-b = mkDerivation {
    name = "selective-b";
    buildCommand = "echo '${dataB}' > $out";
  };
}
EOF

# Record traces for both attributes
nix build --no-link --impure -f "$testDir/test-selective.nix" drv-a
nix build --no-link --impure -f "$testDir/test-selective.nix" drv-b

# Both verification hits — traces valid
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-selective.nix" drv-a
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-selective.nix" drv-b

# Modify only file-a — selectively dirties drv-a's trace
echo "file-a-v2" > "$testDir/file-a.txt"

# drv-a: verify miss — trace verification fails (file-a.txt changed)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-selective.nix" drv-a \
  | grepQuiet "not everything is cached"

# drv-b: verification hit — its trace depends only on file-b (unmodified)
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-selective.nix" drv-b

echo "Test 3 passed: selective trace invalidation"

###############################################################################
# Test 4: Auto-called function trace recording
###############################################################################
clearStoreIndex

cat >"$testDir/func.nix" <<'EOF'
{ name ? "default-name" }:
let inherit (import ./config.nix) mkDerivation; in
{
  drv = mkDerivation {
    inherit name;
    buildCommand = "echo ${name} > $out";
  };
}
EOF

# Fresh evaluation — records trace for auto-called function result
nix build --no-link --impure -f "$testDir/func.nix" drv

# Verification hit — traced result valid
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/func.nix" drv

echo "Test 4 passed: auto-called function trace recording"

###############################################################################
# Test 5: Different --argstr -> different trace key (Salsa: query parameter isolation)
###############################################################################
clearStoreIndex

# Record trace with argstr name=hello
nix build --no-link --impure -f "$testDir/func.nix" drv --argstr name hello

# Verification hit — same trace key (same args)
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/func.nix" drv --argstr name hello

# Different args produce a different trace key — no matching trace exists (verify miss)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/func.nix" drv --argstr name world \
  | grepQuiet "not everything is cached"

echo "Test 5 passed: different --argstr uses different trace key"

###############################################################################
# Test 6: --expr trace recording and verification
###############################################################################
clearStoreIndex

# Fresh evaluation — records trace for --expr (BSàlC: trace recording)
nix build --no-link --impure --expr "let inherit (import $testDir/config.nix) mkDerivation; in mkDerivation { name = \"expr-cached\"; buildCommand = \"echo hello > \\\$out\"; }"

# Verification hit — traced result valid
NIX_ALLOW_EVAL=0 nix build --no-link --impure --expr "let inherit (import $testDir/config.nix) mkDerivation; in mkDerivation { name = \"expr-cached\"; buildCommand = \"echo hello > \\\$out\"; }"

echo "Test 6 passed: --expr trace recording and verification"

###############################################################################
# Test 7: --expr with file import — trace verification fails on dependency change
###############################################################################
clearStoreIndex

echo "expr-data-v1" > "$testDir/expr-data.txt"

# Record trace for --expr that imports a file
nix build --no-link --impure --expr "let inherit (import $testDir/config.nix) mkDerivation; data = builtins.readFile $testDir/expr-data.txt; in mkDerivation { name = \"expr-file-test\"; buildCommand = \"echo '\${data}' > \\\$out\"; }"

# Verification hit — trace dependencies unchanged
NIX_ALLOW_EVAL=0 nix build --no-link --impure --expr "let inherit (import $testDir/config.nix) mkDerivation; data = builtins.readFile $testDir/expr-data.txt; in mkDerivation { name = \"expr-file-test\"; buildCommand = \"echo '\${data}' > \\\$out\"; }"

# Modify the imported file — dirties the content dependency in the trace
echo "expr-data-v2" > "$testDir/expr-data.txt"

# Verify miss — trace verification fails (imported file content changed)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure --expr "let inherit (import $testDir/config.nix) mkDerivation; data = builtins.readFile $testDir/expr-data.txt; in mkDerivation { name = \"expr-file-test\"; buildCommand = \"echo '\${data}' > \\\$out\"; }" \
  | grepQuiet "not everything is cached"

echo "Test 7 passed: --expr trace verification fails on file import change"

echo "All eval-trace-impure-core tests passed! (BSàlC: verifying and recording traces)"
