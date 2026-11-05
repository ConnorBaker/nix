#!/usr/bin/env bash

# A non-reproducible CA derivation whose output was deleted builds again (NixOS/nix#15649).

source common.sh

# The first version of this fork with the fix. A date cannot say whether a
# daemon has the fix: a later upstream 2.36.0 or pre-release without
# NixOS/nix#16477 passes this guard and fails the test.
requireDaemonNewerThan "2.36.0pre20261105"

signIfNeeded

clearStore
clearBinaryCache

cache="file://$cacheDir"
drv=$(nix-instantiate ./nondeterministic-ns.nix -A singleOut)

first=$(nix-build ./nondeterministic-ns.nix -A singleOut --no-out-link)
nix copy --to "$cache" --file ./nondeterministic-ns.nix singleOut
nix-store --delete "$first"

# The build trace of the deleted path gives way to the new one.
second=$(nix-build ./nondeterministic-ns.nix -A singleOut --no-out-link)
[[ -e $second ]]
[[ $(nix store build-trace info "$drv"^out --json | jq -r '.[] | .opaquePath | select(.)') == "$second" ]]

# A trace whose output is still valid conflicts with a different one.
err=$(expectStderr 1 nix copy --from "$cache" --no-check-sigs --file ./nondeterministic-ns.nix singleOut)
grepQuiet -F "Trying to register a realisation of '$(basename "$drv")^out', but we already have another one locally." <<< "$err"
grepQuiet -F "Local:  $second" <<< "$err"
grepQuiet -F "Remote: $first" <<< "$err"

# Once that output is gone too, the copied realisation replaces its trace.
nix-store --delete "$second"
nix copy --from "$cache" --no-check-sigs --file ./nondeterministic-ns.nix singleOut
[[ $(nix store build-trace info "$drv"^out --json | jq -r '.[] | .opaquePath | select(.)') == "$first" ]]
