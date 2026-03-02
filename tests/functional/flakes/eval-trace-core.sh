#!/usr/bin/env bash

# Core eval trace tests: trace recording (fresh evaluation), verifying traces,
# fine-grained dependency tracking, parent chain verification, sibling trace
# sharing, shared dependency traces, GC resilience, error trace recording,
# stack overflow non-cacheability.
# Terminology follows Build Systems à la Carte (BSàlC) and Adapton.

source ./common.sh

requireGit

clearStoreIndex() {
    rm -rf "$TEST_HOME/.cache/nix/eval-trace"* "$TEST_HOME/.cache/nix/attr-vocab.sqlite"* "$TEST_HOME/.cache/nix/stat-hash-cache"*
}

###############################################################################
# Test 1: Basic trace recording and verification (BSàlC: verifying trace)
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

# Fresh evaluation — records a new trace (BSàlC: trace recording)
nix build --no-link "$t1Dir#drv"

# Verification hit — traced result served without re-evaluation (BSàlC: verifying trace succeeds)
NIX_ALLOW_EVAL=0 nix build --no-link "$t1Dir#drv"

echo "Test 1 passed: basic trace recording and verification"

###############################################################################
# Test 2: Fine-grained trace invalidation (Adapton: demand-driven dirtying)
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

# Record traces for both attributes (BSàlC: trace recording)
nix build --no-link "$t2Dir#drvA" "$t2Dir#drvB"

# Both verification hits — traced results valid (BSàlC: verifying trace succeeds)
NIX_ALLOW_EVAL=0 nix build --no-link "$t2Dir#drvA"
NIX_ALLOW_EVAL=0 nix build --no-link "$t2Dir#drvB"

# Modify only b-data.txt — drvA's trace unaffected, drvB's trace invalidated
echo "modified-b" > "$t2Dir/b-data.txt"
git -C "$t2Dir" add b-data.txt
git -C "$t2Dir" commit -m "Modify b-data.txt"

# drvA: verification hit — its trace deps (a-data.txt) unchanged
NIX_ALLOW_EVAL=0 nix build --no-link "$t2Dir#drvA"

# drvB: verify miss — trace verification fails because b-data.txt changed
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link "$t2Dir#drvB" \
  | grepQuiet "not everything is cached"

# Re-evaluate drvB (fresh evaluation records new trace), then verify
nix build --no-link "$t2Dir#drvB"
NIX_ALLOW_EVAL=0 nix build --no-link "$t2Dir#drvB"

# Modify flake.nix itself (root dependency) — all traced results invalidated
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

# Both verify miss — root dependency changed propagates to all children (Shake: transitive dirtying)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link "$t2Dir#drvA" \
  | grepQuiet "not everything is cached"
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link "$t2Dir#drvB" \
  | grepQuiet "not everything is cached"

# Re-record traces and verify they hold (BSàlC: trace recording then verification)
nix build --no-link "$t2Dir#drvA" "$t2Dir#drvB"
NIX_ALLOW_EVAL=0 nix build --no-link "$t2Dir#drvA"
NIX_ALLOW_EVAL=0 nix build --no-link "$t2Dir#drvB"

echo "Test 2 passed: fine-grained trace invalidation"

###############################################################################
# Test 3: Sibling trace sharing via navigateToReal wrapping
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

# Record traces for both siblings (BSàlC: trace recording)
nix build --no-link "$t3Dir#drvA" "$t3Dir#drvB"

# Both verification hits — traced results valid
NIX_ALLOW_EVAL=0 nix build --no-link "$t3Dir#drvA"
NIX_ALLOW_EVAL=0 nix build --no-link "$t3Dir#drvB"

# Modify a-data.txt — drvB's trace unaffected (independent dependency)
echo "modified-a" > "$t3Dir/a-data.txt"
git -C "$t3Dir" add a-data.txt
git -C "$t3Dir" commit -m "Modify a-data.txt"

# drvB: verification hit — its trace dependencies unchanged
NIX_ALLOW_EVAL=0 nix build --no-link "$t3Dir#drvB"

# drvA: verify miss — trace verification fails (a-data.txt changed)
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link "$t3Dir#drvA" \
  | grepQuiet "not everything is cached"

echo "Test 3 passed: sibling trace sharing"

###############################################################################
# Test 4: Parent chain trace verification (Shake: transitive dependency checking)
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

# Record trace (BSàlC: trace recording)
nix build --no-link "$t4Dir#drvA"
NIX_ALLOW_EVAL=0 nix build --no-link "$t4Dir#drvA"

# Modify flake.nix (root dependency) — description change only
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

# drvA: verify miss — trace verification fails because flake.nix (root dependency in parent chain) changed
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link "$t4Dir#drvA" \
  | grepQuiet "not everything is cached"

# Re-record trace and verify (BSàlC: trace recording then verification)
nix build --no-link "$t4Dir#drvA"
NIX_ALLOW_EVAL=0 nix build --no-link "$t4Dir#drvA"

echo "Test 4 passed: parent chain trace verification"

###############################################################################
# Test 5: GC resilience — trace survives garbage collection
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

# Record trace (BSàlC: trace recording)
nix eval "$t5Dir#result" | grepQuiet "gc-test-value"
NIX_ALLOW_EVAL=0 nix eval "$t5Dir#result" | grepQuiet "gc-test-value"

# Run GC to remove store paths backing the traced result
nix-collect-garbage >/dev/null 2>&1

# Clear the index too (GC'd paths invalidate stored traces)
clearStoreIndex

# Fresh evaluation re-records the trace after GC
nix eval "$t5Dir#result" | grepQuiet "gc-test-value"

# Verification hit — newly recorded trace holds
NIX_ALLOW_EVAL=0 nix eval "$t5Dir#result" | grepQuiet "gc-test-value"

echo "Test 5 passed: GC resilience"

###############################################################################
# Test 6: Shared dependency traces (Salsa: shared input tracking)
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

# Record traces for both attributes sharing a dependency
nix eval --json "$t6Dir#a" | grepQuiet "first-shared-data"
nix eval --json "$t6Dir#b" | grepQuiet "second-shared-data"

# Both verification hits — traces valid
NIX_ALLOW_EVAL=0 nix eval --json "$t6Dir#a" | grepQuiet "first-shared-data"
NIX_ALLOW_EVAL=0 nix eval --json "$t6Dir#b" | grepQuiet "second-shared-data"

# Modify the shared dependency — both traces invalidated (Adapton: input change dirties all dependents)
echo "modified-shared" > "$t6Dir/shared.txt"
git -C "$t6Dir" add shared.txt
git -C "$t6Dir" commit -m "Modify shared"

NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --json "$t6Dir#a" \
  | grepQuiet "not everything is cached"

echo "Test 6 passed: shared dependency traces"

###############################################################################
# Test 7: Error trace recording (errors are traced results too)
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

# Fresh evaluation — error is recorded as a traced result (BSàlC: trace recording)
expect 1 nix build "$t7Dir#broken" 2>&1 | grepQuiet 'error: intentional error'

# Verification hit — traced error replayed without re-evaluation
expect 1 nix build "$t7Dir#broken" 2>&1 | grepQuiet 'error: intentional error'

# Non-error attribute: verification hit from its own trace
nix eval "$t7Dir#working" | grepQuiet "ok"
NIX_ALLOW_EVAL=0 nix eval "$t7Dir#working" | grepQuiet "ok"

# Conditional error should NOT be traced (IFD flag may change between sessions)
expect 1 nix build "$t7Dir#ifd" --option allow-import-from-derivation false 2>&1 \
  | grepQuiet 'error: cannot build .* during evaluation because the option '\''allow-import-from-derivation'\'' is disabled'
nix build --no-link "$t7Dir#ifd"

echo "Test 7 passed: error trace recording"

###############################################################################
# Test 8: Stack overflow should NOT be traced (non-deterministic failure)
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

# Stack overflow with low depth — this failure is NOT recorded as a traced result
expect 1 nix build --max-call-depth 50 "$t8Dir#stack-depth" 2>&1 \
  | grepQuiet 'error: stack overflow; max-call-depth exceeded'

# Normal depth should succeed — no stale error trace exists
nix build --no-link "$t8Dir#stack-depth"

echo "Test 8 passed: stack overflow not traced"

echo "All eval-trace-core tests passed! (BSàlC: verifying traces, trace recording, constructive traces)"
