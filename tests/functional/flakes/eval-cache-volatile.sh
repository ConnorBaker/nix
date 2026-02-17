#!/usr/bin/env bash

# Eval cache volatile dependency tests: currentTime always invalidates,
# mixed volatile/non-volatile deps, per-attribute volatile handling,
# volatile root dep propagation.

source ./common.sh

requireGit

clearStoreIndex() {
    rm -f "$TEST_HOME/.cache/nix/eval-index-v1.sqlite"
}

###############################################################################
# Test 1: builtins.currentTime always invalidates
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

# Cold eval — populates cache
nix eval --impure "$t1Dir#result" >/dev/null 2>&1

# currentTime deps must ALWAYS invalidate
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure "$t1Dir#result" \
  | grepQuiet "not everything is cached"

echo "Test 1 passed: builtins.currentTime always invalidates"

###############################################################################
# Test 2: Mixed volatile and non-volatile deps
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

# Cold eval
nix eval --impure "$t2Dir#result" >/dev/null 2>&1

# Must invalidate: currentTime dep means ALWAYS re-eval
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure "$t2Dir#result" \
  | grepQuiet "not everything is cached"

echo "Test 2 passed: mixed volatile + non-volatile deps always invalidate"

###############################################################################
# Test 3: Non-volatile sibling of volatile attribute stays cached
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

# Cold eval both attributes
nix eval --impure "$t3Dir#volatile" >/dev/null 2>&1
nix eval --impure "$t3Dir#stable" >/dev/null 2>&1

# volatile must always invalidate
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure "$t3Dir#volatile" \
  | grepQuiet "not everything is cached"

# stable must be served from cache (file unchanged)
NIX_ALLOW_EVAL=0 nix eval --impure "$t3Dir#stable" >/dev/null 2>&1

echo "Test 3 passed: non-volatile sibling stays cached while volatile sibling invalidates"

###############################################################################
# Test 4: Volatile dep with root-level assert (root dep set is volatile)
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

# Cold eval
nix eval --impure "$t4Dir#result" >/dev/null 2>&1

# The root has currentTime dep (from assert), so the entire tree must invalidate.
# The child "result" inherits the volatile root dep via the inputDrvs chain.
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --impure "$t4Dir#result" \
  | grepQuiet "not everything is cached"

echo "Test 4 passed: volatile root assert invalidates children"

echo "All eval-cache-volatile tests passed!"
