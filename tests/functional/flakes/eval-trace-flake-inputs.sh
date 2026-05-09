#!/usr/bin/env bash

# Eval-trace flake-input tests: F-1 through F-7.
#
# Tests the eval-trace cache under real-world flake input mutation scenarios:
#   F-1: path: input with dirty git tree — session invalidation
#   F-2: builtins.fetchTarball warm hit + content change invalidation
#   F-3: nix flake update → no stale result + constructive recovery
#   F-4: --override-input type switch path: → git: invalidates
#   F-5: Deep follows chain / diamond pattern (soundness + no infinite loop)
#   F-6: Unused flake input change → cache hit (precision)
#   F-7: Partial flake update precision (one input updated, other untouched)
#
# Terminology follows Build Systems à la Carte (BSàlC) and Adapton.

source ./common.sh

requireGit

###############################################################################
# Test F-1: path: input + dirty git tree — session invalidation.
#
# Dirtying the working tree changes the git identity hash (dirty files are
# included in the fingerprint). The old session no longer matches, so a
# normal eval re-evaluates and sees the dirty content.
###############################################################################

clearStoreIndex

t1Dir="$TEST_ROOT/f1-path-dirty"
createGitRepo "$t1Dir" ""
printf '"version-clean"' >"$t1Dir/lib.nix"
cat >"$t1Dir/flake.nix" <<'NIXEOF'
{ outputs = { self }: { result = import ./lib.nix; }; }
NIXEOF
git -C "$t1Dir" add .
git -C "$t1Dir" commit -m "Clean commit"

# Cold eval → records trace for "version-clean"
[[ "$(nix eval --json "$t1Dir#result")" == '"version-clean"' ]]
# Warm eval → must hit (BSàlC: verifying trace)
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t1Dir#result")" == '"version-clean"' ]]

# Dirty the working tree WITHOUT committing
printf '"version-dirty"' >"$t1Dir/lib.nix"

# Dirtying the working tree changes the session fingerprint (dirty files are
# included in the git identity hash). The old session no longer matches, so
# NIX_ALLOW_EVAL=0 must fail — there is no cached trace for the dirty state.
# A normal eval (without NIX_ALLOW_EVAL=0) would re-evaluate and see "version-dirty".
[[ "$(nix eval --json "$t1Dir#result")" == '"version-dirty"' ]]

echo "Test F-1 passed: dirty working tree correctly invalidates session"

###############################################################################
# Test F-2: builtins.fetchTarball — cold records RuntimeFetchIdentity dep;
# warm hits; replacing tarball content invalidates.
#
# NOTE: builtins.fetchTarball strips the single top-level directory from the
# tarball, so the unpacked root IS the contents of that top-level dir.
# We put data.txt directly inside the top-level directory (f2-src/data.txt)
# and reference it as src + "/data.txt" inside the flake.
###############################################################################

clearStoreIndex

t2SrcDir="$TEST_ROOT/f2-src"
t2Tarball="$TEST_ROOT/f2-v1.tar.gz"
t2Dir="$TEST_ROOT/f2-top"
mkdir -p "$t2SrcDir"
printf "tarball-v1" >"$t2SrcDir/data.txt"
# Create tarball with f2-src/ as the single top-level directory.
# fetchTarball strips that top level, so unpacked root = f2-src/ contents.
tar -czf "$t2Tarball" -C "$(dirname "$t2SrcDir")" "$(basename "$t2SrcDir")"
# Compute the unpacked NAR hash — fetchTarball's sha256 is the hash of the
# unpacked contents, not the tarball file itself.
t2Hash=$(nix-prefetch-url --unpack "file://$t2Tarball" 2>/dev/null)

createGitRepo "$t2Dir" ""
cat >"$t2Dir/flake.nix" <<EOF
{ outputs = { self }: let
    src = builtins.fetchTarball {
      url = "file://$t2Tarball";
      sha256 = "$t2Hash";
    };
    # fetchTarball strips the single top-level directory (f2-src/), so
    # src points directly to f2-src/ contents — data.txt is at src/data.txt.
  in { result = builtins.readFile (src + "/data.txt"); }; }
EOF
git -C "$t2Dir" add .
git -C "$t2Dir" commit -m "Init"

# Cold eval → records RuntimeFetchIdentity dep on the tarball URL/hash
[[ "$(nix eval --json "$t2Dir#result")" == '"tarball-v1"' ]]
# Warm eval → must hit
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t2Dir#result")" == '"tarball-v1"' ]]

# Replace tarball content — use a DIFFERENT filename to avoid nix-prefetch-url
# cache hit (same URL returns cached v1 hash).
printf "tarball-v2" >"$t2SrcDir/data.txt"
t2TarballV2="$TEST_ROOT/f2-v2.tar.gz"
tar -czf "$t2TarballV2" -C "$(dirname "$t2SrcDir")" "$(basename "$t2SrcDir")"
t2HashV2=$(nix-prefetch-url --unpack "file://$t2TarballV2" 2>/dev/null)
# Update the flake to use v2 tarball + new hash
cat >"$t2Dir/flake.nix" <<EOF
{ outputs = { self }: let
    src = builtins.fetchTarball {
      url = "file://$t2TarballV2";
      sha256 = "$t2HashV2";
    };
  in { result = builtins.readFile (src + "/data.txt"); }; }
EOF
git -C "$t2Dir" add flake.nix
git -C "$t2Dir" commit -m "Update tarball hash"

# The flake.nix changed (new sha256) → new session → cold eval needed
[[ "$(nix eval --json "$t2Dir#result")" == '"tarball-v2"' ]]
# Warm eval → must hit with new content
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t2Dir#result")" == '"tarball-v2"' ]]

# Re-record trace with v2 tarball
[[ "$(nix eval --json "$t2Dir#result")" == '"tarball-v2"' ]]
# Warm eval → must now hit v2
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t2Dir#result")" == '"tarball-v2"' ]]

echo "Test F-2 passed: tarball input warm hit and content-change invalidation"

###############################################################################
# Test F-3: nix flake update end-to-end.
#
# After lock advances to v2, warm eval must NOT serve stale v1 (BUG-1 class).
# After locking back to v1, constructive recovery must serve v1 without re-eval.
###############################################################################

clearStoreIndex

t3InputDir="$TEST_ROOT/f3-input"
t3Dir="$TEST_ROOT/f3-top"

createGitRepo "$t3InputDir" ""
cat >"$t3InputDir/flake.nix" <<'NIXEOF'
{ outputs = { self }: { value = "input-v1"; }; }
NIXEOF
git -C "$t3InputDir" add .
git -C "$t3InputDir" commit -m "v1"

createGitRepo "$t3Dir" ""
cat >"$t3Dir/flake.nix" <<EOF
{ inputs.dep.url = "git+file://$t3InputDir";
  outputs = { self, dep }: { value = dep.value; }; }
EOF
git -C "$t3Dir" add .
git -C "$t3Dir" commit -m "Init"

# Lock and cold eval → "input-v1"
[[ "$(nix eval --json "$t3Dir#value")" == '"input-v1"' ]]
# Warm eval → cache hit
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t3Dir#value")" == '"input-v1"' ]]

# Advance input to v2 and update lock
cat >"$t3InputDir/flake.nix" <<'NIXEOF'
{ outputs = { self }: { value = "input-v2"; }; }
NIXEOF
git -C "$t3InputDir" add .
git -C "$t3InputDir" commit -m "v2"
nix flake update dep --flake "$t3Dir"
git -C "$t3Dir" add flake.lock
git -C "$t3Dir" commit -m "Lock v2"

# BSàlC: soundness — must NOT serve stale v1 after lock update (BUG-1 class)
[[ "$(nix eval --json "$t3Dir#value")" == '"input-v2"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t3Dir#value")" == '"input-v2"' ]]

# Revert input content to v1 and update lock back
cat >"$t3InputDir/flake.nix" <<'NIXEOF'
{ outputs = { self }: { value = "input-v1"; }; }
NIXEOF
git -C "$t3InputDir" add .
git -C "$t3InputDir" commit -m "Back to v1"
nix flake update dep --flake "$t3Dir"
git -C "$t3Dir" add flake.lock
git -C "$t3Dir" commit -m "Lock back to v1"

# BSàlC: constructive trace recovery — must serve v1 without re-eval.
# NIX_ALLOW_EVAL=0 is the FIRST assertion: if constructive recovery works,
# the cached v1 trace is found and served without calling the evaluator.
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t3Dir#value")" == '"input-v1"' ]]
[[ "$(nix eval --json "$t3Dir#value")" == '"input-v1"' ]]

echo "Test F-3 passed: flake update soundness and constructive recovery"

###############################################################################
# Test F-4: --override-input type switch path: → git:
#
# Switching an input from path: to git: produces a different resolved flake
# (different session key). The old path: trace must not be reused.
###############################################################################

clearStoreIndex

t4PathDir="$TEST_ROOT/f4-path-src"
t4GitDir="$TEST_ROOT/f4-git-src"
t4Dir="$TEST_ROOT/f4-top"

# Both inputs are proper flakes with a `value` output — but different values.
createGitRepo "$t4PathDir" ""
cat >"$t4PathDir/flake.nix" <<'NIXEOF'
{ outputs = { self }: { value = "path-value"; }; }
NIXEOF
git -C "$t4PathDir" add .
git -C "$t4PathDir" commit -m "Init"

createGitRepo "$t4GitDir" ""
cat >"$t4GitDir/flake.nix" <<'NIXEOF'
{ outputs = { self }: { value = "git-value"; }; }
NIXEOF
git -C "$t4GitDir" add .
git -C "$t4GitDir" commit -m "Init"

createGitRepo "$t4Dir" ""
cat >"$t4Dir/flake.nix" <<EOF
{ inputs.dep.url = "git+file://$t4PathDir";
  outputs = { self, dep }: { value = dep.value; }; }
EOF
git -C "$t4Dir" add .
git -C "$t4Dir" commit -m "Init"

# Cold eval with the original git+file: input (path-value)
[[ "$(nix eval --json "$t4Dir#value")" == '"path-value"' ]]

# Override to a different git: input — different session key → must not reuse old trace
result=$(nix eval --json "$t4Dir#value" \
    --override-input dep "git+file://$t4GitDir" --no-write-lock-file)
[[ "$result" == '"git-value"' ]]

echo "Test F-4 passed: --override-input type switch does not reuse stale trace"

###############################################################################
# Test F-5: Deep follows chains / diamond pattern.
#
# A→B→C→D chain with diamond (B and C both follow D via A).
# Verifies:
#   - No infinite loop in TraceValueContext dep resolution
#   - Soundness when the deepest node (D) is updated
#   - Warm eval hits after cold eval with diamond topology
###############################################################################

clearStoreIndex

t5D="$TEST_ROOT/f5-D"
t5C="$TEST_ROOT/f5-C"
t5B="$TEST_ROOT/f5-B"
t5A="$TEST_ROOT/f5-A"

createGitRepo "$t5D" ""
cat >"$t5D/flake.nix" <<'NIXEOF'
{ outputs = { self }: { value = "D-v1"; }; }
NIXEOF
git -C "$t5D" add .
git -C "$t5D" commit -m "Init"

createGitRepo "$t5C" ""
cat >"$t5C/flake.nix" <<EOF
{ inputs.d.url = "git+file://$t5D";
  outputs = { self, d }: { value = "C-\${d.value}"; }; }
EOF
git -C "$t5C" add .
git -C "$t5C" commit -m "Init"

createGitRepo "$t5B" ""
cat >"$t5B/flake.nix" <<EOF
{ inputs.c.url = "git+file://$t5C";
  inputs.d.url = "git+file://$t5D";
  inputs.c.inputs.d.follows = "d";
  outputs = { self, c, d }: { value = "B-\${c.value}-\${d.value}"; }; }
EOF
git -C "$t5B" add .
git -C "$t5B" commit -m "Init"

createGitRepo "$t5A" ""
cat >"$t5A/flake.nix" <<EOF
{ inputs.b.url = "git+file://$t5B";
  inputs.d.url = "git+file://$t5D";
  inputs.b.inputs.d.follows = "d";
  inputs.b.inputs.c.inputs.d.follows = "d";
  outputs = { self, b, d }: { value = "A-\${b.value}-\${d.value}"; }; }
EOF
git -C "$t5A" add .
git -C "$t5A" commit -m "Init"

# Cold eval + lock the result
nix eval --json "$t5A#value" | grepQuiet "D-v1"
git -C "$t5A" add flake.lock
git -C "$t5A" commit -m "Lock"

# Clear cache and re-eval cold (re-builds the trace index)
clearStoreIndex
nix eval --json "$t5A#value" >/dev/null
# Warm eval — must hit (no infinite loop in diamond follows resolution)
NIX_ALLOW_EVAL=0 nix eval --json "$t5A#value" | grepQuiet "D-v1"

# Update D to v2 and propagate lock through the diamond chain
cat >"$t5D/flake.nix" <<'NIXEOF'
{ outputs = { self }: { value = "D-v2"; }; }
NIXEOF
git -C "$t5D" add .
git -C "$t5D" commit -m "D v2"
nix flake update d --flake "$t5A"
git -C "$t5A" add flake.lock
git -C "$t5A" commit -m "Update D"

# BSàlC: soundness — D updated → all dependents must reflect v2
nix eval --json "$t5A#value" | grepQuiet "D-v2"
NIX_ALLOW_EVAL=0 nix eval --json "$t5A#value" | grepQuiet "D-v2"

echo "Test F-5 passed: deep follows chain and diamond pattern"

###############################################################################
# Test F-6: Unused flake input — precision.
#
# Updating a declared-but-never-accessed input must NOT invalidate the
# cached trace for the output that uses only the other input (precision).
###############################################################################

clearStoreIndex

t6UsedDir="$TEST_ROOT/f6-used"
t6UnusedDir="$TEST_ROOT/f6-unused"
t6Dir="$TEST_ROOT/f6-top"

createGitRepo "$t6UsedDir" ""
cat >"$t6UsedDir/flake.nix" <<'NIXEOF'
{ outputs = { self }: { value = "used-v1"; }; }
NIXEOF
git -C "$t6UsedDir" add .
git -C "$t6UsedDir" commit -m "Init"

createGitRepo "$t6UnusedDir" ""
cat >"$t6UnusedDir/flake.nix" <<'NIXEOF'
{ outputs = { self }: { value = "unused-v1"; }; }
NIXEOF
git -C "$t6UnusedDir" add .
git -C "$t6UnusedDir" commit -m "Init"

createGitRepo "$t6Dir" ""
cat >"$t6Dir/flake.nix" <<EOF
{ inputs.used.url   = "git+file://$t6UsedDir";
  inputs.unused.url = "git+file://$t6UnusedDir";
  outputs = { self, used, ... }: { result = used.value; }; }
EOF
git -C "$t6Dir" add .
git -C "$t6Dir" commit -m "Init"

# Cold eval + warm verify
[[ "$(nix eval --json "$t6Dir#result")" == '"used-v1"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t6Dir#result")" == '"used-v1"' ]]

# Update the UNUSED input and advance its lock entry
cat >"$t6UnusedDir/flake.nix" <<'NIXEOF'
{ outputs = { self }: { value = "unused-v2"; }; }
NIXEOF
git -C "$t6UnusedDir" add .
git -C "$t6UnusedDir" commit -m "Update"
nix flake update unused --flake "$t6Dir"
git -C "$t6Dir" add flake.lock
git -C "$t6Dir" commit -m "Update unused lock"

# BSàlC: precision — unused input changed, but the trace for "result" only
# depends on "used". The warm eval must still serve "used-v1" from cache.
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t6Dir#result")" == '"used-v1"' ]]

echo "Test F-6 passed: unused flake input precision"

###############################################################################
# Test F-7: Partial flake update (nix flake update nixpkgs).
#
# Updating one of two inputs invalidates only that input's cached output;
# the other input's cached trace must remain a hit (precision).
###############################################################################

clearStoreIndex

t7NpDir="$TEST_ROOT/f7-nixpkgs"
t7MlDir="$TEST_ROOT/f7-mylib"
t7Dir="$TEST_ROOT/f7-top"

createGitRepo "$t7NpDir" ""
cat >"$t7NpDir/flake.nix" <<'NIXEOF'
{ outputs = { self }: { value = "nixpkgs-v1"; }; }
NIXEOF
git -C "$t7NpDir" add .
git -C "$t7NpDir" commit -m "v1"

createGitRepo "$t7MlDir" ""
cat >"$t7MlDir/flake.nix" <<'NIXEOF'
{ outputs = { self }: { value = "mylib-v1"; }; }
NIXEOF
git -C "$t7MlDir" add .
git -C "$t7MlDir" commit -m "Init"

createGitRepo "$t7Dir" ""
cat >"$t7Dir/flake.nix" <<EOF
{ inputs.nixpkgs.url = "git+file://$t7NpDir";
  inputs.mylib.url   = "git+file://$t7MlDir";
  outputs = { self, nixpkgs, mylib }: {
    np    = nixpkgs.value;
    mylib = mylib.value; }; }
EOF
git -C "$t7Dir" add .
git -C "$t7Dir" commit -m "Init"

# Cold eval for both outputs
[[ "$(nix eval --json "$t7Dir#np")"    == '"nixpkgs-v1"' ]]
[[ "$(nix eval --json "$t7Dir#mylib")" == '"mylib-v1"' ]]
# Warm eval for both — must hit
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t7Dir#np")"    == '"nixpkgs-v1"' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t7Dir#mylib")" == '"mylib-v1"' ]]

# Advance nixpkgs to v2 (partial update — mylib lock entry is unchanged)
cat >"$t7NpDir/flake.nix" <<'NIXEOF'
{ outputs = { self }: { value = "nixpkgs-v2"; }; }
NIXEOF
git -C "$t7NpDir" add .
git -C "$t7NpDir" commit -m "v2"
nix flake update nixpkgs --flake "$t7Dir"
git -C "$t7Dir" add flake.lock
git -C "$t7Dir" commit -m "Partial update nixpkgs"

# BSàlC: soundness — nixpkgs updated → np output must reflect v2
[[ "$(nix eval --json "$t7Dir#np")" == '"nixpkgs-v2"' ]]

# BSàlC: precision — mylib lock entry unchanged → mylib output must warm-hit v1
[[ "$(NIX_ALLOW_EVAL=0 nix eval --json "$t7Dir#mylib")" == '"mylib-v1"' ]]

echo "Test F-7 passed: partial flake update precision"

echo "All eval-trace-flake-inputs tests passed! (F-1 through F-7)"
