#!/usr/bin/env bash

# The lower store lost bin of x, not reproducible, but kept out: rebuilding x in the overlay
# store replaces the traces of both outputs there, as in a plain store, and not in the lower store.

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

# The overlay store compares a copied trace with the lower store's: it refuses bin of another build.
storeC=$storeVolume/store-c
nix build --store "$storeC" --no-link --impure --expr "$expr" 'x^*'
expectStderr 1 nix copy --no-check-sigs --from "$storeC" --to "$storeB" "$drv^bin" \
    | grepQuiet "differs from the build traces recorded here"

out=$(nix store build-trace info --store "$storeA" "$drv^out" --json | jq -r '.[] | .opaquePath | select(.)')
nix build --store "$storeB" --no-link --impure --expr "$expr" 'x^bin'
traced() { nix store build-trace info --store "$1" "$drv^$2" --json | jq -r '.[] | .opaquePath | select(.)'; }
[[ $(traced "$storeB" out) != "$out" && $(traced "$storeB" bin) != "$bin" ]]
[[ $(traced "$storeA" out) == "$out" && $(traced "$storeA" bin) == "$bin" ]]
# One build wrote the same line to both outputs.
[[ $(cat "$storeBRoot$(traced "$storeB" out)") == $(cat "$storeBRoot$(traced "$storeB" bin)") ]]

# The overlay store's traces now differ from the lower store's, which no longer count: losing bin
# again, rebuilding x replaces them again.
bin2=$(traced "$storeB" bin) out2=$(traced "$storeB" out)
nix store delete --store "$storeB" "$bin2"
nix build --store "$storeB" --no-link --impure --expr "$expr" 'x^bin'
[[ $(traced "$storeB" bin) != "$bin2" && $(traced "$storeB" out) != "$out2" ]]
[[ $(cat "$storeBRoot$(traced "$storeB" out)") == $(cat "$storeBRoot$(traced "$storeB" bin)") ]]
