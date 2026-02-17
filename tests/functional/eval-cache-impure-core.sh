#!/usr/bin/env bash

# Core impure eval cache tests: --file caching, --expr caching,
# file modification, selective invalidation, auto-called function,
# --argstr context isolation, --expr file import.

source common.sh

clearStoreIndex() {
    rm -f "$TEST_HOME/.cache/nix/eval-index-v1.sqlite"
}

testDir="$TEST_ROOT/eval-cache-impure"
mkdir -p "$testDir"

cp "${config_nix}" "$testDir/config.nix"

###############################################################################
# Test 1: Basic --file caching
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

# First build populates cache
nix build --no-link --impure -f "$testDir/test.nix" drv

# Second build should use cache
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test.nix" drv

echo "Test 1 passed: basic --file caching"

###############################################################################
# Test 2: File modification invalidation
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

# First build populates cache
nix build --no-link --impure -f "$testDir/test-readfile.nix" drv

# Verify cache works
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-readfile.nix" drv

# Modify the data file
echo "version 2" > "$testDir/data.txt"

# Cache should be invalidated
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-readfile.nix" drv \
  | grepQuiet "not everything is cached"

echo "Test 2 passed: file modification invalidation"

###############################################################################
# Test 3: Selective invalidation
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

# Build both
nix build --no-link --impure -f "$testDir/test-selective.nix" drv-a
nix build --no-link --impure -f "$testDir/test-selective.nix" drv-b

# Verify both cached
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-selective.nix" drv-a
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-selective.nix" drv-b

# Modify only file-a
echo "file-a-v2" > "$testDir/file-a.txt"

# drv-a should be invalidated
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/test-selective.nix" drv-a \
  | grepQuiet "not everything is cached"

# drv-b should still be cached (depends only on file-b)
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/test-selective.nix" drv-b

echo "Test 3 passed: selective invalidation"

###############################################################################
# Test 4: Auto-called function
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

# Build auto-called function
nix build --no-link --impure -f "$testDir/func.nix" drv

# Should be cached
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/func.nix" drv

echo "Test 4 passed: auto-called function"

###############################################################################
# Test 5: Different --argstr -> different cache
###############################################################################
clearStoreIndex

# Build with argstr name=hello
nix build --no-link --impure -f "$testDir/func.nix" drv --argstr name hello

# Should be cached for same args
NIX_ALLOW_EVAL=0 nix build --no-link --impure -f "$testDir/func.nix" drv --argstr name hello

# Different args should miss cache (different stable identity)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure -f "$testDir/func.nix" drv --argstr name world \
  | grepQuiet "not everything is cached"

echo "Test 5 passed: different --argstr uses different cache"

###############################################################################
# Test 6: --expr caching
###############################################################################
clearStoreIndex

# Build with --expr
nix build --no-link --impure --expr "let inherit (import $testDir/config.nix) mkDerivation; in mkDerivation { name = \"expr-cached\"; buildCommand = \"echo hello > \\\$out\"; }"

# Should be cached
NIX_ALLOW_EVAL=0 nix build --no-link --impure --expr "let inherit (import $testDir/config.nix) mkDerivation; in mkDerivation { name = \"expr-cached\"; buildCommand = \"echo hello > \\\$out\"; }"

echo "Test 6 passed: --expr caching"

###############################################################################
# Test 7: --expr with file import change
###############################################################################
clearStoreIndex

echo "expr-data-v1" > "$testDir/expr-data.txt"

# Build with --expr that imports a file
nix build --no-link --impure --expr "let inherit (import $testDir/config.nix) mkDerivation; data = builtins.readFile $testDir/expr-data.txt; in mkDerivation { name = \"expr-file-test\"; buildCommand = \"echo '\${data}' > \\\$out\"; }"

# Cache hit
NIX_ALLOW_EVAL=0 nix build --no-link --impure --expr "let inherit (import $testDir/config.nix) mkDerivation; data = builtins.readFile $testDir/expr-data.txt; in mkDerivation { name = \"expr-file-test\"; buildCommand = \"echo '\${data}' > \\\$out\"; }"

# Modify the imported file
echo "expr-data-v2" > "$testDir/expr-data.txt"

# Cache should be invalidated
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link --impure --expr "let inherit (import $testDir/config.nix) mkDerivation; data = builtins.readFile $testDir/expr-data.txt; in mkDerivation { name = \"expr-file-test\"; buildCommand = \"echo '\${data}' > \\\$out\"; }" \
  | grepQuiet "not everything is cached"

echo "Test 7 passed: --expr with file import change"

echo "All eval-cache-impure-core tests passed!"
