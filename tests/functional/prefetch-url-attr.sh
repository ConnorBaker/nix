#!/usr/bin/env bash

# `nix-prefetch-url --attr` reads the selected fetcher call's `name`
# attribute.  The condition guarding that read was inverted on master:
# with the attribute present the path was named after the URL, and without
# it the command dereferenced a null pointer.  This test fails on the
# reference for that reason and passes on the branch (doc/lazy-store/
# 04-derivation.md, section 4.3, master's gaps).

source common.sh

clearStoreIfPossible

echo payload > "$TEST_ROOT/payload.txt"
cat > "$TEST_ROOT/prefetch.nix" <<NIX
{
  withName = { urls = [ "file://$TEST_ROOT/payload.txt" ]; name = "custom-name"; outputHashMode = "flat"; };
  noName = { urls = [ "file://$TEST_ROOT/payload.txt" ]; outputHashMode = "flat"; };
}
NIX

# The `name` attribute names the store path.
path=$(nix-prefetch-url --print-path -A withName "$TEST_ROOT/prefetch.nix" | tail -n1)
[[ $path == *-custom-name ]]
[[ $(cat "$path") == payload ]]

# Without it, the URL's base name does, and the command does not crash.
path=$(nix-prefetch-url --print-path -A noName "$TEST_ROOT/prefetch.nix" | tail -n1)
[[ $path == *-payload.txt ]]

# An explicit --name wins over the attribute.
path=$(nix-prefetch-url --print-path --name explicit -A withName "$TEST_ROOT/prefetch.nix" | tail -n1)
[[ $path == *-explicit ]]
