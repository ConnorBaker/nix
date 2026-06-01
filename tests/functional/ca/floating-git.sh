#!/usr/bin/env bash

# Floating content-addressed derivations using the Git tree-hashing
# ingestion method (`outputHashMode = "git"`).
#
# The `git` CA method (Xp::GitHashing) is exercised today only by
# `nix hash --mode git`, `nix store add --mode git`, and *fixed*-output
# git derivations (tests/functional/git-hashing/). The combination of a
# *floating* CA output (Xp::CaDerivations, hash computed post-build) with
# the `git` method had no functional coverage, even though the shared
# post-build CA-computation routine implements it
# (src/libstore/unix/build/derivation-builder.cc, the
# `FileIngestionMethod::Git` arm of `newInfoFromCA`). This test pins that
# combination so a regression in the git arm of floating-CA computation
# is caught.

source common.sh

# Needs both: floating CA outputs, and the git hashing method.
requireDaemonNewerThan "2.34.0pre20251217"
enableFeatures "ca-derivations git-hashing"
TODO_NixOS
restartDaemon

# --- Build a floating + git, single-file output. -----------------------
fileOut=$(nix build -f ./floating-git.nix rootFile --no-link --print-out-paths)
[[ -n "$fileOut" ]]
[[ "$(cat "$fileOut")" == "hello-floating-git" ]]

# Its content address must be a git-method address...
fileCA=$(nix path-info --json "$fileOut" | jq -r '.[].ca')
echo "rootFile ca: $fileCA"
[[ "$fileCA" == fixed:git:sha1:* ]]

# ...and that CA hash must equal an INDEPENDENT git hash of the built
# output (self-consistency: the ca IS the git OID of the contents, not
# some unrelated value). This is the non-vacuity guard — if the git arm
# of newInfoFromCA computed the wrong thing, these would differ.
fileCAHash=${fileCA#fixed:git:sha1:}
fileIndep=$(nix hash path --mode git --type sha1 --format nix32 "$fileOut")
fileIndep=${fileIndep#sha1:}
echo "rootFile ca-hash=$fileCAHash  independent-git-hash=$fileIndep"
[[ "$fileCAHash" == "$fileIndep" ]]

# A floating-CA output is a *built* output: it has a deriver and a
# (separately computed) NAR hash alongside its git CA.
nix path-info --json "$fileOut" | jq -e '.[].deriver != null'
nix path-info --json "$fileOut" | jq -e '.[].narHash | startswith("sha256-")'

# --- Build a floating + git, directory-TREE output. --------------------
# This is the source-tree-relevant shape: identity is the git *tree* OID.
treeOut=$(nix build -f ./floating-git.nix rootTree --no-link --print-out-paths)
[[ "$(cat "$treeOut/a")" == "hello" ]]
[[ "$(cat "$treeOut/sub/b")" == "world" ]]

treeCA=$(nix path-info --json "$treeOut" | jq -r '.[].ca')
[[ "$treeCA" == fixed:git:sha1:* ]]
treeCAHash=${treeCA#fixed:git:sha1:}
treeIndep=$(nix hash path --mode git --type sha1 --format nix32 "$treeOut")
treeIndep=${treeIndep#sha1:}
echo "rootTree ca-hash=$treeCAHash  independent-git-hash=$treeIndep"
[[ "$treeCAHash" == "$treeIndep" ]]

# --- Content-addressing property: identical contents => identical path. -
# rootTreeTwin builds byte-identical contents via different commands; the
# git tree OID (hence the store path) must match rootTree exactly.
twinOut=$(nix build -f ./floating-git.nix rootTreeTwin --no-link --print-out-paths)
echo "rootTree    => $treeOut"
echo "rootTreeTwin => $twinOut"
[[ "$treeOut" == "$twinOut" ]]

# --- Determinism: re-evaluating with a different seed (which perturbs
# the .drv but not the output contents) yields the same output path,
# the defining property of content addressing. -------------------------
treeOut2=$(nix build -f ./floating-git.nix rootTree --no-link --print-out-paths --arg seed 1)
[[ "$treeOut" == "$treeOut2" ]]

echo "floating-git CA derivations: OK"
