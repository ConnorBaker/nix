#!/usr/bin/env bash

source common.sh

TODO_NixOS

# Using `--eval-store` with the daemon will eventually copy everything
# to the build store, invalidating most of the tests here
# shellcheck disable=SC1111
needLocalStore "“--eval-store” doesn't achieve much with the daemon"

eval_store=$TEST_ROOT/eval-store

# Commands that inspect derivations must read metadata from the evaluation
# store without first realizing or copying the derivations to the build store.
drv=$(nix-instantiate dependencies.nix --eval-store "$eval_store")
(! test -e "$NIX_STORE_DIR/$(basename "$drv")")
nix derivation show --eval-store "$eval_store" -f dependencies.nix |
    jq --exit-status --arg drv "$(basename "$drv")" '.derivations[$drv] != null'
nix derivation show --recursive --eval-store "$eval_store" -f dependencies.nix |
    jq --exit-status '.derivations | length > 1'
nix path-info --derivation --eval-store "$eval_store" -f dependencies.nix |
    grep -Fx "$drv"
nix path-info --derivation --json --json-format 1 --eval-store "$eval_store" -f dependencies.nix |
    jq --exit-status --arg drv "$drv" '.[$drv] != null'

# The build store has not seen the .drv files yet. queryMissing must read
# them from the eval store instead of reporting them as unknown.
out=$(nix build -f dependencies.nix --eval-store "$eval_store" --dry-run 2>&1)
[[ $out != *"don't know how to build"* ]]
[[ $out == *"will be built"* ]]

# RemoteStore must not query the build store for a derivation that only exists
# in the evaluation store. The builder copies it after the missing-path query.
# Building through such a store needs the sandbox: see eval-store-remote.sh.
remote_store="ssh-ng://localhost?remote-store=$TEST_ROOT/remote-build-store"
out=$(nix build -f dependencies.nix --eval-store "$eval_store" --store "$remote_store" --dry-run 2>&1)
[[ $out != *"don't know how to build"* ]]
[[ $out == *"will be built"* ]]

nix build -f dependencies.nix --eval-store "$eval_store" -o "$TEST_ROOT/result"
[[ -e $TEST_ROOT/result/foobar ]]
if [[ -z "${NIX_TESTS_CA_BY_DEFAULT:-}" ]]; then
    # Resolved CA derivations are written to store for building
    #
    # TODO when we something more systematic
    # (https://github.com/NixOS/nix/issues/5025) that distinguishes
    # between scratch storage for building and the final destination
    # store, we'll be able to make this unconditional again -- resolved
    # derivations should only appear in the scratch store.
    (! ls "$NIX_STORE_DIR"/*.drv)
fi
ls "$eval_store"/nix/store/*.drv

clearStore
rm -rf "$eval_store"

nix-instantiate dependencies.nix --eval-store "$eval_store"
(! ls "$NIX_STORE_DIR"/*.drv)
ls "$eval_store"/nix/store/*.drv

clearStore
rm -rf "$eval_store"

nix-build dependencies.nix --eval-store "$eval_store" -o "$TEST_ROOT/result"
[[ -e $TEST_ROOT/result/foobar ]]
if [[ -z "${NIX_TESTS_CA_BY_DEFAULT:-}" ]]; then
    # See above
    (! ls "$NIX_STORE_DIR"/*.drv)
fi
ls "$eval_store"/nix/store/*.drv

clearStore
rm -rf "$eval_store"

# Confirm that import-from-derivation builds on the build store
[[ $(nix eval --eval-store "$eval_store?require-sigs=false" --impure --raw --file ./ifd.nix) = hi ]]
ls "$NIX_STORE_DIR"/*dependencies-top/foobar
(! ls "$eval_store"/nix/store/*dependencies-top/foobar)

clearStore
rm -rf "$eval_store"

[[ $(nix develop -f ./shell.nix shellDrv --eval-store "$eval_store" -c printenv VAR_FROM_NIX) = bar ]]

# Can't write .drv by default
(! nix-instantiate dependencies.nix --eval-store "dummy://")
nix-instantiate dependencies.nix --eval-store "dummy://?read-only=false"

clearStore
rm -rf "$eval_store"

# The eval-store setting stands in for an absent --eval-store...
nix-instantiate dependencies.nix --option eval-store "$eval_store"
(! ls "$NIX_STORE_DIR"/*.drv)
ls "$eval_store"/nix/store/*.drv
(! nix-instantiate dependencies.nix --option eval-store "dummy://")
# ...which takes precedence over it.
nix-instantiate dependencies.nix --option eval-store "dummy://" --eval-store "dummy://?read-only=false"

clearStore
rm -rf "$eval_store"

nix eval --raw --option eval-store "$eval_store" -f dependencies.nix drvPath
(! ls "$NIX_STORE_DIR"/*.drv)
ls "$eval_store"/nix/store/*.drv
