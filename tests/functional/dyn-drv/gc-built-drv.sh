#!/usr/bin/env bash

source common.sh

buggyNeedLocalStore "a path with two dynamic layers does not survive the trip to the daemon"

helloDrv=$(nix-instantiate ./gc-built-drv.nix -A hello)
producer=$(nix-instantiate ./gc-built-drv.nix -A producer)
meta=$(nix-instantiate ./gc-built-drv.nix -A meta)

echo "S0: build everything once"
out1=$(nix build "${producer}^out^out" --no-link --print-out-paths)
inner=$(nix build "${producer}^out" --no-link --print-out-paths)
[[ "$inner" == "$helloDrv" ]]
mid=$(nix build "${meta}^out" --no-link --print-out-paths)
[[ "$mid" == "$producer" ]]
out3=$(nix build "${meta}^out^out^out" --no-link --print-out-paths)
[[ "$out3" == "$out1" ]]

echo "S1: two levels, inner .drv collected, realisation of producer^out kept"
nix-store --delete "$out1" "$inner"
expect 1 nix path-info "$inner"
out2=$(nix build "${producer}^out^out" --no-link --print-out-paths)
[[ "$out2" == "$out1" ]]
grep hello "$out2/hello"
echo "S1 ok"

echo "S2: three levels, middle .drv collected, realisation of meta^out kept"
nix-store --delete "$out1" "$inner"
nix-store --delete "$mid"
expect 1 nix path-info "$mid"
out4=$(nix build "${meta}^out^out^out" --no-link --print-out-paths)
[[ "$out4" == "$out1" ]]
nix path-info "$mid"
echo "S2 ok"
