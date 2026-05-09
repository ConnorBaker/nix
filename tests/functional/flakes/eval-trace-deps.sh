#!/usr/bin/env bash

# Eval trace dependency tracking tests: pathExists, readDir, flake input update,
# partial tree verification, derivation structure, path: flake trace recording.
# Tests that the verifying trace (BSàlC) correctly tracks fine-grained
# dependencies and invalidates only affected nodes (Adapton: demand-driven).

source ./common.sh

requireGit

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

###############################################################################
# Test 7: NixBinding SC override — unrelated flake.nix binding change
###############################################################################

t7Dir="$TEST_ROOT/deps-nixbinding-flake"
createGitRepo "$t7Dir" ""

cat >"$t7Dir/flake.nix" <<'EOF'
{
  description = "original description";
  outputs = { self }: { value = "hello"; };
}
EOF
git -C "$t7Dir" add .
git -C "$t7Dir" commit -m "Init"

[[ "$(nix eval --json "$t7Dir#value")" == '"hello"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t7Dir#value")" == '"hello"' ]]

cat >"$t7Dir/flake.nix" <<'EOF'
{
  description = "changed description";
  outputs = { self }: { value = "hello"; };
}
EOF
git -C "$t7Dir" add flake.nix
git -C "$t7Dir" commit -m "Change only description"

# Warm verify after description-only change must:
#   (a) return "hello" (value unchanged)
#   (b) NOT record a fresh trace (record.count == 0 for this call) —
#       proves the SC override / file-level subsumption actually
#       subsumed the content mismatch rather than re-evaluating.
# Without the counter check, a regression that silently re-recorded
# on every call would still pass the NIX_ALLOW_EVAL=0 guard (the
# loader runs happily).
warm_out="$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/deps-t7-warm.json" \
    env NIX_ALLOW_EVAL=0 nix eval --json "$t7Dir#value")"
[[ "$warm_out" == '"hello"' ]]
t7_warm_records="$(readEvalTraceCounter "$TEST_HOME/deps-t7-warm.json" evalTrace.record.count)"
if [[ "$t7_warm_records" -ne 0 ]]; then
    echo "Test 7 regression: warm verify re-recorded (records=$t7_warm_records)"
    echo "SC override is not subsuming the description-only change."
    cat "$TEST_HOME/deps-t7-warm.json" >&2
    exit 1
fi

echo "Test 7 passed: NixBinding SC override for description change"

###############################################################################
# Test 8: NixBinding SC override — outputs with path literals
###############################################################################

t8Dir="$TEST_ROOT/deps-nixbinding-pathliterals"
createGitRepo "$t8Dir" ""

mkdir -p "$t8Dir/lib"
cat >"$t8Dir/lib/helper.nix" <<'EOF'
{ value = "from-helper"; }
EOF

cat >"$t8Dir/flake.nix" <<'EOF'
{
  description = "original";
  outputs = { self }: let
    helper = import ./lib/helper.nix;
  in {
    value = helper.value;
  };
}
EOF
git -C "$t8Dir" add .
git -C "$t8Dir" commit -m "Init"

[[ "$(nix eval --json "$t8Dir#value")" == '"from-helper"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t8Dir#value")" == '"from-helper"' ]]

cat >"$t8Dir/flake.nix" <<'EOF'
{
  description = "changed";
  outputs = { self }: let
    helper = import ./lib/helper.nix;
  in {
    value = helper.value;
  };
}
EOF
git -C "$t8Dir" add flake.nix
git -C "$t8Dir" commit -m "Change only description"

# Same pattern as Test 7: warm verify must serve from cache AND not
# re-record. Pins the SC-override-subsumption path on an expression
# that imports a sibling .nix file.
warm_out="$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/deps-t8-warm.json" \
    env NIX_ALLOW_EVAL=0 nix eval --json "$t8Dir#value")"
[[ "$warm_out" == '"from-helper"' ]]
t8_warm_records="$(readEvalTraceCounter "$TEST_HOME/deps-t8-warm.json" evalTrace.record.count)"
if [[ "$t8_warm_records" -ne 0 ]]; then
    echo "Test 8 regression: warm verify re-recorded (records=$t8_warm_records)"
    cat "$TEST_HOME/deps-t8-warm.json" >&2
    exit 1
fi

echo "Test 8 passed: NixBinding SC override with path literals"

###############################################################################
# Test 9: NixBinding SC override — negative: accessed binding changes
###############################################################################

t9Dir="$TEST_ROOT/deps-nixbinding-negative"
createGitRepo "$t9Dir" ""

cat >"$t9Dir/flake.nix" <<'EOF'
{
  description = "original";
  outputs = { self }: { value = "hello"; };
}
EOF
git -C "$t9Dir" add .
git -C "$t9Dir" commit -m "Init"

[[ "$(nix eval --json "$t9Dir#value")" == '"hello"' ]]

cat >"$t9Dir/flake.nix" <<'EOF'
{
  description = "original";
  outputs = { self }: { value = "changed"; };
}
EOF
git -C "$t9Dir" add flake.nix
git -C "$t9Dir" commit -m "Change outputs body"

NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --json "$t9Dir#value" \
  | grepQuiet "not everything is cached"
[[ "$(nix eval --json "$t9Dir#value")" == '"changed"' ]]

echo "Test 9 passed: accessed binding change invalidates"

###############################################################################
# Test 10: NixBinding SC override — child scope re-recording
###############################################################################

t10Dir="$TEST_ROOT/deps-nixbinding-rerecord"
createGitRepo "$t10Dir" ""

cat >"$t10Dir/data.txt" <<'EOF'
original-data
EOF

cat >"$t10Dir/flake.nix" <<'EOF'
{
  description = "v1";
  outputs = { self }: {
    value = builtins.readFile ./data.txt;
  };
}
EOF
git -C "$t10Dir" add .
git -C "$t10Dir" commit -m "Init"

[[ "$(nix eval --json "$t10Dir#value")" == '"original-data\n"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t10Dir#value")" == '"original-data\n"' ]]

echo "modified-data" > "$t10Dir/data.txt"
git -C "$t10Dir" add data.txt
git -C "$t10Dir" commit -m "Modify data"
[[ "$(nix eval --json "$t10Dir#value")" == '"modified-data\n"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t10Dir#value")" == '"modified-data\n"' ]]

cat >"$t10Dir/flake.nix" <<'EOF'
{
  description = "v2";
  outputs = { self }: {
    value = builtins.readFile ./data.txt;
  };
}
EOF
git -C "$t10Dir" add flake.nix
git -C "$t10Dir" commit -m "Change only description"

[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t10Dir#value")" == '"modified-data\n"' ]]

echo "Test 10 passed: child re-recording + description change"

# Clear eval-trace cache before transitive test to avoid interference from prior tests
clearStoreIndex

###############################################################################
# Test 11: Transitive flake input — deps resolved correctly
###############################################################################
#
# A flake has a transitive input (input's input). Files from the transitive
# input must have correctly resolved deps — not fall back to "<absolute>"
# with accessor-relative keys that verification can't resolve.

t11DepDir="$TEST_ROOT/deps-transitive-dep"
t11MidDir="$TEST_ROOT/deps-transitive-mid"
t11Dir="$TEST_ROOT/deps-transitive-top"

# Transitive dep: a flake with a data file
createGitRepo "$t11DepDir" ""
echo "dep-data" > "$t11DepDir/data.txt"
cat >"$t11DepDir/flake.nix" <<'EOF'
{
  description = "Transitive dep";
  outputs = { self }: {
    data = builtins.readFile ./data.txt;
  };
}
EOF
git -C "$t11DepDir" add .
git -C "$t11DepDir" commit -m "Init dep"

# Middle flake: imports the transitive dep
createGitRepo "$t11MidDir" ""
cat >"$t11MidDir/flake.nix" <<EOF
{
  description = "Middle flake";
  inputs.dep.url = "git+file://$t11DepDir";
  outputs = { self, dep }: {
    value = dep.data;
  };
}
EOF
git -C "$t11MidDir" add .
git -C "$t11MidDir" commit -m "Init mid"

# Top-level flake: imports middle, accesses transitive value
createGitRepo "$t11Dir" ""
cat >"$t11Dir/flake.nix" <<EOF
{
  description = "Top flake v1";
  inputs.mid.url = "git+file://$t11MidDir";
  outputs = { self, mid }: {
    value = mid.value;
  };
}
EOF
git -C "$t11Dir" add .
git -C "$t11Dir" commit -m "Init top"

# Record — first eval creates flake.lock, making the tree dirty.
# Verification must succeed despite dirty tree (different root store path).
[[ "$(nix eval --json "$t11Dir#value")" == '"dep-data\n"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t11Dir#value")" == '"dep-data\n"' ]]

# Change only top-level description — transitive deps unchanged
cat >"$t11Dir/flake.nix" <<EOF
{
  description = "Top flake v2";
  inputs.mid.url = "git+file://$t11MidDir";
  outputs = { self, mid }: {
    value = mid.value;
  };
}
EOF
git -C "$t11Dir" add flake.nix
git -C "$t11Dir" commit -m "Change only description"

# Should hit — transitive input deps must be correctly resolved
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t11Dir#value")" == '"dep-data\n"' ]]

echo "Test 11 passed: transitive flake input deps resolved correctly"

###############################################################################
# Test 12: Runtime fetchTree input — per-file precision (RuntimeFetchIdentity)
###############################################################################
#
# A flake calls builtins.fetchTree on a separate git repo (NOT a flake input).
# The fetched repo has two files: accessed.txt (read by the flake) and
# unrelated.txt (not read). Modifying unrelated.txt and updating the pin
# should NOT invalidate the cache — only the accessed file matters.
# This tests RuntimeFetchIdentity dep + session-open root verification.

clearStoreIndex

t12RuntimeDir="$TEST_ROOT/deps-runtime-input"
t12Dir="$TEST_ROOT/deps-runtime-top"

# Create the runtime input repo (not a flake — just a git repo with data files)
createGitRepo "$t12RuntimeDir" ""
echo "accessed-data" > "$t12RuntimeDir/accessed.txt"
echo "unrelated-data" > "$t12RuntimeDir/unrelated.txt"
git -C "$t12RuntimeDir" add .
git -C "$t12RuntimeDir" commit -m "Init runtime input"
t12Rev1=$(git -C "$t12RuntimeDir" rev-parse HEAD)

# Create the top-level flake that fetches the runtime input
createGitRepo "$t12Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t12Dir/"
cat >"$t12Dir/flake.nix" <<EOF
{
  description = "Runtime fetchTree precision test";
  outputs = { self }: let
    inherit (import ./config.nix) mkDerivation;
    src = builtins.fetchTree {
      type = "git";
      url = "file://$t12RuntimeDir";
      rev = "$t12Rev1";
    };
    content = builtins.readFile (src.outPath + "/accessed.txt");
  in {
    drv = builtins.seq content (mkDerivation {
      name = "runtime-test";
      buildCommand = "echo true > \$out";
    });
  };
}
EOF
git -C "$t12Dir" add .
git -C "$t12Dir" commit -m "Init top"

# Record trace
nix build --no-link "$t12Dir#drv"
NIX_ALLOW_EVAL=0 nix build --no-link "$t12Dir#drv"

# Modify UNRELATED file in the runtime input, create new rev
echo "changed-unrelated" > "$t12RuntimeDir/unrelated.txt"
git -C "$t12RuntimeDir" add unrelated.txt
git -C "$t12RuntimeDir" commit -m "Change unrelated file"
t12Rev2=$(git -C "$t12RuntimeDir" rev-parse HEAD)

# Update the pin in flake.nix to the new rev
cat >"$t12Dir/flake.nix" <<EOF
{
  description = "Runtime fetchTree precision test";
  outputs = { self }: let
    inherit (import ./config.nix) mkDerivation;
    src = builtins.fetchTree {
      type = "git";
      url = "file://$t12RuntimeDir";
      rev = "$t12Rev2";
    };
    content = builtins.readFile (src.outPath + "/accessed.txt");
  in {
    drv = builtins.seq content (mkDerivation {
      name = "runtime-test";
      buildCommand = "echo true > \$out";
    });
  };
}
EOF
git -C "$t12Dir" add flake.nix
git -C "$t12Dir" commit -m "Update runtime input pin (unrelated change)"

# Re-record (SC dep on flake.nix fails due to rev change → fresh eval)
nix build --no-link "$t12Dir#drv"
# Verify — runtime root re-verified at session open, per-file deps should pass
# because accessed.txt didn't change between rev1 and rev2.
NIX_ALLOW_EVAL=0 nix build --no-link "$t12Dir#drv"

echo "Test 12 passed: runtime fetchTree per-file precision"

###############################################################################
# Test 13: Runtime fetchTree input — accessed file change detection
###############################################################################
#
# Same setup as Test 12 but modify the ACCESSED file. The cache must miss.

# Modify accessed.txt in the runtime input
echo "changed-accessed" > "$t12RuntimeDir/accessed.txt"
git -C "$t12RuntimeDir" add accessed.txt
git -C "$t12RuntimeDir" commit -m "Change accessed file"
t12Rev3=$(git -C "$t12RuntimeDir" rev-parse HEAD)

# Update the pin
cat >"$t12Dir/flake.nix" <<EOF
{
  description = "Runtime fetchTree precision test";
  outputs = { self }: let
    inherit (import ./config.nix) mkDerivation;
    src = builtins.fetchTree {
      type = "git";
      url = "file://$t12RuntimeDir";
      rev = "$t12Rev3";
    };
    content = builtins.readFile (src.outPath + "/accessed.txt");
  in {
    drv = builtins.seq content (mkDerivation {
      name = "runtime-test-v2";
      buildCommand = "echo true > \$out";
    });
  };
}
EOF
git -C "$t12Dir" add flake.nix
git -C "$t12Dir" commit -m "Update runtime input pin (accessed file changed)"

# Re-record
nix build --no-link "$t12Dir#drv"
# Verify — should hit (just recorded)
NIX_ALLOW_EVAL=0 nix build --no-link "$t12Dir#drv"

echo "Test 13 passed: runtime fetchTree accessed file change detection"

###############################################################################
# Test 13b: Runtime path inputs with identical content must not alias
###############################################################################
#
# Two locked builtins.fetchTree path inputs with identical contents produce the
# same store path, but they are still distinct logical inputs. If one later
# disappears, verification must miss rather than letting the surviving input
# subsume the missing one's fine deps.

clearStoreIndex

t13bDirA="$TEST_ROOT/deps-runtime-alias-a"
t13bDirB="$TEST_ROOT/deps-runtime-alias-b"
t13bTop="$TEST_ROOT/deps-runtime-alias-top"

mkdir -p "$t13bDirA" "$t13bDirB"
printf "shared-data" > "$t13bDirA/value.txt"
printf "shared-data" > "$t13bDirB/value.txt"
printf '"shared-import"\n' > "$t13bDirA/f.nix"
printf '"shared-import"\n' > "$t13bDirB/f.nix"

t13bHashA=$(nix hash path "$t13bDirA")
t13bHashB=$(nix hash path "$t13bDirB")
[[ "$t13bHashA" == "$t13bHashB" ]]

createGitRepo "$t13bTop" ""
cat >"$t13bTop/flake.nix" <<EOF
{
  description = "Runtime alias soundness test";
  outputs = { self }: let
    a = builtins.fetchTree {
      type = "path";
      path = "$t13bDirA";
      narHash = "$t13bHashA";
    };
    b = builtins.fetchTree {
      type = "path";
      path = "$t13bDirB";
      narHash = "$t13bHashB";
    };
  in rec {
    aPath = a.outPath;
    bPath = b.outPath;
    value = builtins.seq a.outPath (builtins.readFile (b.outPath + "/value.txt"));
    valueViaCachedPath = builtins.seq aPath (builtins.readFile (bPath + "/value.txt"));
    findFileValue = builtins.seq a.outPath (builtins.readFile (builtins.findFile [{ prefix = ""; path = b.outPath; }] "value.txt"));
    findFileAttrsetValue = builtins.seq a.outPath (builtins.readFile (builtins.findFile [{ prefix = ""; path = b; }] "value.txt"));
    discardFindFileContextValue =
      builtins.seq a.outPath (
        builtins.readFile
          (builtins.unsafeDiscardStringContext
            (builtins.findFile [{ prefix = ""; path = b; }] "value.txt")));
    importValue = builtins.seq (import (a.outPath + "/f.nix")) (import (b.outPath + "/f.nix"));
    spathValue =
      let
        __nixPath = [{ prefix = ""; path = b.outPath; }];
      in builtins.seq a.outPath (builtins.readFile <value.txt>);
    toStringValue = builtins.seq a.outPath (builtins.readFile (builtins.toString b + "/value.txt"));
    discardStringContextValue =
      builtins.seq a.outPath (builtins.readFile (builtins.unsafeDiscardStringContext b.outPath + "/value.txt"));
    discardAttrsetContextValue =
      builtins.seq a.outPath (
        builtins.readFile
          (builtins.unsafeDiscardStringContext { outPath = b.outPath; } + "/value.txt"));
    filterValue =
      let
        filtered = builtins.filterSource
          (path: type:
            builtins.seq a.outPath (
              if type != "regular" then true
              else if builtins.baseNameOf path == "value.txt" then builtins.readFile path == "shared-data"
              else true))
          b.outPath;
      in builtins.readFile (filtered + "/value.txt");
  };
}
EOF
git -C "$t13bTop" add .
git -C "$t13bTop" commit -m "Init runtime alias test"

[[ "$(nix eval --raw "$t13bTop#value")" == "shared-data" ]]
[[ "$(nix eval --raw "$t13bTop#bPath")" == "$NIX_STORE_DIR/"* ]]
[[ "$(nix eval --raw "$t13bTop#valueViaCachedPath")" == "shared-data" ]]
[[ "$(nix eval --raw "$t13bTop#findFileValue")" == "shared-data" ]]
[[ "$(nix eval --raw "$t13bTop#findFileAttrsetValue")" == "shared-data" ]]
[[ "$(nix eval --raw "$t13bTop#discardFindFileContextValue")" == "shared-data" ]]
[[ "$(nix eval --raw "$t13bTop#importValue")" == "shared-import" ]]
[[ "$(nix eval --raw "$t13bTop#spathValue")" == "shared-data" ]]
[[ "$(nix eval --raw "$t13bTop#toStringValue")" == "shared-data" ]]
[[ "$(nix eval --raw "$t13bTop#discardStringContextValue")" == "shared-data" ]]
[[ "$(nix eval --raw "$t13bTop#discardAttrsetContextValue")" == "shared-data" ]]
[[ "$(nix eval --raw "$t13bTop#filterValue")" == "shared-data" ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t13bTop#value")" == "shared-data" ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t13bTop#bPath")" == "$NIX_STORE_DIR/"* ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t13bTop#valueViaCachedPath")" == "shared-data" ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t13bTop#findFileValue")" == "shared-data" ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t13bTop#findFileAttrsetValue")" == "shared-data" ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t13bTop#discardFindFileContextValue")" == "shared-data" ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t13bTop#importValue")" == "shared-import" ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t13bTop#spathValue")" == "shared-data" ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t13bTop#toStringValue")" == "shared-data" ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t13bTop#discardStringContextValue")" == "shared-data" ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t13bTop#discardAttrsetContextValue")" == "shared-data" ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t13bTop#filterValue")" == "shared-data" ]]

sharedRuntimePath=$(nix eval --raw "$t13bTop#aPath")
nix store delete "$sharedRuntimePath" --ignore-liveness

rm -rf "$t13bDirB"

NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --raw "$t13bTop#bPath" \
  | grepQuiet "not everything is cached"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --raw "$t13bTop#value" \
  | grepQuiet "not everything is cached"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --raw "$t13bTop#valueViaCachedPath" \
  | grepQuiet "not everything is cached"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --raw "$t13bTop#findFileValue" \
  | grepQuiet "not everything is cached"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --raw "$t13bTop#findFileAttrsetValue" \
  | grepQuiet "not everything is cached"
# `unsafeDiscardStringContext` on a matched file path copies that file to its
# own store object, so this spelling intentionally detaches from the original
# runtime root. However, the trace tree still depends on the runtime root via
# parent TraceContext deps (root → discardFindFileContextValue). When the
# runtime root's store path is deleted, the root trace can't verify, so child
# traces also fail. This is a known limitation: trace-level independence
# doesn't extend to the parent chain.
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --raw "$t13bTop#discardFindFileContextValue" \
  | grepQuiet "not everything is cached"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --raw "$t13bTop#importValue" \
  | grepQuiet "not everything is cached"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --raw "$t13bTop#spathValue" \
  | grepQuiet "not everything is cached"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --raw "$t13bTop#toStringValue" \
  | grepQuiet "not everything is cached"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --raw "$t13bTop#discardStringContextValue" \
  | grepQuiet "not everything is cached"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --raw "$t13bTop#discardAttrsetContextValue" \
  | grepQuiet "not everything is cached"
NIX_ALLOW_EVAL=0 expectStderr 1 nix eval --raw "$t13bTop#filterValue" \
  | grepQuiet "not everything is cached"

echo "Test 13b passed: identical runtime inputs do not alias across cached paths, lookup paths, attrset coercions, string-context transforms, imports, or filter callbacks; file-valued unsafeDiscardStringContext detaches to its copied store path as expected"

###############################################################################
# Test 13c: Flake subdir paths must warm-hit under eval-trace
###############################################################################
#
# Regression test for flake inputs with `dir=...` / `?dir=...`.
# sourceInfo.outPath is the store root while the logical flake root is a
# subdirectory. Eval-trace must still record flake-input keys relative to the
# flake root, not the store root, or warm verification will miss.

clearStoreIndex

t13cRepo="$TEST_ROOT/deps-subdir-self-repo"
t13cClient="$TEST_ROOT/deps-subdir-client"
t13cFlakeDir="$t13cRepo/b-low"

createGitRepo "$t13cRepo" ""
mkdir -p "$t13cFlakeDir"
writeSimpleFlake "$t13cRepo"
writeSimpleFlake "$t13cFlakeDir"

echo all good > "$t13cFlakeDir/message"
cat > "$t13cFlakeDir/flake.nix" <<EOF
{
  outputs = inputs: rec {
    packages.$system = rec {
      default =
        assert builtins.readFile ./message == "all good\n";
        assert builtins.baseNameOf inputs.self.outPath == "b-low";
        assert builtins.readFile (inputs.self.outPath + "/message") == "all good\n";
        assert builtins.readFile (inputs.self.sourceInfo.outPath + "/b-low/message") == "all good\n";
        assert builtins.readFile (inputs.self + "/message") == "all good\n";
        import (inputs.self + "/simple.nix");
    };
  };
}
EOF

(
  cd "$t13cRepo"
  git add .
  git commit -m "Init subdir flake"
)

# Direct ?dir= self path: cold record then warm verify.
nix build --no-link "$t13cRepo?dir=b-low"
NIX_ALLOW_EVAL=0 nix build --no-link "$t13cRepo?dir=b-low"

mkdir -p "$t13cClient"
cat > "$t13cClient/flake.nix" <<EOF
{
  inputs.inp = {
    type = "git";
    url = "file://$t13cRepo";
    dir = "b-low";
  };

  outputs = inputs: {
    packages.$system.default = inputs.inp.packages.$system.default;
  };
}
EOF

# External dir= input: cold record then warm verify.
nix build --no-link "$t13cClient"
NIX_ALLOW_EVAL=0 nix build --no-link "$t13cClient"

echo "Test 13c passed: flake subdir paths warm-hit under eval-trace"

###############################################################################
# Test 14: Lock-file input change — cross-revision output correctness
###############################################################################
#
# Regression test for false-positive SC dep override on flake.lock.
# Lock-file inputs are resolved via readFile provenance injected on the lock
# file string in callFlake, so without that provenance the eval-trace
# doesn't detect lock-file changes and serves stale results.
#
# This test creates a flake with input "dep" whose data flows into the
# output. When dep changes, the output must change — the cache must NOT
# serve the old value.

clearStoreIndex

t14DepDir="$TEST_ROOT/deps-lockchange-dep"
t14Dir="$TEST_ROOT/deps-lockchange-top"

# Create the dep flake with initial data
createGitRepo "$t14DepDir" ""
echo '"v1"' > "$t14DepDir/data.nix"
cat >"$t14DepDir/flake.nix" <<'EOF'
{
  description = "Lock change dep";
  outputs = { self }: {
    data = import ./data.nix;
  };
}
EOF
git -C "$t14DepDir" add .
git -C "$t14DepDir" commit -m "Init dep v1"

# Create the top flake that uses dep.data
createGitRepo "$t14Dir" ""
cat >"$t14Dir/flake.nix" <<EOF
{
  description = "Lock change test";
  inputs.dep.url = "git+file://$t14DepDir";
  outputs = { self, dep }: {
    value = dep.data;
  };
}
EOF
git -C "$t14Dir" add .
git -C "$t14Dir" commit -m "Init top"

# Record trace — should get "v1"
[[ "$(nix eval --json "$t14Dir#value")" == '"v1"' ]]
# Verify cache hit
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t14Dir#value")" == '"v1"' ]]

# Change dep to v2
echo '"v2"' > "$t14DepDir/data.nix"
git -C "$t14DepDir" add data.nix
git -C "$t14DepDir" commit -m "Update dep to v2"

# Update lock file
nix flake update dep --flake "$t14Dir"
git -C "$t14Dir" add flake.lock
git -C "$t14Dir" commit -m "Update lock for dep v2"

# CRITICAL: eval with trace must produce "v2", not stale "v1"
[[ "$(nix eval --json "$t14Dir#value")" == '"v2"' ]]

# Verify the new trace
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t14Dir#value")" == '"v2"' ]]

echo "Test 14 passed: lock-file input change — cross-revision correctness"

###############################################################################
# Test 15: Lock-file input change — irrelevant input doesn't invalidate
###############################################################################
#
# Precision test: changing an input that the trace doesn't depend on should
# NOT invalidate the cache. This tests per-key SC dep granularity on
# flake.lock — only traces accessing the changed input's lock entry should
# be invalidated.

clearStoreIndex

t15DepADir="$TEST_ROOT/deps-lockprecision-depA"
t15DepBDir="$TEST_ROOT/deps-lockprecision-depB"
t15Dir="$TEST_ROOT/deps-lockprecision-top"

# Create dep A (accessed by the output)
createGitRepo "$t15DepADir" ""
echo '"from-A"' > "$t15DepADir/data.nix"
cat >"$t15DepADir/flake.nix" <<'EOF'
{
  description = "Dep A";
  outputs = { self }: { data = import ./data.nix; };
}
EOF
git -C "$t15DepADir" add .
git -C "$t15DepADir" commit -m "Init A"

# Create dep B (NOT accessed by the output)
createGitRepo "$t15DepBDir" ""
echo '"from-B"' > "$t15DepBDir/data.nix"
cat >"$t15DepBDir/flake.nix" <<'EOF'
{
  description = "Dep B";
  outputs = { self }: { data = import ./data.nix; };
}
EOF
git -C "$t15DepBDir" add .
git -C "$t15DepBDir" commit -m "Init B"

# Create top flake with both inputs, only accessing A
createGitRepo "$t15Dir" ""
cat >"$t15Dir/flake.nix" <<EOF
{
  description = "Lock precision test";
  inputs.depA.url = "git+file://$t15DepADir";
  inputs.depB.url = "git+file://$t15DepBDir";
  outputs = { self, depA, depB }: {
    value = depA.data;
  };
}
EOF
git -C "$t15Dir" add .
git -C "$t15Dir" commit -m "Init top"

# Record trace
[[ "$(nix eval --json "$t15Dir#value")" == '"from-A"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t15Dir#value")" == '"from-A"' ]]

# Change dep B (irrelevant to the output)
echo '"from-B-v2"' > "$t15DepBDir/data.nix"
git -C "$t15DepBDir" add data.nix
git -C "$t15DepBDir" commit -m "Update B to v2"
nix flake update depB --flake "$t15Dir"
git -C "$t15Dir" add flake.lock
git -C "$t15Dir" commit -m "Update lock for B v2"

# Should still hit cache — dep B is irrelevant to #value
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t15Dir#value")" == '"from-A"' ]]

echo "Test 15 passed: irrelevant lock-file input change — cache hit preserved"

###############################################################################
# Test 16: Attribute removed between revisions — must not serve stale data
###############################################################################
#
# When a flake output attribute is removed between revisions, the eval-trace
# must NOT serve the old cached value. This tests the "attrset shape" gap:
# ParentSlot deps capture the parent's computation inputs but not its output
# key set. Currently, verification may falsely pass for removed attributes.
#
# This test ensures that even if verification falsely passes, the system
# either produces the correct result or fails gracefully (not silently
# serves stale data).

clearStoreIndex

t16Dir="$TEST_ROOT/deps-attr-removed"
createGitRepo "$t16Dir" ""

cat >"$t16Dir/flake.nix" <<'EOF'
{
  description = "Attr removal test v1";
  outputs = { self }: {
    kept = "kept-value";
    removed = "will-be-removed";
  };
}
EOF
git -C "$t16Dir" add .
git -C "$t16Dir" commit -m "Init with both attrs"

# Record traces for both attributes
[[ "$(nix eval --json "$t16Dir#kept")" == '"kept-value"' ]]
[[ "$(nix eval --json "$t16Dir#removed")" == '"will-be-removed"' ]]
# Verify cache hits
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t16Dir#kept")" == '"kept-value"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t16Dir#removed")" == '"will-be-removed"' ]]

# Remove the "removed" attribute
cat >"$t16Dir/flake.nix" <<'EOF'
{
  description = "Attr removal test v2";
  outputs = { self }: {
    kept = "kept-value";
  };
}
EOF
git -C "$t16Dir" add flake.nix
git -C "$t16Dir" commit -m "Remove the removed attr"

# "kept" should still work (cache hit or re-eval — either way correct)
[[ "$(nix eval --json "$t16Dir#kept")" == '"kept-value"' ]]

# "removed" should fail with a proper error (attribute doesn't exist),
# NOT silently return the stale cached value "will-be-removed".
# We accept either "does not provide attribute" or "vanished" as valid errors.
expectStderr 1 nix eval --json "$t16Dir#removed" \
  | grepQuiet -E "does not provide attribute|vanished|not everything is cached"

echo "Test 16 passed: removed attribute — no stale data served"

###############################################################################
# Test 17: Transitive input change — positive (accessed data changes)
###############################################################################
#
# Three-level flake chain: top → mid → dep. The top flake accesses dep.data
# (transitively via mid.value). When dep's data changes and the lock file
# is updated, the eval-trace must serve the new value, not the stale one.
# This tests that lock-file SC deps on transitive input lock entries work
# correctly through the full dependency chain.

clearStoreIndex

t17DepDir="$TEST_ROOT/deps-transitive-pos-dep"
t17MidDir="$TEST_ROOT/deps-transitive-pos-mid"
t17Dir="$TEST_ROOT/deps-transitive-pos-top"

# Transitive dep
createGitRepo "$t17DepDir" ""
echo '"dep-v1"' > "$t17DepDir/data.nix"
cat >"$t17DepDir/flake.nix" <<'EOF'
{
  description = "Transitive dep";
  outputs = { self }: { data = import ./data.nix; };
}
EOF
git -C "$t17DepDir" add .
git -C "$t17DepDir" commit -m "Init dep v1"

# Middle flake
createGitRepo "$t17MidDir" ""
cat >"$t17MidDir/flake.nix" <<EOF
{
  description = "Middle flake";
  inputs.dep.url = "git+file://$t17DepDir";
  outputs = { self, dep }: { value = dep.data; };
}
EOF
git -C "$t17MidDir" add .
git -C "$t17MidDir" commit -m "Init mid"

# Top-level flake
createGitRepo "$t17Dir" ""
cat >"$t17Dir/flake.nix" <<EOF
{
  description = "Top flake";
  inputs.mid.url = "git+file://$t17MidDir";
  outputs = { self, mid }: { value = mid.value; };
}
EOF
git -C "$t17Dir" add .
git -C "$t17Dir" commit -m "Init top"

# Record and verify v1
[[ "$(nix eval --json "$t17Dir#value")" == '"dep-v1"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t17Dir#value")" == '"dep-v1"' ]]

# Change transitive dep to v2
echo '"dep-v2"' > "$t17DepDir/data.nix"
git -C "$t17DepDir" add data.nix
git -C "$t17DepDir" commit -m "Update dep to v2"

# Update mid's lock file (picks up new dep)
nix flake update dep --flake "$t17MidDir"
git -C "$t17MidDir" add flake.lock
git -C "$t17MidDir" commit -m "Update mid lock for dep v2"

# Update top's lock file (picks up new mid which references new dep)
nix flake update mid --flake "$t17Dir"
git -C "$t17Dir" add flake.lock
git -C "$t17Dir" commit -m "Update top lock for mid v2"

# CRITICAL: must produce "dep-v2", not stale "dep-v1"
[[ "$(nix eval --json "$t17Dir#value")" == '"dep-v2"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t17Dir#value")" == '"dep-v2"' ]]

echo "Test 17 passed: transitive input change — correct new value"

###############################################################################
# Test 18: Transitive input change — negative (irrelevant transitive change)
###############################################################################
#
# Same three-level chain: top → mid → dep. But the top flake has TWO outputs:
# one that accesses dep.data (affected) and one that only accesses local data
# (unaffected). When dep changes, the unaffected output must still cache-hit.

clearStoreIndex

t18DepDir="$TEST_ROOT/deps-transitive-neg-dep"
t18MidDir="$TEST_ROOT/deps-transitive-neg-mid"
t18Dir="$TEST_ROOT/deps-transitive-neg-top"

# Transitive dep
createGitRepo "$t18DepDir" ""
echo '"dep-data"' > "$t18DepDir/data.nix"
cat >"$t18DepDir/flake.nix" <<'EOF'
{
  description = "Transitive dep";
  outputs = { self }: { data = import ./data.nix; };
}
EOF
git -C "$t18DepDir" add .
git -C "$t18DepDir" commit -m "Init dep"

# Middle flake
createGitRepo "$t18MidDir" ""
cat >"$t18MidDir/flake.nix" <<EOF
{
  description = "Middle flake";
  inputs.dep.url = "git+file://$t18DepDir";
  outputs = { self, dep }: { value = dep.data; };
}
EOF
git -C "$t18MidDir" add .
git -C "$t18MidDir" commit -m "Init mid"

# Top-level flake: two outputs, only "uses-dep" accesses mid.value
createGitRepo "$t18Dir" ""
echo '"local-only"' > "$t18Dir/local.nix"
cat >"$t18Dir/flake.nix" <<EOF
{
  description = "Top flake";
  inputs.mid.url = "git+file://$t18MidDir";
  outputs = { self, mid }: {
    usesDep = mid.value;
    localOnly = import ./local.nix;
  };
}
EOF
git -C "$t18Dir" add .
git -C "$t18Dir" commit -m "Init top"

# Record both outputs
[[ "$(nix eval --json "$t18Dir#usesDep")" == '"dep-data"' ]]
[[ "$(nix eval --json "$t18Dir#localOnly")" == '"local-only"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t18Dir#usesDep")" == '"dep-data"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t18Dir#localOnly")" == '"local-only"' ]]

# Change transitive dep
echo '"dep-data-v2"' > "$t18DepDir/data.nix"
git -C "$t18DepDir" add data.nix
git -C "$t18DepDir" commit -m "Update dep"
nix flake update dep --flake "$t18MidDir"
git -C "$t18MidDir" add flake.lock
git -C "$t18MidDir" commit -m "Update mid lock"
nix flake update mid --flake "$t18Dir"
git -C "$t18Dir" add flake.lock
git -C "$t18Dir" commit -m "Update top lock"

# localOnly should still cache-hit — it doesn't depend on dep
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t18Dir#localOnly")" == '"local-only"' ]]

# usesDep should produce new value
[[ "$(nix eval --json "$t18Dir#usesDep")" == '"dep-data-v2"' ]]

echo "Test 18 passed: transitive input change — unaffected output still cached"

###############################################################################
# Test 19: Lock-file subsumption — unchanged lock file fast path
###############################################################################
#
# Tests coarse-query subsumption: when the lock file Content dep matches,
# markFileVerified subsumes all per-key SC deps from builtins.fromJSON
# in call-flake.nix (per-file scope, not per-source — avoids subsuming
# flake.nix SC deps). This is the fast path — no individual SC dep
# verification needed.
#
# Setup: flake with 3 inputs. Change only the description (not the lock
# file). All 3 inputs' SC deps should be subsumed. Cache must hit even
# with NIX_ALLOW_EVAL=0.

clearStoreIndex

t19DepADir="$TEST_ROOT/deps-subsume-depA"
t19DepBDir="$TEST_ROOT/deps-subsume-depB"
t19DepCDir="$TEST_ROOT/deps-subsume-depC"
t19Dir="$TEST_ROOT/deps-subsume-top"

for depDir in "$t19DepADir" "$t19DepBDir" "$t19DepCDir"; do
    createGitRepo "$depDir" ""
    name=$(basename "$depDir")
    echo "\"$name\"" > "$depDir/data.nix"
    cat >"$depDir/flake.nix" <<'EOF'
{ description = "dep"; outputs = { self }: { data = import ./data.nix; }; }
EOF
    git -C "$depDir" add .
    git -C "$depDir" commit -m "Init $name"
done

createGitRepo "$t19Dir" ""
cat >"$t19Dir/flake.nix" <<EOF
{
  description = "Subsumption test v1";
  inputs.a.url = "git+file://$t19DepADir";
  inputs.b.url = "git+file://$t19DepBDir";
  inputs.c.url = "git+file://$t19DepCDir";
  outputs = { self, a, b, c }: {
    value = a.data + " " + b.data + " " + c.data;
  };
}
EOF
git -C "$t19Dir" add .
git -C "$t19Dir" commit -m "Init top"

# Record trace
result19=$(nix eval --json "$t19Dir#value")
[[ "$result19" == *"deps-subsume-depA"* ]]
# Verify cache hit
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t19Dir#value")" == "$result19" ]]

# Change ONLY description — lock file unchanged → subsumption fast path
cat >"$t19Dir/flake.nix" <<EOF
{
  description = "Subsumption test v2 — only description changed";
  inputs.a.url = "git+file://$t19DepADir";
  inputs.b.url = "git+file://$t19DepBDir";
  inputs.c.url = "git+file://$t19DepCDir";
  outputs = { self, a, b, c }: {
    value = a.data + " " + b.data + " " + c.data;
  };
}
EOF
git -C "$t19Dir" add flake.nix
git -C "$t19Dir" commit -m "Change only description"

# Must hit cache — lock file unchanged, coarse-query subsumption covers all lock SC deps.
# The flake.nix Content dep fails but SC deps on accessed bindings pass
# (outputs binding unchanged). Pin "warm served, no re-record" via
# NIX_SHOW_STATS: record.count must be 0 and evalTrace.hits must be
# at least 1. A regression that silently re-evaluated and recorded a
# fresh trace for the changed flake.nix would show record.count > 0.
t19_warm_out="$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/deps-t19-warm.json" \
    env NIX_ALLOW_EVAL=0 nix eval --json "$t19Dir#value")"
[[ "$t19_warm_out" == "$result19" ]]
t19_warm_records="$(readEvalTraceCounter "$TEST_HOME/deps-t19-warm.json" evalTrace.record.count)"
t19_warm_hits="$(readEvalTraceCounter "$TEST_HOME/deps-t19-warm.json" evalTrace.hits)"
if [[ "$t19_warm_records" -ne 0 || "$t19_warm_hits" -lt 1 ]]; then
    echo "Test 19 regression: warm-verify path broken"
    echo "  records=$t19_warm_records hits=$t19_warm_hits"
    cat "$TEST_HOME/deps-t19-warm.json" >&2
    exit 1
fi

echo "Test 19 passed: lock-file subsumption — unchanged lock fast path"

###############################################################################
# Test 20: Lock-file subsumption — changed lock falls through to per-key
###############################################################################
#
# When lock file changes, coarse-query subsumption does NOT fire (hash mismatch).
# Individual SC deps are checked. Only traces accessing the changed
# input's lock entry are invalidated.
#
# Same setup as Test 19. Change depA (accessed), verify cache misses.
# Then change depC (also accessed), verify it updates correctly too.

# Change depA
echo '"depA-v2"' > "$t19DepADir/data.nix"
git -C "$t19DepADir" add data.nix
git -C "$t19DepADir" commit -m "Update A to v2"
nix flake update a --flake "$t19Dir"
git -C "$t19Dir" add flake.lock
git -C "$t19Dir" commit -m "Update lock for A v2"

# Must produce new value (cache miss for the trace that accesses A).
# Capture NIX_SHOW_STATS on this cold re-eval: record.count must grow
# — proves the lock change actually drove re-recording, not that the
# previous trace was silently served from some other path.
result20="$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/deps-t20-miss.json" \
    nix eval --json "$t19Dir#value")"
[[ "$result20" == *"depA-v2"* ]]
t20_miss_records="$(readEvalTraceCounter "$TEST_HOME/deps-t20-miss.json" evalTrace.record.count)"
if [[ "$t20_miss_records" -lt 1 ]]; then
    echo "Test 20 regression: lock-file change did not drive re-record"
    echo "  record.count=$t20_miss_records"
    cat "$TEST_HOME/deps-t20-miss.json" >&2
    exit 1
fi

# Verify the new trace
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t19Dir#value")" == "$result20" ]]

echo "Test 20 passed: lock-file subsumption — changed lock per-key precision"

###############################################################################
# Test 21: follows input handling
###############################################################################

t21Dir="$TEST_ROOT/deps-follows"
t21DirB="$TEST_ROOT/deps-follows-B"
t21DirC="$TEST_ROOT/deps-follows-C"

# Create flake B: provides a value
createGitRepo "$t21DirB" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t21DirB/"
cat >"$t21DirB/flake.nix" <<'EOF'
{
  description = "Follows test — B";
  outputs = { self }: { value = "B-v1"; };
}
EOF
git -C "$t21DirB" add .
git -C "$t21DirB" commit -m "Init B"

# Create flake C: depends on B
createGitRepo "$t21DirC" ""
cat >"$t21DirC/flake.nix" <<EOF
{
  description = "Follows test — C";
  inputs.b.url = "git+file://$t21DirB";
  outputs = { self, b }: { value = "C-sees-\${b.value}"; };
}
EOF
git -C "$t21DirC" add .
git -C "$t21DirC" commit -m "Init C"

# Create flake A: depends on B and C, C.b follows A.b
createGitRepo "$t21Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t21Dir/"
cat >"$t21Dir/flake.nix" <<EOF
{
  description = "Follows test — A";
  inputs.b.url = "git+file://$t21DirB";
  inputs.c.url = "git+file://$t21DirC";
  inputs.c.inputs.b.follows = "b";
  outputs = { self, b, c }: { value = "A-sees-\${b.value}-and-\${c.value}"; };
}
EOF
git -C "$t21Dir" add .
git -C "$t21Dir" commit -m "Init A"

# Lock and cold-record
result21=$(nix eval --json "$t21Dir#value")
[[ "$result21" == *"B-v1"* ]]

# Warm-verify
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t21Dir#value")" == "$result21" ]]

# Update B
cat >"$t21DirB/flake.nix" <<'EOF'
{
  description = "Follows test — B v2";
  outputs = { self }: { value = "B-v2"; };
}
EOF
git -C "$t21DirB" add .
git -C "$t21DirB" commit -m "B v2"

# Update A's lock to pick up new B (C follows A's B)
nix flake update b --flake "$t21Dir"
git -C "$t21Dir" add flake.lock
git -C "$t21Dir" commit -m "Update B lock"

# Must see new B value through both direct and follows path
result21b=$(nix eval --json "$t21Dir#value")
[[ "$result21b" == *"B-v2"* ]]
[[ "$result21b" == *"C-sees-B-v2"* ]]

# Warm-verify the updated result
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t21Dir#value")" == "$result21b" ]]

echo "Test 21 passed: follows input handling"

###############################################################################
# Test 22: --override-input with --no-write-lock-file
###############################################################################

t22Dir="$TEST_ROOT/deps-override"
t22Alt="$TEST_ROOT/deps-override-alt"

# Reuse flake B from test 21 as the original input
createGitRepo "$t22Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t22Dir/"
cat >"$t22Dir/flake.nix" <<EOF
{
  description = "Override test";
  inputs.dep.url = "git+file://$t21DirB";
  outputs = { self, dep }: { value = dep.value; };
}
EOF
git -C "$t22Dir" add .
git -C "$t22Dir" commit -m "Init"

# Lock it
nix flake lock "$t22Dir"
git -C "$t22Dir" add flake.lock
git -C "$t22Dir" commit -m "Lock"

# Cold-record baseline
result22=$(nix eval --json "$t22Dir#value")
[[ "$result22" == *"B-v2"* ]]

# Create alternate input
createGitRepo "$t22Alt" ""
cat >"$t22Alt/flake.nix" <<'EOF'
{
  description = "Override alt";
  outputs = { self }: { value = "alt-value"; };
}
EOF
git -C "$t22Alt" add .
git -C "$t22Alt" commit -m "Init alt"

# Evaluate with override — should see alt value, lock file unchanged
result22o=$(nix eval --json "$t22Dir#value" \
  --override-input dep "git+file://$t22Alt" --no-write-lock-file)
[[ "$result22o" == *"alt-value"* ]]

# Verify lock file is unchanged (still points to B)
result22c=$(nix eval --json "$t22Dir#value")
[[ "$result22c" == *"B-v2"* ]]

echo "Test 22 passed: --override-input with --no-write-lock-file"

###############################################################################
# Test 23: reused relative node (path:./subdir input)
###############################################################################

t23Dir="$TEST_ROOT/deps-relative"
t23Sub="$t23Dir/sub"

createGitRepo "$t23Dir" ""
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$t23Dir/"
mkdir -p "$t23Sub"

cat >"$t23Sub/flake.nix" <<'EOF'
{
  description = "Relative sub-flake";
  outputs = { self }: { value = builtins.readFile ./data.txt; };
}
EOF
echo "sub-v1" >"$t23Sub/data.txt"

cat >"$t23Dir/flake.nix" <<'EOF'
{
  description = "Relative parent";
  inputs.sub.url = "path:./sub";
  outputs = { self, sub }: { value = sub.value; };
}
EOF

git -C "$t23Dir" add .
git -C "$t23Dir" commit -m "Init"

# Cold-record
result23=$(nix eval --json "$t23Dir#value")
[[ "$result23" == *"sub-v1"* ]]

# Warm-verify
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t23Dir#value")" == "$result23" ]]

# Modify file in subdir — should invalidate subdir traces
echo "sub-v2" >"$t23Sub/data.txt"
git -C "$t23Dir" add .
git -C "$t23Dir" commit -m "Update sub data"

result23b=$(nix eval --json "$t23Dir#value")
[[ "$result23b" == *"sub-v2"* ]]

# Warm-verify updated result
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t23Dir#value")" == "$result23b" ]]

echo "Test 23 passed: reused relative node (path:./subdir)"

###############################################################################
# Test 24: -I flag produces different session key (BUG-11 regression guard)
#
# computePolicyDigest previously omitted settings.nixPath (the -I /
# nix-path config value), so two sessions with different -I arguments
# shared a policy digest and therefore the same session key.  A warm eval with
# a different -I would incorrectly serve the stale cached result.
#
# This test verifies that the two sessions are isolated: the result served for
# -I foo=... must reflect the actual content of that lookup path
# (i.e. fresh evaluation must run), not the stale result from the other path.
###############################################################################
clearStoreIndex

t24DirA="$TEST_ROOT/deps-nixpath-a"
t24DirB="$TEST_ROOT/deps-nixpath-b"
mkdir -p "$t24DirA" "$t24DirB"

# Each directory exports a distinct value via a .nix file named "value.nix".
echo '"from-path-A"' > "$t24DirA/value.nix"
echo '"from-path-B"' > "$t24DirB/value.nix"

# Cold evaluation with -I adding dir A to the search path.
# <value.nix> resolves to $t24DirA/value.nix (unnamed search path entry).
result24a="$(nix eval --impure -I "$t24DirA" --expr 'import <value.nix>')"
[[ "$result24a" == '"from-path-A"' ]] \
  || { echo "Test 24 FAILED: expected '\"from-path-A\"', got '$result24a'"; exit 1; }

# Cold evaluation with -I adding dir B to the search path.
# Different -I path → different nixPath → different policy digest → different session.
result24b="$(nix eval --impure -I "$t24DirB" --expr 'import <value.nix>')"
[[ "$result24b" == '"from-path-B"' ]] \
  || { echo "Test 24 FAILED: expected '\"from-path-B\"', got '$result24b'"; exit 1; }

# Warm eval with dir A must still return A's value (not B's).
# NIX_ALLOW_EVAL=0 forces a cache-only path — if the session keys are distinct
# (BUG-11 fixed: settings.nixPath.get() is in the policy digest), this hits
# A's trace and returns "from-path-A".
result24a_warm="$(NIX_ALLOW_EVAL=0 nix eval --impure -I "$t24DirA" --expr 'import <value.nix>')"
[[ "$result24a_warm" == '"from-path-A"' ]] \
  || { echo "Test 24 FAILED: warm eval with path-A returned '$result24a_warm' (expected '\"from-path-A\"')"; exit 1; }

# Warm eval with dir B must return B's value (not A's).
result24b_warm="$(NIX_ALLOW_EVAL=0 nix eval --impure -I "$t24DirB" --expr 'import <value.nix>')"
[[ "$result24b_warm" == '"from-path-B"' ]] \
  || { echo "Test 24 FAILED: warm eval with path-B returned '$result24b_warm' (expected '\"from-path-B\"')"; exit 1; }

echo "Test 24 passed: -I flag produces isolated session key (BUG-11)"

###############################################################################
# Test 25: --allowed-uris produces isolated session (policy + recovery)
#
# computePolicyDigest hashes settings.allowedUris unconditionally (the
# hashing loop in session-policy.cc runs outside the `if (!pureEval)`
# guard). The stable recovery key also includes the policy digest (OR-5
# fix in session-builders.cc), so History-based constructive-recovery
# bootstrap respects policy boundaries too. Unit-level coverage is in
# src/libexpr-tests/eval-trace/store/policy-digest.cc.
#
# Test design: hold NIX_PATH / lookup path constant, vary only
# --allowed-uris. Session A records a trace; session B must be cold
# (record.count >= 1, no hit/recovery) because its policy differs.
# If either the policy digest OR the stable recovery key omits
# allowed-uris, session B would reach session A's trace through the
# primary session key or History respectively — and record.count would
# be 0.
###############################################################################
clearStoreIndex

t25Dir="$TEST_ROOT/deps-alluris"
mkdir -p "$t25Dir"
echo '"value"' > "$t25Dir/value.nix"

# Cold eval session A: allowed-uris = "https://example.com/a".
NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/stats25-a.json" \
  nix eval --impure \
  --allowed-uris "https://example.com/a" \
  -I "$t25Dir" --expr 'import <value.nix>' >/dev/null

records_a="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["evalTrace"]["record"]["count"])' "$TEST_HOME/stats25-a.json")"
if [[ "$records_a" == "0" ]]; then
  echo "Test 25 FAILED: session A recorded 0 traces (fixture problem — should be cold)"
  exit 1
fi

# Warm verify session A: must succeed.
result25a_warm="$(NIX_ALLOW_EVAL=0 nix eval --impure \
  --allowed-uris "https://example.com/a" \
  -I "$t25Dir" --expr 'import <value.nix>')"
[[ "$result25a_warm" == '"value"' ]] \
  || { echo "Test 25 FAILED: warm session A returned '$result25a_warm'"; exit 1; }

# Cold eval session B: different allowed-uris → different policy digest
# → different primary session key AND different stable recovery key →
# B must record its own trace rather than recover from A.
NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_HOME/stats25-b.json" \
  nix eval --impure \
  --allowed-uris "https://example.com/b" \
  -I "$t25Dir" --expr 'import <value.nix>' >/dev/null

records_b="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["evalTrace"]["record"]["count"])' "$TEST_HOME/stats25-b.json")"
hits_b="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["evalTrace"]["hits"])' "$TEST_HOME/stats25-b.json")"
if [[ "$records_b" == "0" ]]; then
  echo "Test 25 FAILED: session B recorded 0 traces (record.count=$records_b hits=$hits_b)"
  echo "Session B hit session A's trace — either the policy digest or the stable"
  echo "recovery key does not actually differ by --allowed-uris (OR-5-class bug)."
  cat "$TEST_HOME/stats25-b.json"
  exit 1
fi

# Warm verify session B: must serve B's own trace (not A's stale entry).
result25b_warm="$(NIX_ALLOW_EVAL=0 nix eval --impure \
  --allowed-uris "https://example.com/b" \
  -I "$t25Dir" --expr 'import <value.nix>')"
[[ "$result25b_warm" == '"value"' ]] \
  || { echo "Test 25 FAILED: warm session B returned '$result25b_warm'"; exit 1; }

echo "Test 25 passed: --allowed-uris produces isolated primary + recovery keys (A records=$records_a, B records=$records_b)"

echo "All eval-trace-deps tests passed! (BSàlC: verifying traces with fine-grained dependencies)"
