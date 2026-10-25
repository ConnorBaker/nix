#!/usr/bin/env bash

source common.sh

# Builds with a separate evaluation store, in stores at other locations
# (`?remote-store=...`). Building there needs the sandbox, so set up as
# build-remote.sh does.
requireSandboxSupport
requiresUnprivilegedUserNamespaces
[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

# Avoid store dir being inside sandbox build-dir
unset NIX_STORE_DIR

file=build-hook.nix
eval_store=$TEST_ROOT/eval-store
remote_root=$TEST_ROOT/eval-store-remote
remote_store="ssh-ng://localhost?remote-store=$remote_root?system-features=foo%20bar%20baz"

chmod -R +w "$TEST_ROOT/eval-store"* || true
rm -rf "$TEST_ROOT/eval-store"*

# RemoteStore must not query the build store for a derivation that only exists
# in the evaluation store. The builder copies it after the missing-path query.
out=$(
    nix build -f "$file" --arg busybox "$busybox" --eval-store "$eval_store" --store "$remote_store" \
        --no-link --print-out-paths
)
grep 'FOO BAR BAZ' "$remote_root/$out"
