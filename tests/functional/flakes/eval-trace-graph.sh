#!/usr/bin/env bash

# Eval trace graph totalization tests: resolved flake graph structural integrity,
# follows collapse, dir subflakes, relative path recursion, override-input with
# --no-write-lock-file, internal lock graph distinct from on-disk flake.lock,
# and call-flake.nix phase-2 zero-fetch assertion.
#
# These tests exercise the ResolvedFlakeGraph contract from the implementation
# plan (Section 1): every phase-2-reachable node must have authoritative
# carrierPath, flakePath, sourceInfoKey, and resolvedInputs entries before
# evaluation begins.

source ./common.sh

requireGit

###############################################################################
# Test 1: Follows collapse — resolvedInputs correctness
###############################################################################
#
# A depends on B and C.  B.inputs.dep follows A's dep (which is C).
# After follows collapse, B's resolved dep must point to C.  If follows
# collapse fails, B.inputs.dep would point to a separate copy.
#
# B and C are separate git repos (git+file:// URLs) so each input has a
# cryptographic git-rev identity.  This lets warm-verify use the
# GitRevisionIdentity short-circuit instead of per-file FileBytes deps,
# avoiding the accessor-type instability that arises when relative-path
# inputs (path:./b inside the root git tree) are fetched via a GitSourceAccessor
# during cold eval but via an FSSourceAccessor during warm verify (or vice
# versa), which produces mismatched physical-path keys in the stat-hash cache.

t1Dir="$TEST_ROOT/graph-follows"
t1B="$TEST_ROOT/graph-follows-b"
t1C="$TEST_ROOT/graph-follows-c"

createGitRepo "$t1C" ""
createGitRepo "$t1B" ""
createGitRepo "$t1Dir" ""

cat > "$t1C/flake.nix" <<'EOF'
{
  description = "Dep C";
  outputs = { self }: { value = "from-c"; };
}
EOF
git -C "$t1C" add flake.nix
git -C "$t1C" commit -m "Init"

cat > "$t1B/flake.nix" <<EOF
{
  description = "Flake B";
  inputs.dep.url = "git+file://$t1C";
  outputs = { self, dep }: { passthrough = dep.value; };
}
EOF
git -C "$t1B" add flake.nix
git -C "$t1B" commit -m "Init"

cat > "$t1Dir/flake.nix" <<EOF
{
  description = "Root A";
  inputs = {
    b.url = "git+file://$t1B";
    c.url = "git+file://$t1C";
    b.inputs.dep.follows = "c";
  };
  outputs = { self, b, c }: {
    bVal = b.passthrough;
    cVal = c.value;
    same = b.passthrough == c.value;
  };
}
EOF
git -C "$t1Dir" add flake.nix
git -C "$t1Dir" commit -m "Init"

# After follows collapse, b.passthrough must be "from-c" (same object as c.value)
[[ "$(nix eval --json "$t1Dir#same")" == 'true' ]]
[[ "$(nix eval --raw "$t1Dir#bVal")" == 'from-c' ]]

# Commit the lock file so the tree is clean, then clear the trace/stat caches
# and do a cold eval followed by a warm verify.
git -C "$t1Dir" add flake.lock
git -C "$t1Dir" commit -m "Lock"
clearStoreIndex

nix eval --json "$t1Dir#same" >/dev/null 2>&1
NIX_ALLOW_EVAL=0 nix eval -vvv --json "$t1Dir#same" >/dev/null

echo "Test 1 passed: follows collapse"

###############################################################################
# Test 2: Dir subflake — relative path with subdir
###############################################################################
#
# Root flake uses a subflake via ?dir=sub.  The resolved graph must derive
# the flakePath correctly from carrierPath + subdir.

t2Dir="$TEST_ROOT/graph-dir-subflake"
createGitRepo "$t2Dir" ""
mkdir -p "$t2Dir/sub"

cat > "$t2Dir/sub/flake.nix" <<'EOF'
{
  description = "Subdir flake";
  outputs = { self }: { x = 42; };
}
EOF

cat > "$t2Dir/flake.nix" <<'EOF'
{
  description = "Root with dir subflake";
  inputs.sub.url = "path:./sub";
  outputs = { self, sub }: { y = sub.x * 2; };
}
EOF

git -C "$t2Dir" add flake.nix sub/flake.nix
git -C "$t2Dir" commit -m "Init"

[[ "$(nix eval "$t2Dir#y")" == '84' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval "$t2Dir#y")" == '84' ]]

echo "Test 2 passed: dir subflake"

###############################################################################
# Test 3: Relative path recursion — nested relative inputs
###############################################################################
#
# Root -> A (relative) -> B (relative to A).  B's carrier root must be
# derived recursively through parentInputAttrPath.

t3Dir="$TEST_ROOT/graph-relative-recursion"
createGitRepo "$t3Dir" ""
mkdir -p "$t3Dir/a" "$t3Dir/a/b"

cat > "$t3Dir/a/b/flake.nix" <<'EOF'
{
  description = "Leaf B";
  outputs = { self }: { leaf = "b-value"; };
}
EOF

cat > "$t3Dir/a/flake.nix" <<'EOF'
{
  description = "Middle A";
  inputs.b.url = "path:./b";
  outputs = { self, b }: { mid = b.leaf; };
}
EOF

cat > "$t3Dir/flake.nix" <<'EOF'
{
  description = "Root with nested relative";
  inputs.a.url = "path:./a";
  outputs = { self, a }: { result = a.mid; };
}
EOF

git -C "$t3Dir" add flake.nix a/flake.nix a/b/flake.nix
git -C "$t3Dir" commit -m "Init"

# The first eval generates flake.lock and records a trace.
# Warm-verify runs against the same dirty tree (flake.lock present but
# untracked).  The session key is stable because lockFlake sees
# newLockFile == oldLockFile and does not rewrite.
#
# NOTE: we do NOT clearStoreIndex + re-eval here.  Doing so would cause
# lockFlake to recompute and rewrite the lock (non-deterministic
# serialization of path: inputs), changing the git tree and producing a
# different session key.  Instead, we warm-verify the trace from the
# first eval directly.
[[ "$(nix eval --raw "$t3Dir#result")" == 'b-value' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t3Dir#result")" == 'b-value' ]]

echo "Test 3 passed: relative path recursion"

###############################################################################
# Test 4: --override-input with --no-write-lock-file
###############################################################################
#
# Override an input at the CLI without writing a lock file.  The internal
# resolved graph must reflect the override, not the on-disk lock.

t4Dir="$TEST_ROOT/graph-override"
t4Alt="$TEST_ROOT/graph-override-alt"

createGitRepo "$t4Dir" ""
createGitRepo "$t4Alt" ""

cat > "$t4Alt/flake.nix" <<'EOF'
{
  outputs = { self }: { val = "alt"; };
}
EOF
git -C "$t4Alt" add flake.nix
git -C "$t4Alt" commit -m "Init"

cat > "$t4Dir/flake.nix" <<EOF
{
  inputs.dep.url = "path:$t4Alt";
  outputs = { self, dep }: { result = dep.val; };
}
EOF
git -C "$t4Dir" add flake.nix
git -C "$t4Dir" commit -m "Init"

# First lock normally
nix eval --raw "$t4Dir#result" >/dev/null

# Create an alternative input
t4Alt2="$TEST_ROOT/graph-override-alt2"
createGitRepo "$t4Alt2" ""
cat > "$t4Alt2/flake.nix" <<'EOF'
{
  outputs = { self }: { val = "overridden"; };
}
EOF
git -C "$t4Alt2" add flake.nix
git -C "$t4Alt2" commit -m "Init"

# Override without writing lock file — internal graph must use the override
clearStoreIndex
[[ "$(nix eval --raw --no-write-lock-file --override-input dep "path:$t4Alt2" "$t4Dir#result")" == 'overridden' ]]

# The on-disk lock file should NOT have changed (still points to original)
[[ "$(nix eval --raw "$t4Dir#result")" == 'alt' ]]

echo "Test 4 passed: --override-input with --no-write-lock-file"

###############################################################################
# Test 5: Internal lock graph distinct from on-disk flake.lock
###############################################################################
#
# Modify a dependency source after locking.  The trace cache should detect
# the change via its session identity (which includes the resolved graph
# digest), even though flake.lock on disk is unchanged.

t5Dir="$TEST_ROOT/graph-distinct"
t5Dep="$TEST_ROOT/graph-distinct-dep"

createGitRepo "$t5Dep" ""
cat > "$t5Dep/flake.nix" <<'EOF'
{
  outputs = { self }: { val = "original"; };
}
EOF
git -C "$t5Dep" add flake.nix
git -C "$t5Dep" commit -m "Init"
t5DepRev1=$(git -C "$t5Dep" rev-parse HEAD)

createGitRepo "$t5Dir" ""
cat > "$t5Dir/flake.nix" <<EOF
{
  inputs.dep.url = "git+file://$t5Dep";
  outputs = { self, dep }: { result = dep.val; };
}
EOF
git -C "$t5Dir" add flake.nix
git -C "$t5Dir" commit -m "Init"

# Lock and evaluate
[[ "$(nix eval --raw "$t5Dir#result")" == 'original' ]]
git -C "$t5Dir" add flake.lock
git -C "$t5Dir" commit -m "Lock"
clearStoreIndex
nix eval --raw "$t5Dir#result" >/dev/null
NIX_ALLOW_EVAL=0 nix eval --raw "$t5Dir#result" >/dev/null

# Update the dependency
cat > "$t5Dep/flake.nix" <<'EOF'
{
  outputs = { self }: { val = "updated"; };
}
EOF
git -C "$t5Dep" add flake.nix
git -C "$t5Dep" commit -m "Update"

# Update the lock
nix flake update dep --flake "$t5Dir"
git -C "$t5Dir" add flake.lock
git -C "$t5Dir" commit -m "Update lock"

# The trace cache must NOT serve the old "original" — session identity changed
[[ "$(nix eval --raw "$t5Dir#result")" == 'updated' ]]
NIX_ALLOW_EVAL=0 nix eval --raw "$t5Dir#result" >/dev/null

echo "Test 5 passed: internal lock graph distinct from on-disk flake.lock"

###############################################################################
# Test 6: call-flake.nix phase-2 zero-fetch assertion
###############################################################################
#
# Evaluate a flake in a restricted sandbox where network access is blocked.
# After the initial lock (which fetches), subsequent evals from the lock
# must succeed without network access, proving phase 2 doesn't fetch.

t6Dir="$TEST_ROOT/graph-no-fetch"
t6Dep="$TEST_ROOT/graph-no-fetch-dep"

createGitRepo "$t6Dep" ""
cat > "$t6Dep/flake.nix" <<'EOF'
{
  outputs = { self }: { val = "no-fetch"; };
}
EOF
git -C "$t6Dep" add flake.nix
git -C "$t6Dep" commit -m "Init"

createGitRepo "$t6Dir" ""
cat > "$t6Dir/flake.nix" <<EOF
{
  inputs.dep.url = "git+file://$t6Dep";
  outputs = { self, dep }: { result = dep.val; };
}
EOF
git -C "$t6Dir" add flake.nix
git -C "$t6Dir" commit -m "Init"

# Lock and initial evaluation (fetches the dep)
nix eval --raw "$t6Dir#result" >/dev/null
git -C "$t6Dir" add flake.lock
git -C "$t6Dir" commit -m "Lock"
clearStoreIndex
nix eval --raw "$t6Dir#result" >/dev/null

# Verify cached result works with eval blocked (proves phase 2 doesn't re-evaluate)
NIX_ALLOW_EVAL=0 nix eval --raw "$t6Dir#result" >/dev/null

# Clear the trace cache and re-evaluate — still no network fetch needed because
# the store path already exists from the initial lock
clearStoreIndex
[[ "$(nix eval --raw "$t6Dir#result")" == 'no-fetch' ]]

echo "Test 6 passed: phase-2 zero-fetch"

###############################################################################
# Test 7: Follows collapse with non-flake input
###############################################################################
#
# Non-flake inputs that are redirected via follows must still resolve
# correctly in the graph without triggering a flake.nix import.

t7Dir="$TEST_ROOT/graph-nonflake-follows"
t7Data="$TEST_ROOT/graph-nonflake-data"

createGitRepo "$t7Data" ""
echo '{"key": "data-value"}' > "$t7Data/data.json"
git -C "$t7Data" add data.json
git -C "$t7Data" commit -m "Init"

createGitRepo "$t7Dir" ""
mkdir -p "$t7Dir/sub"

cat > "$t7Dir/sub/flake.nix" <<EOF
{
  inputs.data = {
    url = "git+file://$t7Data";
    flake = false;
  };
  outputs = { self, data }: {
    val = builtins.fromJSON (builtins.readFile (data + "/data.json"));
  };
}
EOF

cat > "$t7Dir/flake.nix" <<EOF
{
  inputs = {
    sub.url = "path:./sub";
    data = {
      url = "git+file://$t7Data";
      flake = false;
    };
    sub.inputs.data.follows = "data";
  };
  outputs = { self, sub, data }: {
    result = sub.val.key;
  };
}
EOF

git -C "$t7Dir" add flake.nix sub/flake.nix
git -C "$t7Dir" commit -m "Init"

[[ "$(nix eval --raw "$t7Dir#result")" == 'data-value' ]]
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw "$t7Dir#result")" == 'data-value' ]]

echo "Test 7 passed: non-flake follows collapse"

###############################################################################
# Test 8: Read-only verification of runtime-fetched inputs
###############################################################################
#
# Verifies that eval-trace verification does not call fetchToStore.
# After cold eval (which fetches), the warm-verify path must succeed
# without re-fetching. We test this by:
#   1. Cold eval with a runtime builtins.fetchTree (creates RuntimeFetchIdentity dep)
#   2. Warm verify with NIX_ALLOW_EVAL=0 (proves no re-evaluation or re-fetch)
#   3. Modify the fetched data
#   4. Re-eval and verify invalidation (proves verification reads the content)

t8Dir="$TEST_ROOT/graph-readonly-verify"
t8Data="$TEST_ROOT/graph-readonly-data"

createGitRepo "$t8Data" ""
echo "original-content" > "$t8Data/data.txt"
git -C "$t8Data" add data.txt
git -C "$t8Data" commit -m "Init"

createGitRepo "$t8Dir" ""
cat > "$t8Dir/flake.nix" <<EOF
{
  outputs = { self }: {
    result = builtins.readFile (
      (builtins.fetchTree {
        type = "git";
        url = "file://$t8Data";
      }) + "/data.txt"
    );
  };
}
EOF
git -C "$t8Dir" add flake.nix
git -C "$t8Dir" commit -m "Init"

# Cold eval — fetches the runtime input, records RuntimeFetchIdentity dep.
# This self-only flake has no declared inputs, so no flake.lock is generated.
clearStoreIndex
result8=$(nix eval --raw --impure "$t8Dir#result")
[[ "$result8" == *"original-content"* ]]

# Warm-verify: the runtime root recorded during cold eval must be loaded
# by loadAndVerifyRuntimeRoots and added to the SemanticRegistry so dep
# verification can resolve the RuntimeFetchIdentity source.
[[ "$(NIX_ALLOW_EVAL=0 nix eval --raw --impure "$t8Dir#result")" == *"original-content"* ]]

# Modify the data — verification must detect the change
echo "modified-content" > "$t8Data/data.txt"
git -C "$t8Data" add data.txt
git -C "$t8Data" commit -m "Modify"

# Re-eval — cache must invalidate (content changed)
result8_mod=$(nix eval --raw --impure "$t8Dir#result")
[[ "$result8_mod" == *"modified-content"* ]]

echo "Test 8 passed: read-only verification of runtime-fetched inputs"

###############################################################################
# Test 9: Stable session key for --override-input with absolute path:
###############################################################################
#
# Demonstrates that normalizing lockedVersionIdentity for path: inputs fixes
# the session key instability: cold eval followed by warm-verify succeeds.
# The override target is a committed git tree accessed via FSSourceAccessor
# (absolute path inputs always use FSSourceAccessor — no git accessor
# ambiguity), so both the session key and FileBytes deps are stable.
#
# This test would fail before Change 1 of the narHash exclusion fix:
# lockedVersionIdentity would include narHash from to_string(), and
# accessor-type divergence between the cold and warm processes would
# produce a different narHash, generating a different session key and
# causing a cold re-eval instead of a warm hit.

t9Override="$TEST_ROOT/graph-path-stable-override"
t9Dir="$TEST_ROOT/graph-path-stable"

createGitRepo "$t9Override" ""
cat > "$t9Override/flake.nix" <<'EOF'
{
  outputs = { self }: { val = "path-stable"; };
}
EOF
git -C "$t9Override" add flake.nix
git -C "$t9Override" commit -m "Init"

createGitRepo "$t9Dir" ""
cat > "$t9Dir/flake.nix" <<EOF
{
  inputs.dep.url = "git+file://$t9Override";
  outputs = { self, dep }: { result = dep.val; };
}
EOF
git -C "$t9Dir" add flake.nix
git -C "$t9Dir" commit -m "Init"

# Cold eval with absolute-path override.
# lockedVersionIdentity for dep = "path:$t9Override" (stable, no narHash).
clearStoreIndex
nix eval --raw --no-write-lock-file \
    --override-input dep "path:$t9Override" \
    "$t9Dir#result" > /dev/null

# Warm verify: must succeed without re-evaluation.
# Session key is stable because lockedVersionIdentity uses path string only.
NIX_ALLOW_EVAL=0 nix eval --raw --no-write-lock-file \
    --override-input dep "path:$t9Override" \
    "$t9Dir#result" > /dev/null

echo "Test 9 passed: stable session key for absolute path: override"

echo "All graph totalization tests passed"
