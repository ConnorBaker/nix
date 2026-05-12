#!/usr/bin/env bash

source common.sh

clearStoreIfPossible

# shellcheck disable=SC2016
outPath1=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "foo1"; builder = builtins.toFile "builder" "mkdir $out; echo hello > $out/foo"; }' | nix-build - --no-out-link --auto-optimise-store)
# shellcheck disable=SC2016
outPath2=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "foo2"; builder = builtins.toFile "builder" "mkdir $out; echo hello > $out/foo"; }' | nix-build - --no-out-link --auto-optimise-store)

TODO_NixOS # ignoring the client-specified setting 'auto-optimise-store', because it is a restricted setting and you are not a trusted user
  # TODO: only continue when trusted user or root

# Both store paths must be deduplicated by auto-optimise-store via
# hardlinking. `assertDeduplicated` is a same-inode check.
assertDeduplicated "$outPath1/foo" "$outPath2/foo"

# Structural invariant: the canonical's nlink == 3 (user1 + user2
# + the `.links/` entry name itself). We inspect the canonical
# because its nlink is deterministic across optimise's iteration
# order.
canonical="$(findCanonicalLink "$outPath1"/foo)"
if [ -z "$canonical" ]; then
    echo "no .links/ canonical entry found for $outPath1/foo"
    exit 1
fi
canonicalNlink="$(stat --format=%h "$canonical")"
if [ "$canonicalNlink" != 3 ]; then
    echo "canonical nlink incorrect: got $canonicalNlink, want 3"
    exit 1
fi

# shellcheck disable=SC2016
outPath3=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "foo3"; builder = builtins.toFile "builder" "mkdir $out; echo hello > $out/foo"; }' | nix-build - --no-out-link)

# outPath3 was built *without* --auto-optimise-store, so it must be
# a fresh inode (not yet deduped).
inode1="$(stat --format=%i "$outPath1"/foo)"
inode3="$(stat --format=%i "$outPath3"/foo)"
if [ "$inode1" = "$inode3" ]; then
    echo "inodes match unexpectedly"
    exit 1
fi

# XXX: This should work through the daemon too
NIX_REMOTE="" nix-store --optimise

# After an explicit optimise, outPath3 must also be deduped against
# outPath1.
assertDeduplicated "$outPath1/foo" "$outPath3/foo"

nix-store --gc

if [ -n "$(find "$NIX_STORE_DIR/.links" -mindepth 1 ! -type d 2>/dev/null)" ]; then
    echo ".links directory not empty after GC"
    exit 1
fi
