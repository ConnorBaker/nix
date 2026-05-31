#!/usr/bin/env bash

# Item 5 (storeFS root-keyed reshape, doc/tecnix-survey/PROPOSAL.md §6.8):
# the eval root re-roots the absolute store path `<storeDir>` onto the
# now root-keyed `storeFS` with exactly ONE prefix strip (pure-eval Switch
# keyed at `<storeDir>`; impure-eval `StripPrefix(<storeDir>)` inside a
# Union with posixFS). The historical failure mode this guards against is
# the double-strip: `<storeDir>/<hash>-name/file` stripped twice yields a
# residual that no longer matches the inner mount key →
# `/nix/store/nix/store/...` InvalidPath / not-found.
#
# This test materialises a source into the store, then reads a file back
# THROUGH the eval root (`builtins.readFile (storePath + "/f")`), proving
# the single strip resolves end-to-end. It runs the read in the default
# store AND in a relocated chroot store (distinct `<storeDir>`), which is
# where a keying bug surfaces.

source ../common.sh

clearStore

# A source directory with a nested file, materialised via `builtins.path`.
src=$TEST_ROOT/src
mkdir -p "$src/sub"
echo "hello-item5" > "$src/sub/f.txt"

# (1) Default store: materialise + read the nested file back through the
#     eval root. The returned store path is absolute (`<storeDir>/<hash>-name`);
#     `builtins.readFile` resolves `<storeDir>/<hash>-name/sub/f.txt`
#     through `rootFS` — the single-strip path.
storePath=$(nix eval --impure --raw --expr \
    "builtins.path { path = $src; name = \"item5-src\"; }")
echo "storePath=$storePath"

content=$(nix eval --impure --raw --expr \
    "builtins.readFile (builtins.path { path = $src; name = \"item5-src\"; } + \"/sub/f.txt\")")
[[ "$content" = "hello-item5" ]] \
    || fail "default-store read through eval root failed: got '$content'"

# Reading the materialised path directly as a store path string must also
# resolve through the eval root (exercises the getMount / Switch dispatch
# on the absolute key).
content2=$(nix eval --impure --raw --expr "builtins.readFile (\"$storePath\" + \"/sub/f.txt\")")
[[ "$content2" = "hello-item5" ]] \
    || fail "default-store store-path-string read failed: got '$content2'"

# (2) Relocated chroot store (distinct <storeDir>): the same eval against a
#     `local?root=...` store. A double-strip bug manifests here as
#     InvalidPath / not-found because the residual key mismatches.
chroot=$TEST_ROOT/chroot-store
content3=$(nix eval --impure --raw --store "local?root=$chroot" --expr \
    "builtins.readFile (builtins.path { path = $src; name = \"item5-src\"; } + \"/sub/f.txt\")")
[[ "$content3" = "hello-item5" ]] \
    || fail "chroot-store read through eval root failed (double-strip?): got '$content3'"

echo "storefs-root-key: ok"
