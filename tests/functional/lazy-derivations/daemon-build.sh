#!/usr/bin/env bash

# Regression — lazy-derivations against a DAEMON build store (LD-S6, remote
# regime). Selective elision makes the build's `evalStore` a client-side
# `LazyDrvStore` overlay; for a remote daemon the daemon's Worker cannot see the
# client queue, so `RemoteStore::copyDrvsFromEvalStore` must ship the deferred
# `.drv` closure to the daemon first. That `copyClosure` reads each pending
# `.drv` via `narFromPath` — which `LazyDrvStore` now serves from the queue
# (`dumpString(contents)`, no round-trip). Before that override this HUNG at
# "copying path …drv to daemon". Here we assert the build COMPLETES; `timeout`
# makes a hang regression fail loudly rather than stall the whole suite.

source ../common.sh

TODO_NixOS  # startDaemon is unsupported on NixOS

clearStore
startDaemon
export NIX_PATH=config="${config_nix}"

cat > "$TEST_ROOT/d.nix" <<'EOF'
with import <config>;
mkDerivation { name = "daemon-lazy"; buildCommand = "echo daemon-lazy-ok > $out"; }
EOF

# Build from source against the daemon with lazy-derivations on.
out=$(timeout 60 nix build --no-link --print-out-paths \
    --option lazy-derivations true -f "$TEST_ROOT/d.nix") \
    || fail "lazy-derivations build against the daemon did not complete (hang regression?)"

nix-store --check-validity "$out" || fail "daemon-built output is missing"
[ "$(cat "$out")" = "daemon-lazy-ok" ] || fail "daemon-built output has wrong contents"

killDaemon
echo "daemon-build: ok (lazy build completes against a daemon)"
