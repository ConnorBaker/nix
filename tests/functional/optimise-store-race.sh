#!/usr/bin/env bash

# Test Patch A: `nix-store --optimise` is tolerant of concurrent GC
# racing with optimise on .links/ entries.

source common.sh

TODO_NixOS

clearStoreIfPossible

# Build two derivations that produce identical file contents, so
# optimisation has something to deduplicate.
outPath1=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "race1"; builder = builtins.toFile "builder" "mkdir $out; echo identical > $out/file"; }' | nix-build - --no-out-link)
outPath2=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "race2"; builder = builtins.toFile "builder" "mkdir $out; echo identical > $out/file"; }' | nix-build - --no-out-link)

# Positive: a normal optimise on an empty .links/ populates it and
# dedups both files via hardlinking.
NIX_REMOTE="" nix-store --optimise

assertDeduplicated "$outPath1/file" "$outPath2/file"

inode1="$(stat --format=%i "$outPath1"/file)"

# Positive: a second optimise is a no-op for files already deduped —
# the user file's inode doesn't change because `optimisePath_`'s
# inodeHash fast-path short-circuits when the user file already shares
# an inode with the canonical.
NIX_REMOTE="" nix-store --optimise

assertDeduplicated "$outPath1/file" "$outPath2/file"

inode1b="$(stat --format=%i "$outPath1"/file)"
if [ "$inode1b" != "$inode1" ]; then
    echo "FAIL: second optimise changed inode unexpectedly"
    exit 1
fi

# Negative: pre-delete the .links/ entry, then optimise. The optimise
# loop must recreate the link without throwing.
linkFile="$(findCanonicalLink "$outPath1"/file)"
if [ -z "$linkFile" ]; then
    echo "FAIL: could not find .links/ entry for $outPath1/file"
    exit 1
fi

# Simulate the GC race: remove the .links entry, then optimise.
rm -f "$linkFile"

# The store files still have the same content. Optimise must re-create
# the .links entry without error and re-dedupe both files against it.
NIX_REMOTE="" nix-store --optimise

recreated="$(findCanonicalLink "$outPath1"/file)"
if [ -z "$recreated" ]; then
    echo "FAIL: .links/ entry was not recreated after race"
    exit 1
fi

assertDeduplicated "$outPath1/file" "$outPath2/file"

# Negative: a corrupted .links entry (size mismatch) must be removed
# and rebuilt, not cause optimise to fail.
# First, delete existing link then replace with an empty file of same name.
rm -f "$recreated"
: > "$recreated"
# Make it read-only to match store permissions.
chmod 0444 "$recreated"

NIX_REMOTE="" nix-store --optimise

# After optimise re-runs, there must be a .links entry whose content
# matches the user files (not the corrupted empty placeholder).
recreated="$(findCanonicalLink "$outPath1"/file)"
if [ -z "$recreated" ]; then
    echo "FAIL: corrupted .links entry was not rebuilt"
    exit 1
fi

linkSize="$(stat --format=%s "$recreated")"
if [ "$linkSize" = "0" ]; then
    echo "FAIL: rebuilt .links entry is empty (corruption not detected)"
    exit 1
fi

echo "PASS: optimise-store-race"
