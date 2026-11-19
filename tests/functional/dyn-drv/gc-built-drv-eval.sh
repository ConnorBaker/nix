#!/usr/bin/env bash

source common.sh

helloDrv=$(nix-instantiate ./gc-built-drv.nix -A hello)
producer=$(nix-instantiate ./gc-built-drv.nix -A producer)

# Context producer^out^out: getContext names the derivation producer^out.
# The producer is named by its path, so evaluation does not write hello.drv again.
expr="builtins.attrNames (builtins.getContext (builtins.outputOf (builtins.outputOf (builtins.storePath \"$producer\") \"out\") \"out\"))"

echo "S4: getContext names the inner .drv from the realisation"
out1=$(nix build "${producer}^out^out" --no-link --print-out-paths)
inner=$(nix build "${producer}^out" --no-link --print-out-paths)
[[ "$inner" == "$helloDrv" ]]
before=$(nix eval --impure --json --expr "$expr")
echo "before: $before"
[[ "$before" == "[\"$helloDrv\"]" ]]

nix-store --delete "$out1" "$inner"
expect 1 nix path-info "$inner"
set +e
after=$(nix eval --impure --json --expr "$expr" 2> "$TEST_ROOT/s4.err")
rc=$?
set -e
cat "$TEST_ROOT/s4.err"
echo "S4 after: rc=$rc value=$after"
expect 1 nix path-info "$inner"
[[ $rc == 0 ]]
[[ "$after" == "$before" ]]
echo "S4 ok"
