#!/usr/bin/env bash

# Eval cache output tests: scalar types, --json drv/outPath/__toString/list,
# --write-to, nix eval cursor evaluation.

source ./common.sh

requireGit

clearStoreIndex() {
    rm -f "$TEST_HOME/.cache/nix/eval-index-v1.sqlite"
}

###############################################################################
# Test 1: nix eval scalar types via cursor path
###############################################################################

t1Dir="$TEST_ROOT/output-eval-types"
createGitRepo "$t1Dir" ""

cat >"$t1Dir/flake.nix" <<EOF
{
  description = "Scalar types test";
  outputs = { self }: {
    myString = "hello world";
    myBool = true;
    myInt = 42;
    myNull = null;
    myPath = ./flake.nix;
  };
}
EOF

git -C "$t1Dir" add .
git -C "$t1Dir" commit -m "Init"

# Cold cache
nix eval "$t1Dir#myString" | grepQuiet "hello world"
[[ "$(nix eval --json "$t1Dir#myBool")" == 'true' ]]
[[ "$(nix eval --json "$t1Dir#myInt")" == '42' ]]
[[ "$(nix eval --json "$t1Dir#myNull")" == 'null' ]]
nix eval --raw "$t1Dir#myPath" | grepQuiet "flake.nix"

# Warm cache
NIX_ALLOW_EVAL=0 nix eval "$t1Dir#myString" | grepQuiet "hello world"
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t1Dir#myBool")" == 'true' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t1Dir#myInt")" == '42' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t1Dir#myNull")" == 'null' ]]

echo "Test 1 passed: nix eval scalar types"

###############################################################################
# Test 2: nix eval --json with derivation
###############################################################################

t2Dir="$TEST_ROOT/output-json-drv"
createGitRepo "$t2Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t2Dir/"

cat >"$t2Dir/flake.nix" <<EOF
{
  description = "JSON drv test";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    drv = mkDerivation {
      name = "json-test";
      buildCommand = ''
        echo true > \$out
      '';
    };
  };
}
EOF

git -C "$t2Dir" add .
git -C "$t2Dir" commit -m "Init"

# nix eval --json on derivation returns a JSON string (output path)
drvJson="$(nix eval --json "$t2Dir#drv")"
[[ "$drvJson" == '"'* ]]

# From cache
drvJsonCached="$(NIX_ALLOW_EVAL=0 nix eval --json "$t2Dir#drv")"
[[ "$drvJsonCached" == "$drvJson" ]]

echo "Test 2 passed: nix eval --json with derivation"

###############################################################################
# Test 3: --write-to with cursor path
###############################################################################

t3Dir="$TEST_ROOT/output-write-to"
createGitRepo "$t3Dir" ""

cat >"$t3Dir/flake.nix" <<EOF
{
  description = "write-to test";
  outputs = { self }: {
    writeTest = {
      greeting = "hello";
      nested = {
        inner = "world";
      };
    };
  };
}
EOF

git -C "$t3Dir" add .
git -C "$t3Dir" commit -m "Init"

# Populate cache
nix eval "$t3Dir#writeTest" --json > /dev/null

# Use --write-to on warm cache
outDir="$TEST_ROOT/output-write-to-out"
nix eval "$t3Dir#writeTest" --write-to "$outDir"
[[ "$(cat "$outDir/greeting")" == "hello" ]]
[[ "$(cat "$outDir/nested/inner")" == "world" ]]
rm -rf "$outDir"

# Use --write-to with NIX_ALLOW_EVAL=0
NIX_ALLOW_EVAL=0 nix eval "$t3Dir#writeTest" --write-to "$outDir"
[[ "$(cat "$outDir/greeting")" == "hello" ]]
[[ "$(cat "$outDir/nested/inner")" == "world" ]]
rm -rf "$outDir"

echo "Test 3 passed: --write-to cursor path"

###############################################################################
# Test 4: nix eval --json with non-derivation outPath
###############################################################################

t4Dir="$TEST_ROOT/output-outpath"
createGitRepo "$t4Dir" ""

cat >"$t4Dir/flake.nix" <<EOF
{
  description = "outPath JSON test";
  outputs = { self }: {
    withOutPath = { outPath = "/nix/store/fake-path"; foo = 42; };
  };
}
EOF

git -C "$t4Dir" add .
git -C "$t4Dir" commit -m "Init"

# nix eval --json on non-derivation attrset with outPath should return the outPath
outPathJson="$(nix eval --json "$t4Dir#withOutPath")"
[[ "$outPathJson" == '"/nix/store/fake-path"' ]]

# From warm cache with NIX_ALLOW_EVAL=0
outPathJson2="$(NIX_ALLOW_EVAL=0 nix eval --json "$t4Dir#withOutPath")"
[[ "$outPathJson2" == '"/nix/store/fake-path"' ]]

echo "Test 4 passed: nix eval --json with non-derivation outPath"

###############################################################################
# Test 5: nix eval --json with list of strings
###############################################################################

t5Dir="$TEST_ROOT/output-list"
createGitRepo "$t5Dir" ""

cat >"$t5Dir/flake.nix" <<EOF
{
  description = "List JSON test";
  outputs = { self }: {
    myList = [ "alpha" "beta" "gamma" ];
  };
}
EOF

git -C "$t5Dir" add .
git -C "$t5Dir" commit -m "Init"

# nix eval --json on list of strings should return a JSON array
listJson="$(nix eval --json "$t5Dir#myList")"
[[ "$listJson" == '["alpha","beta","gamma"]' ]]

# Lists are cached as ListOfStrings (all-string fast path).
# Verify warm cache returns correct JSON.
listJson2="$(nix eval --json "$t5Dir#myList")"
[[ "$listJson2" == '["alpha","beta","gamma"]' ]]

# Warm cache should serve list without rootLoader
listJson3="$(NIX_ALLOW_EVAL=0 nix eval --json "$t5Dir#myList")"
[[ "$listJson3" == '["alpha","beta","gamma"]' ]]

echo "Test 5 passed: nix eval --json with list of strings"

echo "All eval-cache-output tests passed!"
