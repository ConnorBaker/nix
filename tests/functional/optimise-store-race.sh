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
# dedups both files.
NIX_REMOTE="" nix-store --optimise

inode1="$(stat --format=%i "$outPath1"/file)"
inode2="$(stat --format=%i "$outPath2"/file)"
if [ "$inode1" != "$inode2" ]; then
    echo "FAIL: files were not deduplicated after optimise"
    exit 1
fi

# Positive: a second optimise is a no-op; files are already deduped.
NIX_REMOTE="" nix-store --optimise

inode1b="$(stat --format=%i "$outPath1"/file)"
if [ "$inode1b" != "$inode1" ]; then
    echo "FAIL: second optimise changed inode unexpectedly"
    exit 1
fi

# Negative: pre-delete the .links/ entry while the store files point
# to the (former) canonical inode. optimise must recreate the link
# without throwing.
# We find the .links entry by matching inode of the deduped files.
canonicalInode="$inode1"
linkFile=""
for f in "$NIX_STORE_DIR"/.links/*; do
    if [ "$(stat --format=%i "$f")" = "$canonicalInode" ]; then
        linkFile="$f"
        break
    fi
done

if [ -z "$linkFile" ]; then
    echo "FAIL: could not find .links/ entry for inode $canonicalInode"
    exit 1
fi

# Simulate the GC race: remove the .links entry, then optimise.
rm -f "$linkFile"

# The store files still reference each other (nlink >= 2). We expect
# optimise to re-create the .links entry without error.
NIX_REMOTE="" nix-store --optimise

# The .links entry should exist again.
recreated=""
for f in "$NIX_STORE_DIR"/.links/*; do
    if [ "$(stat --format=%i "$f")" = "$canonicalInode" ]; then
        recreated="$f"
        break
    fi
done

if [ -z "$recreated" ]; then
    echo "FAIL: .links/ entry was not recreated after race"
    exit 1
fi

# Inodes must still be shared.
inode1c="$(stat --format=%i "$outPath1"/file)"
inode2c="$(stat --format=%i "$outPath2"/file)"
if [ "$inode1c" != "$inode2c" ]; then
    echo "FAIL: dedup lost after recreate"
    exit 1
fi

# Negative: a corrupted .links entry (size mismatch) must be removed
# and rebuilt, not cause optimise to fail.
# First, delete existing link then replace with an empty file of same name.
rm -f "$recreated"
: > "$recreated"
# Make it read-only to match store permissions.
chmod 0444 "$recreated"

NIX_REMOTE="" nix-store --optimise

# The entry must now have the real size (non-zero) again.
linkSize="$(stat --format=%s "$recreated" 2>/dev/null || echo 0)"
if [ "$linkSize" = "0" ]; then
    # Either it was recreated with the right contents, or a new entry
    # replaced it. Search again by inode.
    for f in "$NIX_STORE_DIR"/.links/*; do
        if [ "$(stat --format=%i "$f")" = "$(stat --format=%i "$outPath1"/file)" ]; then
            recreated="$f"
            break
        fi
    done
    linkSize="$(stat --format=%s "$recreated")"
fi

if [ "$linkSize" = "0" ]; then
    echo "FAIL: corrupted .links entry was not rebuilt"
    exit 1
fi

echo "PASS: optimise-store-race"
