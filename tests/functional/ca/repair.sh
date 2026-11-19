#!/usr/bin/env bash

source common.sh

needLocalStore "--repair needs a local store"

clearStore

# A repair rebuilds an output whose path is already valid, made by another
# derivation with the same output, and replaces it.
nix-build ./content-addressed.nix -A dependentCA --arg seed 1 --no-out-link
nix-build ./content-addressed.nix -A dependentCA --arg seed 2 --no-out-link --repair
