#!/usr/bin/env bash

# A non-reproducible CA derivation built remotely builds again once both the
# local store and the builder have deleted its output: the new output
# replaces the stale build traces and is copied back. For a reproducible
# one, see build-remote-content-addressed-cache-loss.sh.

source common.sh

enableFeatures "ca-derivations"
requireSandboxSupport
requiresUnprivilegedUserNamespaces
[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"
unset NIX_STORE_DIR

store="$TEST_ROOT/machine0"
builder="$TEST_ROOT/machine1"
builders="ssh-ng://localhost?remote-store=$builder?system-features=ca-derivations - - 1 1 ca-derivations"
# shellcheck disable=SC2016
expr='with import ./config.nix; derivation {
  name = "remote-nondeterministic";
  inherit system;
  builder = "'"$busybox"'";
  args = [ "sh" "-c" "read -r x < /proc/sys/kernel/random/uuid; echo $x > $out" ];
  __contentAddressed = true;
  outputHashMode = "recursive";
  outputHashAlgo = "sha256";
}'
build () {
    nix build -L --no-link --print-out-paths --impure --expr "$expr" --max-jobs 0 \
        --store "$store" --builders "$builders"
}

first=$(build)
nix store delete --store "$store" "$first"
nix store delete --store "$builder" "$first"
second=$(build)
[[ $second != "$first" ]]
nix path-info --store "$store" "$second"
