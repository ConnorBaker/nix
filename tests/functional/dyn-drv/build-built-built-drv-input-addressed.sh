#!/usr/bin/env bash

source common.sh

# Build an output of a derivation two dynamic layers below an
# input-addressed derivation. The path of the derivation in the middle
# layer is known before it is built, but it cannot be read until it is.

# The daemon must have dyn-drv-ia-nested's fix; 2.35 fails with "path
# ...drv is not valid".
requireDaemonNewerThan "2.36pre"

out1=$(nix build -f ./text-hashed-output.nix hello --no-link --print-out-paths)

clearStore

drvDep=$(nix-instantiate ./text-hashed-output.nix -A producingProducingDrvInputAddressed)

out2=$(nix build "${drvDep}^out^out^out" --no-link --print-out-paths)

test "$out1" == "$out2"
