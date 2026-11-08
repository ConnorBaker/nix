#!/usr/bin/env bash

source common.sh

# builder-rpc-v0
requireDaemonNewerThan "2.36pre"

TODO_NixOS

enableFeatures 'dynamic-derivations ca-derivations'
restartDaemon

NIX_BIN_DIR="$(dirname "$(type -p nix)")"
export NIX_BIN_DIR

expectStderr 1 nix build -L --file ./submit-failure.nix noSubmit --no-link | grepQuiet "failed to submit output"
expectStderr 1 nix build -L --file ./submit-failure.nix duplicate --no-link | grepQuiet "submit duplicate output"
expectStderr 1 nix build -L --file ./submit-failure.nix notAdded --no-link | grepQuiet "submitted path '.*-not-added' for output 'out', but that path is not valid"
expectStderr 1 nix build -L --file ./submit-failure.nix fixedInputAddressed --no-link | grepQuiet "error: content-addressing derivation '.*' output 'out' (at '.*-input-addressed') was not content-addressed"
expectStderr 1 nix build -L --file ./submit-failure.nix fixedText --no-link | grepQuiet "was hashed with method 'text', expected 'flat'"
