#!/usr/bin/env bash

source common.sh

# An input-addressed derivation outputs the derivation of the
# content-addressed `producingDrv`, at a path other than that
# derivation's own. Building an output of that derivation and then
# looking the output up must agree on which derivation was built.

drv=$(nix-instantiate ./text-hashed-output.nix -A hello)

clearStore

drvDep=$(nix-instantiate ./text-hashed-output.nix -A producingDrvAtForeignPath)

# Store layer needs bugfix
requireDaemonNewerThan "2.37pre"

out1=$(nix build "${drvDep}^out^out" --no-link --print-out-paths)
test "$out1" == "$drv"

out2=$(nix path-info "${drvDep}^out^out")
test "$out2" == "$drv"

# Named by the path that holds it, it is the same derivation.
clearStore
drvDep=$(nix-instantiate ./text-hashed-output.nix -A producingDrvAtForeignPath)
held=$(nix build "${drvDep}^out" --no-link --print-out-paths)
nix build "${held}^out" --no-link
test "$(nix path-info "${held}^out")" == "$drv"
nix build --dry-run "${held}^out" 2>&1 | grepQuietInverse "will be built"

# An input-addressed derivation with an input derivation, held at a
# foreign path, is built as it is.
clearStore
drvDep=$(nix-instantiate ./text-hashed-output.nix -A producingIaDrvAtForeignPath)
out=$(nix build "${drvDep}^out^out" --no-link --print-out-paths)
[[ $(cat "$out/hello") == "Hello World" ]]
