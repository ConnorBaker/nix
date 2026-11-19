#!/usr/bin/env bash

source common.sh

TODO_NixOS

needLocalStore "'--no-require-sigs' can’t be used with the daemon"

clearStore
clearBinaryCache

drvOf() {
    nix-instantiate ./query-missing.nix -A "$1"
}

section() {
    # The paths listed under the heading that matches $1.
    echo "$out" | awk -v h="$1" '/^[^ ]/ { on = ($0 ~ h) } on && /^  / { print $1 }' | sort
}

# Only the wanted output is asked about: `partial^out` is fetched,
# though `partial.dev` is in no binary cache. (Without Internet access,
# `nix` substitutes only when told to.)
nix copy --to "file://$cacheDir" --file ./query-missing.nix 'partial^out'
clearStore
out=$(nix build --dry-run "$(drvOf partial)^out" \
    --substituters "file://$cacheDir" --no-require-sigs --option substitute true 2>&1)
echo "$out"
[[ -z $(section "will be built") && $(section "will be fetched") == *-query-missing-partial ]]

# A binary cache as the store, whose narinfo for `valid`'s
# output is corrupt: the narinfo exists, so the output is valid, as
# `isValidPath` says, and nothing is missing.
validDrv=$(drvOf valid)
validOut=$(nix-build ./query-missing.nix -A valid --no-out-link)
nix copy --to "file://$cacheDir" --derivation "$validDrv"
nix copy --to "file://$cacheDir" --file ./query-missing.nix valid
echo corrupt > "$cacheDir/$(basename "$validOut" | cut -c1-32).narinfo"
out=$(nix-store --store "file://$cacheDir" --realise --dry-run "$validDrv" 2>&1)
echo "$out"
[[ $out != *"will be"* && $out != *"don't know"* ]]
