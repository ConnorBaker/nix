#!/usr/bin/env bash

source common.sh

set -x

needLocalStore "command 'nix store build-trace delete' can’t be used with the daemon"

clearStore

singleOutput=$(nix-instantiate ./nondeterministic-ns.nix -A singleOut)
multiOutput=$(nix-instantiate ./nondeterministic-ns.nix -A multiOut)

# First build
singleOutPath=$(nix-build ./nondeterministic-ns.nix -A singleOut --no-out-link)
nix-store --delete "$singleOutPath"
# The build trace outlives the output, and a rebuild replaces it
nix-build ./nondeterministic-ns.nix -A singleOut --no-out-link
nix store build-trace delete "$singleOutput"^out
nix-build ./nondeterministic-ns.nix -A singleOut --no-out-link

# Only lib gone: the rebuild does not reproduce out, which is still valid, so it registers nothing
traced() { nix store build-trace info "$1^$2" --json | jq -r '.[] | .opaquePath | select(.)'; }
nix-build ./nondeterministic-ns.nix -A multiOut --no-out-link
multiOutPath=$(traced "$multiOutput" out) multiLibPath=$(traced "$multiOutput" lib)
nix-store --delete "$multiLibPath"
cmd=$(expectStderr 1 nix-build ./nondeterministic-ns.nix -A multiOut.lib --no-out-link | grep -o "nix store build-trace delete [^;]*")
[[ $(traced "$multiOutput" out) == "$multiOutPath" && $(traced "$multiOutput" lib) == "$multiLibPath" ]]
# As the error says, start over
eval "$cmd"

# A trace alone, as copied here, replaces no stale one: its siblings may be of another build.
# The command to start over names all of them.
nix-build ./nondeterministic-ns.nix -A multiOut --no-out-link
nix copy --to "file://$TEST_ROOT/bt-cache" -f ./nondeterministic-ns.nix multiOut
nix-store --delete "$(traced "$multiOutput" out)" "$(traced "$multiOutput" lib)" "$(traced "$multiOutput" dev)"
nix-build ./nondeterministic-ns.nix -A multiOut --no-out-link
nix-store --delete "$(traced "$multiOutput" out)"
cmd=$(expectStderr 1 nix copy --no-check-sigs --from "file://$TEST_ROOT/bt-cache" -f ./nondeterministic-ns.nix 'multiOut^out' | grep -o "nix store build-trace delete [^;]*")
eval "$cmd"
expect 1 nix store build-trace info "$multiOutput"^lib

# Multi-output first
nix-build ./nondeterministic-ns.nix -A multiOut --no-out-link
multiOutPath=$(nix store build-trace info "$multiOutput"^out --json | jq -r '.[] | .opaquePath | select(.)')
multiLibPath=$(nix store build-trace info "$multiOutput"^lib --json | jq -r '.[] | .opaquePath | select(.)')

# We should be able to delete multiple build traces/realisations at once
nix-store --delete "$multiOutPath" "$multiLibPath"
nix store build-trace delete "$multiOutput"^out "$multiOutput"^lib

# out and lib should be deleted, but dev should not
expect 1 nix store build-trace info "$multiOutput"^out
expect 1 nix store build-trace info "$multiOutput"^lib
nix store build-trace info "$multiOutput"^dev

# A repair rebuilds an output whose path is already valid, made by another derivation with the
# same output, and replaces it.
nix-build ./content-addressed.nix -A dependentCA --arg seed 1 --no-out-link
nix-build ./content-addressed.nix -A dependentCA --arg seed 2 --no-out-link --repair

# Only bin gone: the rebuild reproduces out, which keeps its trace and signatures
partly=$(nix-instantiate ./nondeterministic-ns.nix -A partlyReproducible)
nix-build ./nondeterministic-ns.nix -A partlyReproducible --no-out-link
out=$(traced "$partly" out)
nix key generate-secret --key-name test > "$TEST_ROOT/sk"
nix store sign --key-file "$TEST_ROOT/sk" "$out"
nix-store --delete "$(traced "$partly" bin)"
nix-build ./nondeterministic-ns.nix -A partlyReproducible.bin --no-out-link
[[ $(traced "$partly" out) == "$out" ]]
nix path-info --json "$out" | grepQuiet '"test:'
