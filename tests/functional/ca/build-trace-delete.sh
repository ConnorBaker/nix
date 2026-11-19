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

# Only lib gone: the rebuild does not reproduce out, which is still valid. The traces of all outputs
# are replaced by those of the rebuild, so that they name outputs of one build; the old out stays.
traced() { nix store build-trace info "$1^$2" --json | jq -r '.[] | .opaquePath | select(.)'; }
nix-build ./nondeterministic-ns.nix -A multiOut --no-out-link
multiOutPath=$(traced "$multiOutput" out) multiLibPath=$(traced "$multiOutput" lib) multiDevPath=$(traced "$multiOutput" dev)
nix-store --delete "$multiLibPath"
nix-build ./nondeterministic-ns.nix -A multiOut.lib --no-out-link
[[ $(traced "$multiOutput" out) != "$multiOutPath" && $(traced "$multiOutput" lib) != "$multiLibPath" ]]
[[ $(traced "$multiOutput" dev) == "$multiDevPath" ]]
[[ -e $(traced "$multiOutput" out) && -e $(traced "$multiOutput" lib) && -e $multiOutPath ]]

# A trace alone, as copied here, replaces no stale one: its siblings may be of another build.
# The command to start over names all of them.
nix-build ./nondeterministic-ns.nix -A multiOut --no-out-link
nix copy --to "file://$TEST_ROOT/bt-cache" -f ./nondeterministic-ns.nix multiOut
nix-store --delete "$(traced "$multiOutput" out)" "$(traced "$multiOutput" lib)" "$(traced "$multiOutput" dev)"
nix-build ./nondeterministic-ns.nix -A multiOut --no-out-link
nix-store --delete "$(traced "$multiOutput" out)"
expectStderr 1 nix copy --no-check-sigs --from "file://$TEST_ROOT/bt-cache" -f ./nondeterministic-ns.nix 'multiOut^out' > "$TEST_ROOT/bt-log"
# Through a daemon, only who may write to the store can delete its traces: the hint says so.
grepQuiet "as a user that may write to this store" "$TEST_ROOT/bt-log"
cmd=$(grep -o "nix store build-trace delete [^;]*" "$TEST_ROOT/bt-log")
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

# One id can name several outputs
nix store build-trace delete "$multiOutput"^dev
nix-build ./nondeterministic-ns.nix -A multiOut --no-out-link
nix store build-trace delete "$multiOutput"^out,lib
expect 1 nix store build-trace info "$multiOutput"^out
expect 1 nix store build-trace info "$multiOutput"^lib
nix store build-trace info "$multiOutput"^dev

# '^*' is refused
expectStderr 1 nix store build-trace delete "$multiOutput"^'*' | grepQuiet -F "must name each output after '^' ('^*' is not accepted)"

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

# Only out gone, and a user of out and lib built alone: the rebuild of multiOut answers for both,
# so that the user is built with outputs of one build, those that the traces name.
nix-build ./nondeterministic-ns.nix -A multiOut --no-out-link
nix-store --delete "$(traced "$multiOutput" out)"
userPath=$(nix build -f ./nondeterministic-ns.nix multiOutUser --no-link --print-out-paths)
[[ $(cat "$userPath") == "$(traced "$multiOutput" out) $(traced "$multiOutput" lib)" ]] \
    || fail "multiOutUser was built with outputs of two builds of multiOut: $(cat "$userPath")"
