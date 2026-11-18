#!/usr/bin/env bash

source common.sh

# A derivation built at a foreign path can mention inputs that the
# store object holding it does not reference. Building it must still
# obtain those inputs, here from a binary cache.

clearStore

drvDep=$(nix-instantiate ./text-hashed-output.nix -A producingDrvWithUnreferencedInputs)
drv=$(nix-instantiate ./text-hashed-output.nix -A helloFromSource)
mapfile -t inputs < <(nix-store --query --references "$drv")
(( ${#inputs[@]} > 0 ))

cache="file://$TEST_ROOT/foreign-path-cache"
nix copy --to "$cache" "${inputs[@]}"
nix-store --delete "$drv" "${inputs[@]}"
for input in "${inputs[@]}"; do
    [[ ! -e "$input" ]]
done

# Store layer needs bugfix
requireDaemonNewerThan "2.37pre"

out=$(nix build "${drvDep}^out^out" --no-link --print-out-paths \
    --substitute --substituters "$cache" --no-require-sigs)
[[ $(cat "$out") == "Hello World" ]]

# It was stored at its own path, where a remote builder reads it from.
nix path-info "$drv"

# Once its output is valid, building it needs none of its inputs.
nix-store --delete "$drv" "${inputs[@]}"
[[ $(nix build "${drvDep}^out^out" --no-link --print-out-paths --substituters "") == "$out" ]]

# Building it again does.
expectStderr 1 nix build "${drvDep}^out^out" --no-link --rebuild --substituters "" | grepQuiet "failed to obtain the inputs"

needLocalStore "the evaluation store is a local store"

# Inputs that only the evaluation store has are copied from it.
clearStore
evalStore=$TEST_ROOT/foreign-path-eval-store
drvDep=$(nix-instantiate --eval-store "$evalStore" ./text-hashed-output.nix -A producingDrvWithUnreferencedInputs)
nix copy --from "$evalStore" "$drvDep"
out=$(nix build --eval-store "$evalStore" "${drvDep}^out^out" --no-link --print-out-paths --substituters "")
[[ $(cat "$out") == "Hello World" ]]
