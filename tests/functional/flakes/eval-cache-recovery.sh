#!/usr/bin/env bash

# Eval cache recovery tests: dep revert, alternating versions, three-way cycling.
# Recovery uses dep content hash index: same deps -> same dep hash ->
# index lookup -> find matching trace -> cache hit.

source ./common.sh

requireGit

clearStoreIndex() {
    rm -f "$TEST_HOME/.cache/nix/eval-cache-v1.sqlite"
}

###############################################################################
# Test 1: Dep revert recovery (deterministic output path)
# Cache v1, modify -> cache v2, revert -> store path for v1 still exists
###############################################################################

t1Dir="$TEST_ROOT/recovery-revert"
createGitRepo "$t1Dir" ""

echo "version-1" > "$t1Dir/data.txt"

cat >"$t1Dir/flake.nix" <<EOF
{
  description = "Recovery revert test";
  outputs = { self }: {
    result = builtins.readFile ./data.txt;
  };
}
EOF

git -C "$t1Dir" add .
git -C "$t1Dir" commit -m "Version 1"

# Cache version 1
nix eval "$t1Dir#result" | grepQuiet "version-1"
NIX_ALLOW_EVAL=0 nix eval "$t1Dir#result" | grepQuiet "version-1"

# Modify to version 2
echo "version-2" > "$t1Dir/data.txt"
git -C "$t1Dir" add data.txt
git -C "$t1Dir" commit -m "Version 2"

# Cache version 2
nix eval "$t1Dir#result" | grepQuiet "version-2"
NIX_ALLOW_EVAL=0 nix eval "$t1Dir#result" | grepQuiet "version-2"

# Revert to version 1 — recovery should find v1's output path in store
echo "version-1" > "$t1Dir/data.txt"
git -C "$t1Dir" add data.txt
git -C "$t1Dir" commit -m "Revert to version 1"

# Should be served from store (deterministic output path recovery)
NIX_ALLOW_EVAL=0 nix eval "$t1Dir#result" | grepQuiet "version-1"

echo "Test 1 passed: dep revert recovery"

###############################################################################
# Test 2: Alternating flake versions with recovery
# Uses nix eval (not nix build) because recovery works at attribute level.
###############################################################################

t2Dir="$TEST_ROOT/recovery-alternating"
createGitRepo "$t2Dir" ""

echo "version-1" > "$t2Dir/data.txt"
cat >"$t2Dir/flake.nix" <<EOF
{
  description = "Alternating versions test";
  outputs = { self }: {
    result = builtins.readFile ./data.txt;
  };
}
EOF
git -C "$t2Dir" add .
git -C "$t2Dir" commit -m "Version 1"

# Cache version 1
nix eval "$t2Dir#result" | grepQuiet "version-1"
NIX_ALLOW_EVAL=0 nix eval "$t2Dir#result" | grepQuiet "version-1"

# Create version 2
echo "version-2" > "$t2Dir/data.txt"
git -C "$t2Dir" add data.txt
git -C "$t2Dir" commit -m "Version 2"

# Version 2 invalidates cache
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval "$t2Dir#result" \
  | grepQuiet "not everything is cached"

# Cache version 2
nix eval "$t2Dir#result" | grepQuiet "version-2"
NIX_ALLOW_EVAL=0 nix eval "$t2Dir#result" | grepQuiet "version-2"

# Revert to version 1 — deterministic output path recovery
echo "version-1" > "$t2Dir/data.txt"
git -C "$t2Dir" add data.txt
git -C "$t2Dir" commit -m "Back to version 1"

# Recovery: same deps -> same dep hash -> index lookup -> matching trace
NIX_ALLOW_EVAL=0 nix eval "$t2Dir#result" | grepQuiet "version-1"

echo "Test 2 passed: alternating flake versions with recovery"

###############################################################################
# Test 3: Three-way version cycling
# Cache A, B, C, then cycle back to A and B — all recovered from store.
###############################################################################

t3Dir="$TEST_ROOT/recovery-cycling"
createGitRepo "$t3Dir" ""

echo "version-A" > "$t3Dir/data.txt"
cat >"$t3Dir/flake.nix" <<EOF
{
  description = "Three-way cycling test";
  outputs = { self }: {
    result = builtins.readFile ./data.txt;
  };
}
EOF
git -C "$t3Dir" add .
git -C "$t3Dir" commit -m "Version A"

# Cache version A
nix eval "$t3Dir#result" | grepQuiet "version-A"
NIX_ALLOW_EVAL=0 nix eval "$t3Dir#result" | grepQuiet "version-A"

# Cache version B
echo "version-B" > "$t3Dir/data.txt"
git -C "$t3Dir" add data.txt
git -C "$t3Dir" commit -m "Version B"
nix eval "$t3Dir#result" | grepQuiet "version-B"

# Cache version C
echo "version-C" > "$t3Dir/data.txt"
git -C "$t3Dir" add data.txt
git -C "$t3Dir" commit -m "Version C"
nix eval "$t3Dir#result" | grepQuiet "version-C"

# Revert to version A — should recover from store
echo "version-A" > "$t3Dir/data.txt"
git -C "$t3Dir" add data.txt
git -C "$t3Dir" commit -m "Back to A"
NIX_ALLOW_EVAL=0 nix eval "$t3Dir#result" | grepQuiet "version-A"

# Switch to version B — should also recover
echo "version-B" > "$t3Dir/data.txt"
git -C "$t3Dir" add data.txt
git -C "$t3Dir" commit -m "Back to B"
NIX_ALLOW_EVAL=0 nix eval "$t3Dir#result" | grepQuiet "version-B"

echo "Test 3 passed: three-way version cycling"

echo "All eval-cache-recovery tests passed!"
