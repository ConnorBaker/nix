#!/usr/bin/env bash

set -eu -o pipefail

source common.sh

# Avoid store dir being inside sandbox build-dir
unset NIX_STORE_DIR
unset NIX_STATE_DIR

setupStoreDirs

initLowerStore

# Raw names are not StorePaths. Their lower bytes and dangling link survive GC.
rawDuplicate=gc-raw-duplicate
rawDangling=gc-raw-dangling
printf 'lower raw bytes\n' > "$storeA/nix/store/$rawDuplicate"
ln -s lower-missing-target "$storeA/nix/store/$rawDangling"
lowerRawInode=$(stat -c %i "$storeA/nix/store/$rawDuplicate")

mountOverlayfs

# Exercise upper-only raw deletion and duplicate removal through the real mount.
mergedStore="$storeBRoot/nix/store"
printf 'upper raw bytes\n' > "$mergedStore/$rawDuplicate"
ln -snf upper-missing-target "$mergedStore/$rawDangling"
printf 'upper-only bytes\n' > "$mergedStore/gc-raw-upper-only"
mkdir "$mergedStore/tmp-held-gc" "$mergedStore/tmp-unlocked-gc"
exec {temporaryLock}< "$mergedStore/tmp-held-gc"
trap '[[ ! -v temporaryLock ]] || exec {temporaryLock}<&-; cleanupOverlay' EXIT
flock -x "$temporaryLock"

# Use the existing remount action, with a witness only after it succeeds.
remountWitness="$TEST_ROOT/owned-gc-remount-witness"
remountAction="$TEST_ROOT/owned-gc-remount.sh"
export remountWitness
cat > "$remountAction" <<'EOF'
#!/usr/bin/env bash
set -eu
mount -o remount "$1"
printf 'remounted\n' >> "$remountWitness"
EOF
chmod +x "$remountAction"
export NIX_REMOTE="$storeB&remount-hook=$remountAction"

stateB="$storeBRoot/nix/var/nix"
outPath=$(nix-build ../hermetic.nix --no-out-link --arg busybox "$busybox" --arg seed 2)

# Set a GC root.
mkdir -p "$stateB"
rm -f "$stateB/gcroots/foo"
ln -sf "$outPath" "$stateB/gcroots/foo"

[ "$(nix-store -q --roots "$outPath")" = "$stateB/gcroots/foo -> $outPath" ]

nix-store --gc --print-roots | grep "$outPath"
nix-store --gc --print-live | grep "$outPath"
if nix-store --gc --print-dead | grep -E "$outPath"$; then false; fi

nix-store --gc --print-dead

expect 1 nix-store --delete "$outPath"
test -e "$storeBRoot/$outPath"

shopt -s nullglob
for i in "$storeBRoot"/*; do
    if [[ $i =~ /trash ]]; then continue; fi # compat with old daemon
    touch "$i".lock
    touch "$i".chroot
done

rm -f "$remountWitness"
nix-collect-garbage

# Upper duplicates are gone without whiteouts; the lower objects remain visible.
test -s "$remountWitness"
test "$(stat -c %i "$storeA/nix/store/$rawDuplicate")" = "$lowerRawInode"
test "$(cat "$storeA/nix/store/$rawDuplicate")" = 'lower raw bytes'
test "$(cat "$mergedStore/$rawDuplicate")" = 'lower raw bytes'
test "$(readlink "$storeA/nix/store/$rawDangling")" = lower-missing-target
test "$(readlink "$mergedStore/$rawDangling")" = lower-missing-target
for name in "$rawDuplicate" "$rawDangling" gc-raw-upper-only tmp-unlocked-gc; do
    test ! -e "$storeBTop/$name"
    test ! -L "$storeBTop/$name"
done
test ! -e "$mergedStore/gc-raw-upper-only"
test ! -e "$mergedStore/tmp-unlocked-gc"
test -d "$mergedStore/tmp-held-gc"
exec {temporaryLock}<&-
unset temporaryLock


# Check that the root and its dependencies haven't been deleted.
cat "$storeBRoot/$outPath"

rm "$stateB/gcroots/foo"

nix-collect-garbage

# Releasing the held directory lock permits the next collection to remove it.
test ! -e "$mergedStore/tmp-held-gc"
test ! -e "$storeBTop/tmp-held-gc"

# Check that the output has been GC'd.
test ! -e "$outPath"

# Check that the store is empty.
# shellcheck disable=SC2012
[ "$(ls -1 "$storeBTop" | wc -l)" = "0" ]
