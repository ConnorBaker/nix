#!/usr/bin/env bash

# --check of a builder-rpc-v0 derivation compares the submitted output with
# the recorded build trace, and does not replace it.

source common.sh

# builder-rpc-v0
requireDaemonNewerThan "2.36pre"

TODO_NixOS

enableFeatures 'dynamic-derivations ca-derivations'
restartDaemon

NIX_BIN_DIR="$(dirname "$(type -p nix)")"
export NIX_BIN_DIR

traced() {
    nix store build-trace info "$(nix eval --raw -f ./check-submitted.nix --arg random "$1" drvPath)^out" --json \
        | jq -r '.[] | .opaquePath | select(.)'
}

# Not reproducible: the check fails, and the trace stays.
out=$(nix build -L -f ./check-submitted.nix --arg random true --no-link --print-out-paths)
[[ $(traced true) == "$out" ]]
expectStderr 1 nix build -L -f ./check-submitted.nix --arg random true --no-link --rebuild \
    | grepQuiet "may not be deterministic"
[[ $(traced true) == "$out" ]]

# Reproducible: the check passes.
out=$(nix build -L -f ./check-submitted.nix --arg random false --no-link --print-out-paths)
nix build -L -f ./check-submitted.nix --arg random false --no-link --rebuild
[[ $(traced false) == "$out" ]]
