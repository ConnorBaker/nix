#!/usr/bin/env bash

# Eval trace dependency tracking tests: pathExists, readDir, flake input update,
# partial tree verification, derivation structure, path: flake trace recording.
# Tests that the verifying trace (BSàlC) correctly tracks fine-grained
# dependencies and invalidates only affected nodes (Adapton: demand-driven).

source ./common.sh

requireGit

clearStoreIndex() {
    rm -rf "$TEST_HOME/.cache/nix/eval-trace"* "$TEST_HOME/.cache/nix/attr-vocab.sqlite"* "$TEST_HOME/.cache/nix/stat-hash-cache"*
}

###############################################################################
# Test 1: pathExists dependency in trace (Adapton: existence oracle)
###############################################################################

t1Dir="$TEST_ROOT/deps-pathexists"
createGitRepo "$t1Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t1Dir/"

cat >"$t1Dir/flake.nix" <<EOF
{
  description = "pathExists dep test";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    drv = let
      flag = builtins.pathExists ./optional.nix;
    in builtins.seq flag (mkDerivation {
      name = "pe-test";
      buildCommand = ''
        echo true > \$out
      '';
    });
  };
}
EOF

git -C "$t1Dir" add .
git -C "$t1Dir" commit -m "Init"

# optional.nix doesn't exist — fresh evaluation records existence dependency in trace
nix build --no-link "$t1Dir#drv"
NIX_ALLOW_EVAL=0 nix build --no-link "$t1Dir#drv"

# Create optional.nix — trace verification fails (existence dependency changed false->true)
cat >"$t1Dir/optional.nix" <<'EOF'
42
EOF
git -C "$t1Dir" add optional.nix
git -C "$t1Dir" commit -m "Add optional.nix"

NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link "$t1Dir#drv" \
  | grepQuiet "not everything is cached"

nix build --no-link "$t1Dir#drv"

echo "Test 1 passed: pathExists dependency in trace"

###############################################################################
# Test 2: readDir dependency in trace (Adapton: directory listing oracle)
###############################################################################

t2Dir="$TEST_ROOT/deps-readdir"
createGitRepo "$t2Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t2Dir/"

mkdir -p "$t2Dir/modules"
echo '"foo module"' > "$t2Dir/modules/foo.nix"

cat >"$t2Dir/flake.nix" <<EOF
{
  description = "readDir dep test";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    drv = let
      entries = builtins.readDir ./modules;
    in builtins.seq entries (mkDerivation {
      name = "rd-test";
      buildCommand = ''
        echo true > \$out
      '';
    });
  };
}
EOF

git -C "$t2Dir" add .
git -C "$t2Dir" commit -m "Init"

# Fresh evaluation records directory listing dependency in trace
nix build --no-link "$t2Dir#drv"
# Verification hit — traced result valid
NIX_ALLOW_EVAL=0 nix build --no-link "$t2Dir#drv"

# Add another module — trace verification fails (directory listing changed)
echo '"bar module"' > "$t2Dir/modules/bar.nix"
git -C "$t2Dir" add modules/bar.nix
git -C "$t2Dir" commit -m "Add bar module"

NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link "$t2Dir#drv" \
  | grepQuiet "not everything is cached"

nix build --no-link "$t2Dir#drv"

echo "Test 2 passed: readDir dependency in trace"

###############################################################################
# Test 3: Flake input update — trace unaffected by irrelevant input changes
###############################################################################

t3DepDir="$TEST_ROOT/deps-input-dep"
t3Dir="$TEST_ROOT/deps-input-update"

createGitRepo "$t3DepDir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t3DepDir/"
cat >"$t3DepDir/dep-data.txt" <<'EOF'
original-dep-data
EOF
cat >"$t3DepDir/flake.nix" <<EOF
{
  description = "A dependency flake";
  outputs = { self }: {
    data = builtins.readFile ./dep-data.txt;
  };
}
EOF
git -C "$t3DepDir" add .
git -C "$t3DepDir" commit -m "Init dep"

createGitRepo "$t3Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t3Dir/"
cat >"$t3Dir/flake.nix" <<EOF
{
  description = "Input update test";
  inputs.dep.url = "git+file://$t3DepDir";
  outputs = { self, dep }: let inherit (import ./config.nix) mkDerivation; in {
    drv = builtins.seq (builtins.readFile ./local-data.txt) (mkDerivation {
      name = "input-test";
      buildCommand = ''
        echo true > \$out
      '';
    });
  };
}
EOF
echo "local-data" > "$t3Dir/local-data.txt"
git -C "$t3Dir" add .
git -C "$t3Dir" commit -m "Init"

# Record trace for drv (BSàlC: trace recording)
nix build --no-link "$t3Dir#drv"
NIX_ALLOW_EVAL=0 nix build --no-link "$t3Dir#drv"

# Update input (change dep-data.txt) but drv's trace does not depend on it
echo "changed-dep-data" > "$t3DepDir/dep-data.txt"
git -C "$t3DepDir" add dep-data.txt
git -C "$t3DepDir" commit -m "Change dep data"
nix flake update dep --flake "$t3Dir"
git -C "$t3Dir" add flake.lock
git -C "$t3Dir" commit -m "Update lock"

# drv: verification hit — trace dependencies (local-data.txt only) unchanged (Shake: early cutoff)
NIX_ALLOW_EVAL=0 nix build --no-link "$t3Dir#drv"

echo "Test 3 passed: flake input update — verification hit (irrelevant change)"

###############################################################################
# Test 4: Partial trace invalidation — independent dependencies (Adapton: selective dirtying)
###############################################################################

t4Dir="$TEST_ROOT/deps-partial"
createGitRepo "$t4Dir" ""

cat >"$t4Dir/a-data.txt" <<'EOF'
alpha
EOF

cat >"$t4Dir/b-data.txt" <<'EOF'
beta
EOF

cat >"$t4Dir/flake.nix" <<EOF
{
  description = "Partial tree invalidation test";
  outputs = { self }: {
    a = builtins.readFile ./a-data.txt;
    b = builtins.readFile ./b-data.txt;
  };
}
EOF

git -C "$t4Dir" add .
git -C "$t4Dir" commit -m "Init"

# Fresh evaluation — records traces for both attributes (BSàlC: trace recording)
[[ "$(nix eval --json "$t4Dir#a")" == '"alpha\n"' ]]
[[ "$(nix eval --json "$t4Dir#b")" == '"beta\n"' ]]

# Verification hits — both traced results valid (BSàlC: verifying trace succeeds)
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t4Dir#a")" == '"alpha\n"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t4Dir#b")" == '"beta\n"' ]]

# Modify only a-data.txt — selectively dirties a's trace
echo "gamma" > "$t4Dir/a-data.txt"
git -C "$t4Dir" add a-data.txt
git -C "$t4Dir" commit -m "Modify a-data.txt"

# a: verify miss then fresh evaluation — records new trace with updated value
[[ "$(nix eval --json "$t4Dir#a")" == '"gamma\n"' ]]

# b: verification hit — its trace dependencies (b-data.txt only) unchanged
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t4Dir#b")" == '"beta\n"' ]]

# After re-evaluation, both traces verified successfully
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t4Dir#a")" == '"gamma\n"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t4Dir#b")" == '"beta\n"' ]]

echo "Test 4 passed: partial trace invalidation"

###############################################################################
# Test 5: Traced result correctness via nix eval (functional round-trip)
###############################################################################

t5Dir="$TEST_ROOT/deps-show-drv"
createGitRepo "$t5Dir" ""

echo "test-content" > "$t5Dir/dep-file.txt"

cat >"$t5Dir/flake.nix" <<EOF
{
  description = "show-derivation test";
  outputs = { self }: {
    result = builtins.readFile ./dep-file.txt;
  };
}
EOF

git -C "$t5Dir" add .
git -C "$t5Dir" commit -m "Init"

# Record trace (BSàlC: trace recording)
nix eval "$t5Dir#result" | grepQuiet "test-content"

# Verification hit — traced result matches original (BSàlC: verifying trace succeeds)
NIX_ALLOW_EVAL=0 nix eval "$t5Dir#result" | grepQuiet "test-content"

echo "Test 5 passed: traced result correctness (functional round-trip)"

###############################################################################
# Test 6: path: flake trace recording and verification
###############################################################################

t6Dir="$TEST_ROOT/deps-path-flake"
mkdir -p "$t6Dir"
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t6Dir/"

cat >"$t6Dir/flake.nix" <<EOF
{
  description = "Path flake cache test";
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    drv = builtins.seq (builtins.readFile ./path-data.txt) (mkDerivation {
      name = "path-test";
      buildCommand = ''
        echo true > \$out
      '';
    });
  };
}
EOF
echo "path-data" > "$t6Dir/path-data.txt"

# Fresh evaluation records trace (path: flakes get stable identity for trace keying)
nix build --no-link "path:$t6Dir#drv"

# Verification hit — traced result valid (BSàlC: verifying trace succeeds)
NIX_ALLOW_EVAL=0 nix build --no-link "path:$t6Dir#drv"

# Modify the data file — trace verification fails
echo "changed-path-data" > "$t6Dir/path-data.txt"
NIX_ALLOW_EVAL=0 expectStderr 1 nix build --no-link "path:$t6Dir#drv" \
  | grepQuiet "not everything is cached"

# Re-record trace and verify (BSàlC: trace recording then verification)
nix build --no-link "path:$t6Dir#drv"
NIX_ALLOW_EVAL=0 nix build --no-link "path:$t6Dir#drv"

echo "Test 6 passed: path: flake trace recording and verification"

echo "All eval-trace-deps tests passed! (BSàlC: verifying traces with fine-grained dependencies)"
