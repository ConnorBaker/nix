#!/usr/bin/env bash

# Eval trace volatile dependency tests: currentTime always fails verification,
# mixed volatile/non-volatile trace dependencies, per-attribute volatile handling,
# volatile root dependency propagation through trace hierarchy.
# Volatile deps are oracle inputs (Shake) that always return a new value,
# so verifying traces containing them always fails (BSàlC).

source ./common.sh

requireGit

clearStoreIndex() {
    rm -rf "$TEST_HOME/.cache/nix/eval-trace"* "$TEST_HOME/.cache/nix/attr-vocab.sqlite"* "$TEST_HOME/.cache/nix/stat-hash-cache"*
}

###############################################################################
# Test 1: builtins.currentTime — trace verification always fails (volatile oracle)
###############################################################################

t1Dir="$TEST_ROOT/volatile-currenttime"
createGitRepo "$t1Dir" ""

cat >"$t1Dir/flake.nix" <<EOF
{
  description = "Volatile dep test";
  outputs = { self, ... }: {
    result = builtins.toString builtins.currentTime;
  };
}
EOF
git -C "$t1Dir" add flake.nix && git -C "$t1Dir" commit -m "Init"

# Fresh evaluation — records trace with volatile currentTime dependency
nix eval --impure "$t1Dir#result" >/dev/null 2>&1

# Verify miss — volatile oracle (currentTime) always fails trace verification
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure "$t1Dir#result" \
  | grepQuiet "not everything is cached"

echo "Test 1 passed: builtins.currentTime — trace verification always fails"

###############################################################################
# Test 2: Mixed volatile and non-volatile trace dependencies
###############################################################################

t2Dir="$TEST_ROOT/volatile-mixed"
createGitRepo "$t2Dir" ""

echo "stable-content" > "$t2Dir/data.txt"

cat >"$t2Dir/flake.nix" <<'EOF'
{
  description = "Mixed volatile dep test";
  outputs = { self, ... }: {
    result = builtins.readFile ./data.txt + builtins.toString builtins.currentTime;
  };
}
EOF
git -C "$t2Dir" add flake.nix data.txt && git -C "$t2Dir" commit -m "Init"

# Fresh evaluation — records trace with both stable and volatile dependencies
nix eval --impure "$t2Dir#result" >/dev/null 2>&1

# Verify miss — any volatile dependency in the trace forces fresh evaluation
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure "$t2Dir#result" \
  | grepQuiet "not everything is cached"

echo "Test 2 passed: mixed volatile + non-volatile traces always fail verification"

###############################################################################
# Test 3: Non-volatile sibling — verification hit despite volatile sibling
###############################################################################

t3Dir="$TEST_ROOT/volatile-sibling"
createGitRepo "$t3Dir" ""

echo "stable-content" > "$t3Dir/data.txt"

cat >"$t3Dir/flake.nix" <<'EOF'
{
  description = "Volatile sibling test";
  outputs = { self, ... }: {
    volatile = builtins.toString builtins.currentTime;
    stable = builtins.readFile ./data.txt;
  };
}
EOF
git -C "$t3Dir" add flake.nix data.txt && git -C "$t3Dir" commit -m "Init"

# Record traces for both attributes (each has its own trace)
nix eval --impure "$t3Dir#volatile" >/dev/null 2>&1
nix eval --impure "$t3Dir#stable" >/dev/null 2>&1

# volatile: verify miss — trace contains volatile oracle, verification always fails
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure "$t3Dir#volatile" \
  | grepQuiet "not everything is cached"

# stable: verification hit — its trace has no volatile dependencies (Adapton: per-node dirtying)
NIX_ALLOW_EVAL=0 nix eval --impure "$t3Dir#stable" >/dev/null 2>&1

echo "Test 3 passed: non-volatile sibling verification hit despite volatile sibling"

###############################################################################
# Test 4: Volatile root dependency propagation (Shake: transitive dirtying)
###############################################################################

t4Dir="$TEST_ROOT/volatile-root"
createGitRepo "$t4Dir" ""

cat >"$t4Dir/flake.nix" <<EOF
{
  description = "Volatile root dep test";
  outputs = { self, ... }:
    assert builtins.currentTime > 0;
    { result = "hello"; };
}
EOF
git -C "$t4Dir" add flake.nix && git -C "$t4Dir" commit -m "Init"

# Fresh evaluation — records trace with volatile root dependency
nix eval --impure "$t4Dir#result" >/dev/null 2>&1

# Verify miss — root trace contains volatile oracle (currentTime), so all children's
# traces fail verification too (BSàlC: verifying trace propagates through parent chain)
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure "$t4Dir#result" \
  | grepQuiet "not everything is cached"

echo "Test 4 passed: volatile root dependency propagates to child traces"

echo "All eval-trace-volatile tests passed! (BSàlC: volatile oracle handling in verifying traces)"
