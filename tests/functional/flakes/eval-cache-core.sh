#!/usr/bin/env bash

# Core eval cache tests: cold/warm paths, fine-grained deps, parent chain,
# sibling caching, dep sharing, GC resilience, error caching, stack overflow.

source ./common.sh

requireGit

clearStoreIndex() {
    rm -f "$TEST_HOME/.cache/nix/eval-index-v1.sqlite"
}

###############################################################################
# Test 1: Basic cold/warm path
###############################################################################

t1Dir="$TEST_ROOT/core-basic"
createGitRepo "$t1Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t1Dir/"

cat >"$t1Dir/flake.nix" <<EOF
{
  description = "Basic cold/warm test";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    drv = mkDerivation {
      name = "core-basic";
      buildCommand = ''
        echo true > \$out
      '';
    };
  };
}
EOF

git -C "$t1Dir" add .
git -C "$t1Dir" commit -m "Init"

# Cold cache
nix build --no-link "$t1Dir#drv"

# Warm cache — must be served entirely from cache
NIX_ALLOW_EVAL=0 nix build --no-link "$t1Dir#drv"

echo "Test 1 passed: basic cold/warm path"

###############################################################################
# Test 2: Fine-grained dep invalidation
###############################################################################

t2Dir="$TEST_ROOT/core-fine-grained"
createGitRepo "$t2Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t2Dir/"

cat >"$t2Dir/a-data.txt" <<'EOF'
hello-from-a
EOF

cat >"$t2Dir/b-data.txt" <<'EOF'
hello-from-b
EOF

cat >"$t2Dir/flake.nix" <<EOF
{
  description = "Fine-grained dep test";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    drvA = builtins.seq (builtins.readFile ./a-data.txt) (mkDerivation {
      name = "drv-a";
      buildCommand = ''
        echo true > \$out
      '';
    });
    drvB = builtins.seq (builtins.readFile ./b-data.txt) (mkDerivation {
      name = "drv-b";
      buildCommand = ''
        echo true > \$out
      '';
    });
  };
}
EOF

git -C "$t2Dir" add .
git -C "$t2Dir" commit -m "Init"

# Populate cache
nix build --no-link "$t2Dir#drvA" "$t2Dir#drvB"

# Both cached
NIX_ALLOW_EVAL=0 nix build --no-link "$t2Dir#drvA"
NIX_ALLOW_EVAL=0 nix build --no-link "$t2Dir#drvB"

# Modify only b-data.txt
echo "modified-b" > "$t2Dir/b-data.txt"
git -C "$t2Dir" add b-data.txt
git -C "$t2Dir" commit -m "Modify b-data.txt"

# drvA should still be cached (depends only on a-data.txt)
NIX_ALLOW_EVAL=0 nix build --no-link "$t2Dir#drvA"

# drvB should be invalidated (b-data.txt changed)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link "$t2Dir#drvB" \
  | grepQuiet "not everything is cached"

# Re-evaluate drvB, then verify it's cached again
nix build --no-link "$t2Dir#drvB"
NIX_ALLOW_EVAL=0 nix build --no-link "$t2Dir#drvB"

# Modify flake.nix itself (root dep) — all cached attributes should invalidate
cat >"$t2Dir/flake.nix" <<EOF
{
  description = "Modified description";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    drvA = builtins.seq (builtins.readFile ./a-data.txt) (mkDerivation {
      name = "drv-a";
      buildCommand = ''
        echo true > \$out
      '';
    });
    drvB = builtins.seq (builtins.readFile ./b-data.txt) (mkDerivation {
      name = "drv-b";
      buildCommand = ''
        echo true > \$out
      '';
    });
  };
}
EOF
git -C "$t2Dir" add flake.nix
git -C "$t2Dir" commit -m "Modify flake.nix description"

# Both should be invalidated since flake.nix changed (root dep)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link "$t2Dir#drvA" \
  | grepQuiet "not everything is cached"
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link "$t2Dir#drvB" \
  | grepQuiet "not everything is cached"

# Re-populate and verify cache works again
nix build --no-link "$t2Dir#drvA" "$t2Dir#drvB"
NIX_ALLOW_EVAL=0 nix build --no-link "$t2Dir#drvA"
NIX_ALLOW_EVAL=0 nix build --no-link "$t2Dir#drvB"

echo "Test 2 passed: fine-grained dep invalidation"

###############################################################################
# Test 3: Sibling caching via navigateToReal wrapping
###############################################################################

t3Dir="$TEST_ROOT/core-siblings"
createGitRepo "$t3Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t3Dir/"

cat >"$t3Dir/a-data.txt" <<'EOF'
data-a
EOF

cat >"$t3Dir/b-data.txt" <<'EOF'
data-b
EOF

cat >"$t3Dir/flake.nix" <<EOF
{
  description = "Sibling caching test";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    drvA = builtins.seq (builtins.readFile ./a-data.txt) (mkDerivation {
      name = "sibling-a";
      buildCommand = ''
        echo true > \$out
      '';
    });
    drvB = builtins.seq (builtins.readFile ./b-data.txt) (mkDerivation {
      name = "sibling-b";
      buildCommand = ''
        echo true > \$out
      '';
    });
  };
}
EOF

git -C "$t3Dir" add .
git -C "$t3Dir" commit -m "Init"

# Build both derivations — populate cache for both
nix build --no-link "$t3Dir#drvA" "$t3Dir#drvB"

# Both should be cached
NIX_ALLOW_EVAL=0 nix build --no-link "$t3Dir#drvA"
NIX_ALLOW_EVAL=0 nix build --no-link "$t3Dir#drvB"

# Modify a-data.txt — drvB should still be cached (independent dep)
echo "modified-a" > "$t3Dir/a-data.txt"
git -C "$t3Dir" add a-data.txt
git -C "$t3Dir" commit -m "Modify a-data.txt"

# drvB should still be cached — its deps haven't changed
NIX_ALLOW_EVAL=0 nix build --no-link "$t3Dir#drvB"

# drvA should be invalidated
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link "$t3Dir#drvA" \
  | grepQuiet "not everything is cached"

echo "Test 3 passed: sibling caching"

###############################################################################
# Test 4: Parent chain invalidation
###############################################################################

t4Dir="$TEST_ROOT/core-parent-chain"
createGitRepo "$t4Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t4Dir/"

cat >"$t4Dir/a-data.txt" <<'EOF'
alpha
EOF

cat >"$t4Dir/flake.nix" <<EOF
{
  description = "Parent chain test";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    drvA = builtins.seq (builtins.readFile ./a-data.txt) (mkDerivation {
      name = "chain-a";
      buildCommand = ''
        echo true > \$out
      '';
    });
  };
}
EOF

git -C "$t4Dir" add .
git -C "$t4Dir" commit -m "Init"

# Populate cache
nix build --no-link "$t4Dir#drvA"
NIX_ALLOW_EVAL=0 nix build --no-link "$t4Dir#drvA"

# Modify flake.nix (root dep) — description change only
cat >"$t4Dir/flake.nix" <<EOF
{
  description = "Modified description";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    drvA = builtins.seq (builtins.readFile ./a-data.txt) (mkDerivation {
      name = "chain-a";
      buildCommand = ''
        echo true > \$out
      '';
    });
  };
}
EOF
git -C "$t4Dir" add flake.nix
git -C "$t4Dir" commit -m "Modify description"

# drvA should be invalidated because flake.nix changed (root dep in parent chain)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link "$t4Dir#drvA" \
  | grepQuiet "not everything is cached"

# Re-populate
nix build --no-link "$t4Dir#drvA"
NIX_ALLOW_EVAL=0 nix build --no-link "$t4Dir#drvA"

echo "Test 4 passed: parent chain invalidation"

###############################################################################
# Test 5: GC resilience
###############################################################################

t5Dir="$TEST_ROOT/core-gc"
createGitRepo "$t5Dir" ""

cat >"$t5Dir/flake.nix" <<EOF
{
  description = "GC resilience test";
  outputs = { self }: {
    result = "gc-test-value";
  };
}
EOF

git -C "$t5Dir" add .
git -C "$t5Dir" commit -m "Init"

# Populate cache
nix eval "$t5Dir#result" | grepQuiet "gc-test-value"
NIX_ALLOW_EVAL=0 nix eval "$t5Dir#result" | grepQuiet "gc-test-value"

# Run GC to remove cached store paths
nix-collect-garbage >/dev/null 2>&1

# Clear the index too (since GC'd paths are invalid)
clearStoreIndex

# Should still work — cold path re-evaluates
nix eval "$t5Dir#result" | grepQuiet "gc-test-value"

# And should be cached again
NIX_ALLOW_EVAL=0 nix eval "$t5Dir#result" | grepQuiet "gc-test-value"

echo "Test 5 passed: GC resilience"

###############################################################################
# Test 6: Dep sharing
###############################################################################

t6Dir="$TEST_ROOT/core-dep-sharing"
createGitRepo "$t6Dir" ""

echo "shared-data" > "$t6Dir/shared.txt"

cat >"$t6Dir/flake.nix" <<EOF
{
  description = "Dep sharing test";
  outputs = { self }: let
    data = builtins.readFile ./shared.txt;
  in {
    a = "first-\${data}";
    b = "second-\${data}";
  };
}
EOF

git -C "$t6Dir" add .
git -C "$t6Dir" commit -m "Init"

# Populate cache for both attributes
nix eval --json "$t6Dir#a" | grepQuiet "first-shared-data"
nix eval --json "$t6Dir#b" | grepQuiet "second-shared-data"

# Both should be cached
NIX_ALLOW_EVAL=0 nix eval --json "$t6Dir#a" | grepQuiet "first-shared-data"
NIX_ALLOW_EVAL=0 nix eval --json "$t6Dir#b" | grepQuiet "second-shared-data"

# Modify the shared dep — both should invalidate
echo "modified-shared" > "$t6Dir/shared.txt"
git -C "$t6Dir" add shared.txt
git -C "$t6Dir" commit -m "Modify shared"

NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --json "$t6Dir#a" \
  | grepQuiet "not everything is cached"

echo "Test 6 passed: dep sharing"

###############################################################################
# Test 7: Error caching
###############################################################################

t7Dir="$TEST_ROOT/core-errors"
createGitRepo "$t7Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t7Dir/"

cat >"$t7Dir/flake.nix" <<EOF
{
  description = "Error caching test";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    broken = throw "intentional error";
    working = "ok";
    ifd = assert (import self.drv); self.drv;
    drv = mkDerivation {
      name = "build";
      buildCommand = ''
        echo true > \$out
      '';
    };
  };
}
EOF

git -C "$t7Dir" add .
git -C "$t7Dir" commit -m "Init"

# Cold cache — should produce error
expect 1 nix build "$t7Dir#broken" 2>&1 | grepQuiet 'error: intentional error'

# Error should be cached — same error on warm path
expect 1 nix build "$t7Dir#broken" 2>&1 | grepQuiet 'error: intentional error'

# Working attribute should still work
nix eval "$t7Dir#working" | grepQuiet "ok"
NIX_ALLOW_EVAL=0 nix eval "$t7Dir#working" | grepQuiet "ok"

# Conditional error should not be cached
expect 1 nix build "$t7Dir#ifd" --option allow-import-from-derivation false 2>&1 \
  | grepQuiet 'error: cannot build .* during evaluation because the option '\''allow-import-from-derivation'\'' is disabled'
nix build --no-link "$t7Dir#ifd"

echo "Test 7 passed: error caching"

###############################################################################
# Test 8: Stack overflow should NOT be cached
###############################################################################

t8Dir="$TEST_ROOT/core-stack-overflow"
createGitRepo "$t8Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t8Dir/"

cat >"$t8Dir/flake.nix" <<EOF
{
  description = "Stack overflow test";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    stack-depth =
      let
        f = x: if x == 0 then true else f (x - 1);
      in
        assert (f 100); mkDerivation {
          name = "stack-test";
          buildCommand = ''
            echo true > \$out
          '';
        };
  };
}
EOF

git -C "$t8Dir" add .
git -C "$t8Dir" commit -m "Init"

# Stack overflow with low depth
expect 1 nix build --max-call-depth 50 "$t8Dir#stack-depth" 2>&1 \
  | grepQuiet 'error: stack overflow; max-call-depth exceeded'

# Normal depth should succeed (SO should NOT be cached)
nix build --no-link "$t8Dir#stack-depth"

echo "Test 8 passed: stack overflow not cached"

echo "All eval-cache-core tests passed!"
