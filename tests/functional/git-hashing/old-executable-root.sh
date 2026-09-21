#!/usr/bin/env bash

# A git-addressed path whose root is a bare executable file or a symlink,
# made by an older Nix, is refused on copy into this store, and the message
# names the address change.
#
# The release note (doc/manual/rl-next/one-address.md, *Sources*) states it:
#
#   An earlier Nix that did address such a root under the git method (`nix
#   store add --mode git` on an executable file or a symlink) named it by the
#   blob id of its bytes, the mode discarded; this Nix names it by the id of
#   the synthetic one-entry tree, so the two disagree on that path's name, and
#   a copy of such a path from an older store or cache into this one -- `nix
#   copy`, substitution, anything that carries the path's content address --
#   is refused with `ca hash mismatch importing path '…'; specified:
#   sha256:<blob id>; got: sha256:<tree id>`.
#   (`nix-store --import` carries no content address and admits the path as it
#   is.)  Re-add the source under this Nix to obtain its new name.
#
# The address itself is doc/lazy-store/01-specification.md section 9.11 and
# tests/functional/git-hashing/bare-root.sh; what is pinned here is the older
# peer's disagreement, which needs a binary built from master
# (NIX_REFERENCE_BIN) and is skipped without one.

source common.sh

needLocalStore "the reference binary is given store roots of this test"

if [[ -z "${NIX_REFERENCE_BIN:-}" || ! -x "$NIX_REFERENCE_BIN/nix" ]]; then
    skipTest "NIX_REFERENCE_BIN is not set, so no Nix built from master is in hand"
fi

# The harness points the dynamic loader at this build's libraries; the
# reference binary must find its own.  Master addresses a source under the git
# method only with its `git-hashing` experimental feature.
ref() {
    env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u LD_LIBRARY_PATH \
        "$NIX_REFERENCE_BIN/nix" --extra-experimental-features 'nix-command git-hashing' "$@"
}
refLegacy() {
    local cmd=$1
    shift
    NIX_CONFIG="extra-experimental-features = git-hashing" \
        env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u LD_LIBRARY_PATH \
        "$NIX_REFERENCE_BIN/$cmd" "$@"
}

d=$TEST_ROOT/old-roots
rm -rf "$d"
mkdir -p "$d"
printf 'Hello World\n' > "$d/prog"
chmod +x "$d/prog"
ln -s prog "$d/link"

oldStore=$TEST_ROOT/old-exec-store
oldCache=$TEST_ROOT/old-exec-cache
newStore=$TEST_ROOT/new-exec-store
rm -rf "$oldStore" "$oldCache" "$newStore"

# The two roots the git method used to name by the blob id alone.
for root in prog link; do
    oldPath=$(ref store add --store "$oldStore" --mode git "$d/$root")
    ref copy --from "$oldStore" --to "file://$oldCache" "$oldPath"

    # Master named it by the blob id of its bytes, the mode discarded; this
    # Nix names the same root by the id of the one-entry tree that keeps the
    # mode, so the two disagree.
    blobId=$(ref hash path --mode git --algo sha256 --format nix32 "$d/$root")
    treeId=$(nix hash path --mode git --algo sha256 --format nix32 "$d/$root")
    [[ $blobId != "$treeId" ]]
    grepQuiet "^CA: fixed:git:sha256:$blobId\$" "$oldCache"/*.narinfo

    # The description is read here: a reader tolerates what a creator refuses.
    nix path-info --store "file://$oldCache" --json --json-format 2 "$oldPath" \
        | jq -e --arg h "sha256-" '.info.[].ca.method == "git" and (.info.[].ca.hash | startswith($h))'

    # The copy carries the content address, so it is refused, and the message
    # names both names.
    log=$TEST_ROOT/old-exec-copy-$root.log
    expectStderr 1 nix copy --from "file://$oldCache" --to "$newStore" "$oldPath" > "$log"
    grepQuiet "ca hash mismatch importing path '$oldPath'" "$log"
    grepQuiet "specified: sha256:$blobId" "$log"
    grepQuiet "got: *sha256:$treeId" "$log"
    [[ ! -e "$newStore$oldPath" ]]

    # `nix-store --import` carries no content address and admits the path as
    # it is.
    refLegacy nix-store --store "$oldStore" --export "$oldPath" > "$TEST_ROOT/old-exec-$root.export"
    nix-store --store "$newStore" --import < "$TEST_ROOT/old-exec-$root.export" \
        | grepQuiet "^$oldPath\$"
    nix path-info --store "$newStore" --json --json-format 2 "$oldPath" \
        | jq -e '.info.[].ca == null'

    # The remedy: re-added here, the same root gets its new name, and that
    # path's address is the tree id.
    newPath=$(nix store add --store "$newStore" --mode git "$d/$root")
    [[ $newPath != "$oldPath" ]]
    nix path-info --store "$newStore" --json --json-format 2 "$newPath" \
        | jq -e --arg h "$treeId" '.info.[].ca.method == "git"'
    [[ $(nix hash convert --hash-algo sha256 --to nix32 \
        "$(nix path-info --store "$newStore" --json --json-format 2 "$newPath" | jq -r '.info.[].ca.hash')") \
       == "$treeId" ]]
done
