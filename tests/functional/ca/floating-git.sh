#!/usr/bin/env bash

# Floating content-addressed derivations using the Git tree-hashing
# ingestion method (`outputHashMode = "git"`).
#
# The `git` CA method is exercised by `nix hash --mode git`, `nix store add
# --mode git`, and fixed-output git derivations (tests/functional/git-hashing/).
# The combination of a floating CA output, whose hash is computed after the
# build, with the `git` method had no functional coverage although the shared
# post-build routine implements it (the `FileIngestionMethod::Git` arm of
# `newInfoFromCA` in src/libstore/unix/build/derivation-builder.cc).  This
# test pins that combination.  Carried from an earlier branch, where it was
# written against the previous path-info JSON shape (doc/lazy-store/
# 04-derivation.md, §4).

source common.sh

# `ca/common.sh`, sourced above, requires a daemon with floating CA outputs,
# enables `ca-derivations`, marks the whole suite `TODO_NixOS` and restarts
# the daemon; this script adds no marker of its own.  The git method needs
# no feature.

# The content address recorded for a path must be the git method with the
# hash an independent `nix hash path --mode git` computes over the contents.
checkGitCA() {
    local path=$1
    local independent
    independent=$(nix hash path --mode git --algo sha256 --format sri "$path")
    nix path-info --json --json-format 2 "$path" | jq -e \
        --arg hashSRI "$independent" \
        '.info.[].ca == { method: "git", hash: $hashSRI }'
}

# --- The SHA-1 form is refused at instantiation. -----------------------
# The git method admits SHA-256 only (doc/lazy-store/01-specification.md,
# section 10, "The git method is SHA-256 only"); a floating output
# declares its algorithm alone,
# so the refusal is on `outputHashAlgo`.
expectStderr 1 nix-instantiate ./floating-git.nix -A rootFileSha1 \
    | grepQuiet 'the git content-address method admits SHA-256 only'

# --- Build a floating + git, single-file output. -----------------------
fileOut=$(nix build -f ./floating-git.nix rootFile --no-link --print-out-paths)
[[ -n "$fileOut" ]]
[[ "$(cat "$fileOut")" == "hello-floating-git" ]]
checkGitCA "$fileOut"

# A floating-CA output is a built output: it has a deriver and a
# separately computed NAR hash alongside its git content address.
nix path-info --json --json-format 2 "$fileOut" | jq -e '.info.[].deriver != null'
nix path-info --json --json-format 2 "$fileOut" | jq -e '.info.[].narHash | startswith("sha256-")'

# --- Build a floating + git, directory-tree output. --------------------
# The source-tree-relevant shape: identity is the git tree object.
treeOut=$(nix build -f ./floating-git.nix rootTree --no-link --print-out-paths)
[[ "$(cat "$treeOut/a")" == "hello" ]]
[[ "$(cat "$treeOut/sub/b")" == "world" ]]
checkGitCA "$treeOut"

# --- Content addressing: identical contents, identical path. -----------
# rootTreeTwin builds byte-identical contents by different commands; the
# git tree object, hence the store path, must be the same.
twinOut=$(nix build -f ./floating-git.nix rootTreeTwin --no-link --print-out-paths)
[[ "$treeOut" == "$twinOut" ]]

# --- Determinism: another seed perturbs the derivation, not the output. -
treeOut2=$(nix build -f ./floating-git.nix rootTree --no-link --print-out-paths --arg seed 1)
[[ "$treeOut" == "$treeOut2" ]]
