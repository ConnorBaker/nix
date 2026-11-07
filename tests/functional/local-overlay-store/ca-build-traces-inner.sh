#!/usr/bin/env bash

# The lower store lost bin of x, not reproducible, but kept out: rebuilding x in the overlay
# store is refused, as in a plain store.

set -eu -o pipefail

source common.sh

unset NIX_STORE_DIR
unset NIX_STATE_DIR

setupStoreDirs

# shellcheck disable=SC2016
expr='with import ../config.nix; { x = derivation { name = "x"; inherit system; builder = '"$busybox"';
  outputs = [ "bin" "out" ]; __contentAddressed = true; outputHashMode = "recursive"; outputHashAlgo = "sha256";
  args = [ "sh" "-c" "read -r r < /proc/sys/kernel/random/uuid; echo $r > $bin; echo $r > $out" ]; }; }'
nix build --store "$storeA" --no-link --impure --expr "$expr" 'x^*'
drv=$(nix eval --store "$storeA" --impure --raw --expr "($expr).x.drvPath")
bin=$(nix store build-trace info --store "$storeA" "$drv^bin" --json | jq -r '.[] | .opaquePath | select(.)')
nix store delete --store "$storeA" "$bin"

mountOverlayfs

expectStderr 1 nix build --store "$storeB" --no-link --impure --expr "$expr" 'x^bin' | grepQuiet "build-trace delete"
