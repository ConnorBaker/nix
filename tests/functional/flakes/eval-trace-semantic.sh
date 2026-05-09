#!/usr/bin/env bash

# Semantic model verification tests for the eval-trace rewrite.
# Tests the plan's verification criteria:
#   - referenceLockFilePath with eval-trace warm-verify
#   - override-input session isolation (internal vs on-disk graph)
#   - path/text/context semantic replay with selective invalidation
#   - identity-sensitive equality round-trip across materialization
#   - relative-node lock structure assertions

source ./common.sh

requireGit

###############################################################################
# Test 1: --reference-lock-file with eval-trace warm-verify
###############################################################################

clearStoreIndex

t1Dir="$TEST_ROOT/semantic-reflock"
t1DepDir="$TEST_ROOT/semantic-reflock-dep"

# Create dependency flake with v1
createGitRepo "$t1DepDir" ""
cat >"$t1DepDir/flake.nix" <<'EOF'
{
  description = "Ref-lock dep";
  outputs = { self }: { value = "dep-v1"; };
}
EOF
git -C "$t1DepDir" add .
git -C "$t1DepDir" commit -m "dep v1"

# Create consumer flake
createGitRepo "$t1Dir" ""
cat >"$t1Dir/flake.nix" <<EOF
{
  description = "Ref-lock consumer";
  inputs.dep.url = "git+file://$t1DepDir";
  outputs = { self, dep }: { value = dep.value; };
}
EOF
git -C "$t1Dir" add .
git -C "$t1Dir" commit -m "Init"

# Lock to v1
nix flake lock "$t1Dir"
git -C "$t1Dir" add flake.lock
git -C "$t1Dir" commit -m "Lock v1"

# Save v1 lock file
cp "$t1Dir/flake.lock" "$TEST_ROOT/v1.lock"

# Update dep to v2
cat >"$t1DepDir/flake.nix" <<'EOF'
{
  description = "Ref-lock dep v2";
  outputs = { self }: { value = "dep-v2"; };
}
EOF
git -C "$t1DepDir" add .
git -C "$t1DepDir" commit -m "dep v2"

# Re-lock to v2
nix flake update dep --flake "$t1Dir"
git -C "$t1Dir" add flake.lock
git -C "$t1Dir" commit -m "Lock v2"

# Cold-record baseline (v2)
result1_v2=$(nix eval --json "$t1Dir#value")
[[ "$result1_v2" == *"dep-v2"* ]]

# Warm-verify baseline
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t1Dir#value")" == "$result1_v2" ]]

# Eval with --reference-lock-file pointing to v1 lock — should resolve to dep-v1
result1_ref=$(nix eval --json "$t1Dir#value" --reference-lock-file "$TEST_ROOT/v1.lock")
[[ "$result1_ref" == *"dep-v1"* ]]

# Warm-verify the reference-lock-file result
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t1Dir#value" --reference-lock-file "$TEST_ROOT/v1.lock")" == "$result1_ref" ]]

echo "Test 1 passed: --reference-lock-file with eval-trace warm-verify"

###############################################################################
# Test 2: Override-input session isolation
# The internal graph (with override) and on-disk graph (without override)
# must maintain separate session keys so neither evicts the other's cache.
###############################################################################

clearStoreIndex

t2Dir="$TEST_ROOT/semantic-session-iso"
t2DepDir="$TEST_ROOT/semantic-session-iso-dep"
t2AltDir="$TEST_ROOT/semantic-session-iso-alt"

# Create original dep
createGitRepo "$t2DepDir" ""
cat >"$t2DepDir/flake.nix" <<'EOF'
{
  description = "Session iso dep";
  outputs = { self }: { value = "original"; };
}
EOF
git -C "$t2DepDir" add .
git -C "$t2DepDir" commit -m "Init dep"

# Create alt dep
createGitRepo "$t2AltDir" ""
cat >"$t2AltDir/flake.nix" <<'EOF'
{
  description = "Session iso alt";
  outputs = { self }: { value = "alternate"; };
}
EOF
git -C "$t2AltDir" add .
git -C "$t2AltDir" commit -m "Init alt"

# Create consumer
createGitRepo "$t2Dir" ""
cat >"$t2Dir/flake.nix" <<EOF
{
  description = "Session iso consumer";
  inputs.dep.url = "git+file://$t2DepDir";
  outputs = { self, dep }: { value = dep.value; };
}
EOF
git -C "$t2Dir" add .
git -C "$t2Dir" commit -m "Init"

# Lock and cold-record baseline
nix flake lock "$t2Dir"
git -C "$t2Dir" add flake.lock
git -C "$t2Dir" commit -m "Lock"

result2_base=$(nix eval --json "$t2Dir#value")
[[ "$result2_base" == *"original"* ]]

# Warm-verify baseline
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t2Dir#value")" == "$result2_base" ]]

# Cold-record with override-input
result2_ovr=$(nix eval --json "$t2Dir#value" \
  --override-input dep "git+file://$t2AltDir" --no-write-lock-file)
[[ "$result2_ovr" == *"alternate"* ]]

# Warm-verify override session
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t2Dir#value" \
  --override-input dep "git+file://$t2AltDir" --no-write-lock-file)" == "$result2_ovr" ]]

# Re-verify baseline (no override) — must still warm-hit from original session
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t2Dir#value")" == "$result2_base" ]]

echo "Test 2 passed: override-input session isolation"

###############################################################################
# Test 3: Path/text/context semantic replay with selective invalidation
# Exercises all three provenance types and proves independent tracking.
###############################################################################

clearStoreIndex

t3Dir="$TEST_ROOT/semantic-replay"
createGitRepo "$t3Dir" ""

echo "hello-data" >"$t3Dir/data.txt"
cat >"$t3Dir/flake.nix" <<'EOF'
{
  description = "Semantic replay test";
  outputs = { self }: {
    # TextObject provenance (readFile)
    text = builtins.readFile ./data.txt;
    # PathObject provenance (path existence)
    pathCheck = builtins.pathExists ./data.txt;
    # ContextObject (string interpolation with context)
    combined = "prefix-${builtins.readFile ./data.txt}";
  };
}
EOF
git -C "$t3Dir" add .
git -C "$t3Dir" commit -m "Init"

# Cold-record all three
cold_text=$(nix eval --raw "$t3Dir#text")
cold_path=$(nix eval --json "$t3Dir#pathCheck")
cold_combined=$(nix eval --raw "$t3Dir#combined")

[[ "$cold_text" == "hello-data" ]]
[[ "$cold_path" == "true" ]]
[[ "$cold_combined" == "prefix-hello-data" ]]

# Warm-verify all three
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t3Dir#text")" == "$cold_text" ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t3Dir#pathCheck")" == "$cold_path" ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t3Dir#combined")" == "$cold_combined" ]]

# Modify data.txt — text and combined should invalidate, pathCheck should survive
echo "modified-data" >"$t3Dir/data.txt"
git -C "$t3Dir" add .
git -C "$t3Dir" commit -m "Modify data"

# pathCheck should still warm-verify (file still exists, existence unchanged)
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t3Dir#pathCheck")" == "true" ]]

# text and combined need re-evaluation (content changed)
new_text=$(nix eval --raw "$t3Dir#text")
[[ "$new_text" == "modified-data" ]]
new_combined=$(nix eval --raw "$t3Dir#combined")
[[ "$new_combined" == "prefix-modified-data" ]]

# Warm-verify the re-recorded results
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t3Dir#text")" == "$new_text" ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t3Dir#combined")" == "$new_combined" ]]

echo "Test 3 passed: path/text/context semantic replay with selective invalidation"

###############################################################################
# Test 4: Identity-sensitive equality round-trip
# Two references to the same attrset must compare equal after materialization.
# This tests IdentityObject stamp survival across the trace round-trip.
###############################################################################

clearStoreIndex

t4Dir="$TEST_ROOT/semantic-identity"
createGitRepo "$t4Dir" ""

cat >"$t4Dir/flake.nix" <<'EOF'
{
  description = "Identity equality test";
  outputs = { self }: let
    base = { x = 1; y = 2; z = 3; w = 4; };
    a = base;
    b = base;
  in {
    result = a == b;
  };
}
EOF
git -C "$t4Dir" add .
git -C "$t4Dir" commit -m "Init"

# Cold-record — must be true (pointer equality fast-path at record time)
[[ "$(nix eval --json "$t4Dir#result")" == "true" ]]

# Warm-verify — materialized result must be correct
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t4Dir#result")" == "true" ]]

echo "Test 4 passed: identity-sensitive equality round-trip"

###############################################################################
# Test 5: Relative-node lock structure assertions
# Verifies that relative path inputs have no content locks (no narHash)
# and no absolute paths leak into the lock file.
###############################################################################

clearStoreIndex

t5Dir="$TEST_ROOT/semantic-relative"
t5Sub="$t5Dir/sub"

createGitRepo "$t5Dir" ""
mkdir -p "$t5Sub"

cat >"$t5Sub/flake.nix" <<'EOF'
{
  description = "Relative sub-flake";
  outputs = { self }: { value = builtins.readFile ./data.txt; };
}
EOF
echo "sub-v1" >"$t5Sub/data.txt"

cat >"$t5Dir/flake.nix" <<'EOF'
{
  description = "Relative parent";
  inputs.sub.url = "path:./sub";
  outputs = { self, sub }: { value = sub.value; };
}
EOF

git -C "$t5Dir" add .
git -C "$t5Dir" commit -m "Init"

# Cold-record
result5=$(nix eval --json "$t5Dir#value")
[[ "$result5" == *"sub-v1"* ]]

# Warm-verify
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t5Dir#value")" == "$result5" ]]

# Lock structure assertions for relative inputs:
# No narHash (relative inputs have no content lock)
(! grep -q "narHash" "$t5Dir/flake.lock")

# No absolute paths leaked into lock file
(! grep -q "$TEST_ROOT" "$t5Dir/flake.lock")
if ! isTestOnNixOS; then
    (! grep -q "$NIX_STORE_DIR" "$t5Dir/flake.lock")
fi

# Modify file in subdir — should invalidate subdir traces
echo "sub-v2" >"$t5Sub/data.txt"
git -C "$t5Dir" add .
git -C "$t5Dir" commit -m "Update sub data"

result5b=$(nix eval --json "$t5Dir#value")
[[ "$result5b" == *"sub-v2"* ]]

# Warm-verify updated result
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t5Dir#value")" == "$result5b" ]]

echo "Test 5 passed: relative-node lock structure and eval-trace"

###############################################################################

echo "All eval-trace-semantic tests passed!"
