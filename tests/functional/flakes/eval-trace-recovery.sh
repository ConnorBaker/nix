#!/usr/bin/env bash

# Eval trace constructive recovery tests (BSàlC: constructive traces).
# When verifying trace fails but the current dependency fingerprint matches
# a previously recorded trace, the result is recovered without fresh evaluation.
# Mechanism: same deps -> same trace hash -> index lookup -> constructive hit.

source ./common.sh

requireGit

clearStoreIndex() {
    rm -rf "$TEST_HOME/.cache/nix/eval-trace"* "$TEST_HOME/.cache/nix/attr-vocab.sqlite"* "$TEST_HOME/.cache/nix/stat-hash-cache"*
}

###############################################################################
# Test 1: Constructive recovery via dependency revert (BSàlC: constructive trace)
# Record trace for v1, modify -> record trace for v2, revert -> constructive
# recovery finds v1's traced result via matching dependency fingerprint.
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

# Record trace for version 1 (BSàlC: trace recording)
nix eval "$t1Dir#result" | grepQuiet "version-1"
NIX_ALLOW_EVAL=0 nix eval "$t1Dir#result" | grepQuiet "version-1"

# Modify to version 2 — dirties version 1's trace
echo "version-2" > "$t1Dir/data.txt"
git -C "$t1Dir" add data.txt
git -C "$t1Dir" commit -m "Version 2"

# Record trace for version 2 (fresh evaluation after verify miss)
nix eval "$t1Dir#result" | grepQuiet "version-2"
NIX_ALLOW_EVAL=0 nix eval "$t1Dir#result" | grepQuiet "version-2"

# Revert to version 1 — constructive recovery finds v1's traced result
echo "version-1" > "$t1Dir/data.txt"
git -C "$t1Dir" add data.txt
git -C "$t1Dir" commit -m "Revert to version 1"

# Constructive hit — traced result served without fresh evaluation (BSàlC: constructive trace)
NIX_ALLOW_EVAL=0 nix eval "$t1Dir#result" | grepQuiet "version-1"

echo "Test 1 passed: constructive recovery via dependency revert"

###############################################################################
# Test 2: Alternating versions with constructive recovery
# Uses nix eval (not nix build) because constructive recovery operates at
# the attribute level (BSàlC: constructive trace lookup per key).
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

# Record trace for version 1 (BSàlC: trace recording)
nix eval "$t2Dir#result" | grepQuiet "version-1"
NIX_ALLOW_EVAL=0 nix eval "$t2Dir#result" | grepQuiet "version-1"

# Create version 2 — dirties version 1's trace
echo "version-2" > "$t2Dir/data.txt"
git -C "$t2Dir" add data.txt
git -C "$t2Dir" commit -m "Version 2"

# Verify miss — trace verification fails (dependency changed)
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval "$t2Dir#result" \
  | grepQuiet "not everything is cached"

# Record trace for version 2 (fresh evaluation)
nix eval "$t2Dir#result" | grepQuiet "version-2"
NIX_ALLOW_EVAL=0 nix eval "$t2Dir#result" | grepQuiet "version-2"

# Revert to version 1 — constructive recovery (BSàlC: constructive trace lookup)
echo "version-1" > "$t2Dir/data.txt"
git -C "$t2Dir" add data.txt
git -C "$t2Dir" commit -m "Back to version 1"

# Constructive hit: same deps -> same trace hash -> index lookup -> matching recorded result
NIX_ALLOW_EVAL=0 nix eval "$t2Dir#result" | grepQuiet "version-1"

echo "Test 2 passed: alternating versions with constructive recovery"

###############################################################################
# Test 3: Three-way version cycling with constructive recovery
# Record traces for A, B, C, then cycle back to A and B — all constructively
# recovered from previously recorded traces (BSàlC: constructive trace).
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

# Record trace for version A
nix eval "$t3Dir#result" | grepQuiet "version-A"
NIX_ALLOW_EVAL=0 nix eval "$t3Dir#result" | grepQuiet "version-A"

# Record trace for version B
echo "version-B" > "$t3Dir/data.txt"
git -C "$t3Dir" add data.txt
git -C "$t3Dir" commit -m "Version B"
nix eval "$t3Dir#result" | grepQuiet "version-B"

# Record trace for version C
echo "version-C" > "$t3Dir/data.txt"
git -C "$t3Dir" add data.txt
git -C "$t3Dir" commit -m "Version C"
nix eval "$t3Dir#result" | grepQuiet "version-C"

# Revert to version A — constructive recovery from recorded trace
echo "version-A" > "$t3Dir/data.txt"
git -C "$t3Dir" add data.txt
git -C "$t3Dir" commit -m "Back to A"
NIX_ALLOW_EVAL=0 nix eval "$t3Dir#result" | grepQuiet "version-A"

# Switch to version B — also constructively recovered
echo "version-B" > "$t3Dir/data.txt"
git -C "$t3Dir" add data.txt
git -C "$t3Dir" commit -m "Back to B"
NIX_ALLOW_EVAL=0 nix eval "$t3Dir#result" | grepQuiet "version-B"

echo "Test 3 passed: three-way version cycling with constructive recovery"

echo "All eval-trace-recovery tests passed! (BSàlC: constructive traces)"
