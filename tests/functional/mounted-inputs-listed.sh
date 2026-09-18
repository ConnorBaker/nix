#!/usr/bin/env bash

source common.sh

requireGit
clearStoreIfPossible

# A fetched input is mounted at its store path without being copied.  The
# evaluator behaves as if the copy had been performed: in impure evaluation,
# a directory listing of the store directory includes the mounted input, and
# the directories leading to a mount point exist.

repo=$TEST_ROOT/repo
createGitRepo "$repo"
echo hello > "$repo/hello"
git -C "$repo" add hello
git -C "$repo" commit -q -m init

# The basename carries no string context, so using it as a name or printing it copies nothing.
expr="let t = builtins.fetchTree { type = \"git\"; url = \"file://$repo\"; }; name = builtins.unsafeDiscardStringContext (baseNameOf t.outPath); in"

name=$(nix eval --impure --raw --expr "$expr name")
[[ $name =~ -source$ ]]
[[ ! -e "$NIX_STORE_DIR/$name" ]]

# The listing of the store directory shows the input all the same.
[[ $(nix eval --impure --expr "$expr builtins.seq name (builtins.hasAttr name (builtins.readDir builtins.storeDir))") = true ]]
[[ $(nix eval --impure --expr "$expr builtins.seq name ((builtins.readDir builtins.storeDir).\${name})") = '"directory"' ]]

# And still nothing was copied.
[[ ! -e "$NIX_STORE_DIR/$name" ]]
