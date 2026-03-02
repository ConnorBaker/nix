#!/usr/bin/env bash

# Eval trace output tests: scalar type trace recording, --json drv/outPath/list
# traced result replay, --write-to from traced results, nix eval cursor evaluation.
# Verifies that recorded results (BSàlC) are faithfully replayed across types.

source ./common.sh

requireGit

clearStoreIndex() {
    rm -rf "$TEST_HOME/.cache/nix/eval-trace"* "$TEST_HOME/.cache/nix/attr-vocab.sqlite"* "$TEST_HOME/.cache/nix/stat-hash-cache"*
}

###############################################################################
# Test 1: Scalar type trace recording and verification via cursor path
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

# Fresh evaluation — records traces for all scalar types (BSàlC: trace recording)
nix eval "$t1Dir#myString" | grepQuiet "hello world"
[[ "$(nix eval --json "$t1Dir#myBool")" == 'true' ]]
[[ "$(nix eval --json "$t1Dir#myInt")" == '42' ]]
[[ "$(nix eval --json "$t1Dir#myNull")" == 'null' ]]
nix eval --raw "$t1Dir#myPath" | grepQuiet "flake.nix"

# Verification hits — traced results replayed faithfully (BSàlC: verifying trace succeeds)
NIX_ALLOW_EVAL=0 nix eval "$t1Dir#myString" | grepQuiet "hello world"
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t1Dir#myBool")" == 'true' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t1Dir#myInt")" == '42' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t1Dir#myNull")" == 'null' ]]

echo "Test 1 passed: nix eval scalar type trace recording and verification"

###############################################################################
# Test 2: Derivation trace recording — --json returns traced store path
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

# Fresh evaluation — records derivation trace, --json returns output path
drvJson="$(nix eval --json "$t2Dir#drv")"
[[ "$drvJson" == '"'* ]]

# Verification hit — traced derivation result replayed (BSàlC: verifying trace succeeds)
drvJsonCached="$(NIX_ALLOW_EVAL=0 nix eval --json "$t2Dir#drv")"
[[ "$drvJsonCached" == "$drvJson" ]]

echo "Test 2 passed: derivation trace recording and verification"

###############################################################################
# Test 3: --write-to from traced results via cursor path
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

# Record trace (BSàlC: trace recording)
nix eval "$t3Dir#writeTest" --json > /dev/null

# Use --write-to from traced result (verification hit)
outDir="$TEST_ROOT/output-write-to-out"
nix eval "$t3Dir#writeTest" --write-to "$outDir"
[[ "$(cat "$outDir/greeting")" == "hello" ]]
[[ "$(cat "$outDir/nested/inner")" == "world" ]]
rm -rf "$outDir"

# Use --write-to entirely from traced results (no evaluation permitted)
NIX_ALLOW_EVAL=0 nix eval "$t3Dir#writeTest" --write-to "$outDir"
[[ "$(cat "$outDir/greeting")" == "hello" ]]
[[ "$(cat "$outDir/nested/inner")" == "world" ]]
rm -rf "$outDir"

echo "Test 3 passed: --write-to from traced results"

###############################################################################
# Test 4: Non-derivation outPath trace recording and replay
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

# Fresh evaluation — traces outPath as the recorded result
outPathJson="$(nix eval --json "$t4Dir#withOutPath")"
[[ "$outPathJson" == '"/nix/store/fake-path"' ]]

# Verification hit — traced outPath replayed without evaluation
outPathJson2="$(NIX_ALLOW_EVAL=0 nix eval --json "$t4Dir#withOutPath")"
[[ "$outPathJson2" == '"/nix/store/fake-path"' ]]

echo "Test 4 passed: non-derivation outPath trace replay"

###############################################################################
# Test 5: List of strings trace recording (ListOfStrings fast path)
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

# Fresh evaluation — list recorded as ListOfStrings traced result
listJson="$(nix eval --json "$t5Dir#myList")"
[[ "$listJson" == '["alpha","beta","gamma"]' ]]

# Lists traced as ListOfStrings (all-string fast path).
# Verify traced result replays correct JSON.
listJson2="$(nix eval --json "$t5Dir#myList")"
[[ "$listJson2" == '["alpha","beta","gamma"]' ]]

# Verification hit — traced list served without rootLoader (no evaluation)
listJson3="$(NIX_ALLOW_EVAL=0 nix eval --json "$t5Dir#myList")"
[[ "$listJson3" == '["alpha","beta","gamma"]' ]]

echo "Test 5 passed: list of strings trace recording and replay"

echo "All eval-trace-output tests passed! (BSàlC: traced result fidelity across types)"
