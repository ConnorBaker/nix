#!/usr/bin/env bash

# A bare root is addressed by the object hash (the manual, store/
# file-system-object/content-address.md, "Git"; doc/lazy-store/
# 01-specification.md, section 9.11): a plain file by its blob id, a
# directory by its tree id, and an executable file or a symlink by the id
# of the one-entry tree `{"." -> (mode, blob)}`, which keeps the mode.  The
# address is total -- every door that turns a root into an address accepts
# every root kind -- and injective: two objects with the same bytes that
# differ in the executable bit are two paths, at every door.

source common.sh

# SHA-256 git objects in the store
requireDaemonNewerThan 2.31pre20250724

enableFeatures flakes

d=$TEST_ROOT/bare
rm -rf "$d"
mkdir -p "$d/tree"
printf 'hello\n' > "$d/plain"
printf 'hello\n' > "$d/exec"
chmod +x "$d/exec"
ln -s plain "$d/link"
cp "$d/exec" "$d/tree/run"

# The hashing door: every root kind has a git address.
hPlain=$(nix hash path --mode git --algo sha256 --format base16 "$d/plain")
hExec=$(nix hash path --mode git --algo sha256 --format base16 "$d/exec")
hLink=$(nix hash path --mode git --algo sha256 --format base16 "$d/link")
hTree=$(nix hash path --mode git --algo sha256 --format base16 "$d/tree")
[[ -n $hPlain && -n $hExec && -n $hLink && -n $hTree ]]

# The plain file's address is git's own blob id.  Checked against git when
# it is installed and can hash under SHA-256: `--object-format` on
# `hash-object` is newer than SHA-256 repositories themselves, so a git that
# lacks it is asked through a SHA-256 repository instead, and a git that can
# do neither skips this one assertion.
if [[ $(type -p git) ]]; then
    gitBlob=$(git hash-object --object-format=sha256 -t blob "$d/plain" 2>/dev/null || true)
    if [[ -z $gitBlob ]]; then
        rm -rf "$d/repo"
        if git init -q --object-format=sha256 "$d/repo" 2>/dev/null; then
            gitBlob=$(git -C "$d/repo" hash-object -t blob "$d/plain")
        fi
    fi
    if [[ -n $gitBlob ]]; then
        [[ $hPlain == "$gitBlob" ]]
    else
        echo "git cannot hash under SHA-256; skipping the comparison with 'git hash-object'" >&2
    fi
fi

# Injective: the executable is not the plain file (the same blob, another
# mode) and not the symlink (another mode again).
[[ $hExec != "$hPlain" ]]
[[ $hExec != "$hLink" ]]
[[ $hLink != "$hPlain" ]]

# The store's door: a bare executable is added under the git method, and
# the object's content address is the hash the hashing door computed.
pExec=$(nix store add --mode git --hash-algo sha256 "$d/exec")
[[ $(nix path-info --json --json-format 2 "$pExec" | jq -r '.info | to_entries[0].value.ca.method') == git ]]
caSri=$(nix path-info --json --json-format 2 "$pExec" | jq -r '.info | to_entries[0].value.ca.hash')
[[ $(nix hash convert --hash-algo sha256 --to base16 "$caSri") == "$hExec" ]]
nix store add --mode git --hash-algo sha256 "$d/plain"
nix store add --mode git --hash-algo sha256 "$d/tree"

# The builder's door: a fixed-output derivation in git mode whose output is
# a bare executable builds when its `outputHash` is the object hash of a
# file with the same bytes and the same bit.
nix-build --no-out-link --expr "
  with import ../config.nix;
  mkDerivation {
    name = \"bare\";
    buildCommand = \"echo hello > \\\$out; chmod +x \\\$out\";
    outputHashMode = \"git\";
    outputHashAlgo = \"sha256\";
    outputHash = \"$hExec\";
  }"

# The evaluator's doors: `builtins.path`, the coercion of a path to a
# string, and `fetchTree` name the executable by its object hash -- the
# hash the hashing door computed -- under the git method, the only naming
# of sources (doc/lazy-store/01-specification.md, section 9.11); the plain
# file, the same bytes without the bit, is another path.  A directory takes
# its tree name.
caHashOf() { nix hash convert --hash-algo sha256 --to base16 "$(nix path-info --json --json-format 2 "$1" | jq -r '.info | to_entries[0].value.ca.hash')"; }
caMethodOf() { nix path-info --json --json-format 2 "$1" | jq -r '.info | to_entries[0].value.ca.method'; }
eExec=$(nix eval --raw --impure --expr "builtins.path { path = $d/exec; }")
[[ $eExec == $(nix eval --raw --impure --expr "\"\${$d/exec}\"") ]]
[[ $(caMethodOf "$eExec") == git ]]
[[ $(caHashOf "$eExec") == "$hExec" ]]
# `fetchTree` names its copy `source` (path.cc), so its path differs from
# `builtins.path`'s by name alone: the same object, the same address, and
# the tree's `treeHash` attribute is that address.
tExec=$(nix eval --raw --impure --expr "(builtins.fetchTree { type = \"path\"; path = \"$d/exec\"; }).outPath")
[[ $tExec != "$eExec" ]]
[[ $(caMethodOf "$tExec") == git ]]
[[ $(caHashOf "$tExec") == "$hExec" ]]
[[ $(nix hash convert --hash-algo sha256 --to base16 "$(nix eval --raw --impure --expr "(builtins.fetchTree { type = \"path\"; path = \"$d/exec\"; }).treeHash")") == "$hExec" ]]
# Two objects with the same bytes are two paths, each under its own hash.
ePlain=$(nix eval --raw --impure --expr "builtins.path { path = $d/plain; }")
[[ $ePlain != "$eExec" ]]
[[ $(caMethodOf "$ePlain") == git ]]
[[ $(caHashOf "$ePlain") == "$hPlain" ]]
# A directory takes its tree id.
eTree=$(nix eval --raw --impure --expr "builtins.path { path = $d/tree; }")
[[ $(caMethodOf "$eTree") == git ]]
[[ $(caHashOf "$eTree") == "$hTree" ]]
