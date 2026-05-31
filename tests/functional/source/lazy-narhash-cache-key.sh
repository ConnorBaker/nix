#!/usr/bin/env bash

# Lazy narHash cache-key R3 / cross-process determinism (Item 2).
#
# The `attrsToJSONForKey` variant emits `null` for any LazyAttr,
# producing a stable serialisation per-process. Cross-process
# determinism: two processes evaluating the same flake see the same
# input attrs at the same evaluation points, so the cache-key JSON
# is identical.
#
# This test asserts that two independent `nix eval` invocations
# against the same flake produce byte-identical lockfiles — the
# strongest cross-process determinism guarantee for narHash. (R3 in
# DEFERRED-WORK.md §"Risk register".)

source ../common.sh

requireGit

clearStore
clearCache

repo=$TEST_ROOT/repo
createGitRepo "$repo"
echo "deterministic content" > "$repo/file.txt"
git -C "$repo" add -A
git -C "$repo" commit -m initial
rev=$(git -C "$repo" rev-parse HEAD)

mkdir -p "$TEST_ROOT/flake-A"
cat > "$TEST_ROOT/flake-A/flake.nix" <<EOF
{
  inputs.foo.url = "git+file://$repo?ref=master&rev=$rev";
  inputs.foo.flake = false;
  outputs = { self, foo }: { out = foo.narHash; };
}
EOF

# Initial lock — process 1.
nix flake lock "path:$TEST_ROOT/flake-A" 2> "$TEST_ROOT/lock1.log"
cp "$TEST_ROOT/flake-A/flake.lock" "$TEST_ROOT/lock1.json"

# Independent re-lock — process 2. Clear caches between to ensure
# the flake.lock is regenerated from a fresh fetch path, exercising
# the same key-stable serialisation logic.
clearStore
clearCache
rm -f "$TEST_ROOT/flake-A/flake.lock"

nix flake lock "path:$TEST_ROOT/flake-A" 2> "$TEST_ROOT/lock2.log"
cp "$TEST_ROOT/flake-A/flake.lock" "$TEST_ROOT/lock2.json"

# Cross-process determinism: byte-identical lockfiles.
diff -u "$TEST_ROOT/lock1.json" "$TEST_ROOT/lock2.json" \
    || fail "expected byte-identical lockfiles across two independent eval processes"

# The narHash field should be a concrete SRI string (lockfile writes
# use attrsToJSON which forces).
narHash=$(jq -r '.nodes.foo.locked.narHash' "$TEST_ROOT/lock1.json")
[[ "$narHash" =~ ^sha256- ]] || fail "expected concrete SRI narHash in lockfile, got '$narHash'"

echo "lazy-narhash-cache-key: ok"
