#!/usr/bin/env bash

source common.sh

helloDrv=$(nix-instantiate ./gc-built-drv.nix -A hello)
producer=$(nix-instantiate ./gc-built-drv.nix -A producer)
consumer=$(nix-instantiate ./gc-built-drv.nix -A consumer)

echo "S3: CA consumer of an output of a dynamic derivation whose .drv was collected"
c1=$(nix build "${consumer}^out" --no-link --print-out-paths)
grep hello "$c1"
inner=$(nix build "${producer}^out" --no-link --print-out-paths)
[[ "$inner" == "$helloDrv" ]]
# Its output stays: the resolved consumer derivation refers to it.
nix-store --delete "$c1" "$inner"
expect 1 nix path-info "$inner"
c2=$(nix build "${consumer}^out" --no-link --print-out-paths)
[[ "$c2" == "$c1" ]]
echo "S3 ok"
