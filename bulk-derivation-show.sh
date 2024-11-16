#!/usr/bin/env bash

FLAKE_REF="github:NixOS/nixpkgs/19d66fab291f90ce56d0479b128cc7a5271bf666"
ATTR_PATH="legacyPackages.x86_64-linux"
NIX_SEARCH_EVAL_STATS_PATH="$PWD/nix-search-eval-stats.json"
NIX_DERIVATION_SHOW_EVAL_STATS_PATH="$PWD/nix-derivation-show-eval-stats.json"

sudo NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="${NIX_SEARCH_EVAL_STATS_PATH}" GC_DONT_GC=1 ./result/bin/nix search "${FLAKE_REF}#${ATTR_PATH}" ^ --json --no-eval-cache \
| jq -cr 'keys | .[]' \
| sudo NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="${NIX_DERIVATION_SHOW_EVAL_STATS_PATH}" GC_DONT_GC=1 ./result/bin/nix derivation show --no-eval-cache --expr "builtins.getFlake \"${FLAKE_REF}\"" --keep-going --stdin \
> out.json