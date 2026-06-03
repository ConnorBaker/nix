#!/usr/bin/env bash

# Regression — a deferred (fingerprintable) FLAKE source built over a DAEMON.
# `daemon-build.sh` covers the sourceless case; this covers the source case,
# which deadlocked before the fix: `RemoteStore::copyDrvsFromEvalStore`'s
# `copyClosure(LazyDrvStore → daemon)` pulled the pending source via
# `narFromPath`, whose override re-entered the daemon connection (materialise →
# `outPathOf` → `addToStore`) while the copy held one. Fix: for a remote build
# store, `Installable::build` flushes the deferred closure to the daemon UP FRONT
# (LD-S6) instead of the re-entrant pull. This must COMPLETE (a hang would time
# out the suite) and produce the right output; `timeout` makes a regression fail
# loudly.

source ../common.sh

requireGit
TODO_NixOS  # startDaemon unsupported on NixOS
enableFeatures "flakes"

clearStore

flake="$TEST_ROOT/flk"
createGitRepo "$flake"
mkdir -p "$flake/src"
echo daemon-src-payload > "$flake/src/payload"
cat > "$flake/flake.nix" <<'EOF'
{
  outputs = { self }: let
    src = builtins.path { path = ./src; name = "daemonsrcmarker"; };
  in {
    packages.SYSTEM.default = derivation {
      name = "daemon-src-target"; system = "SYSTEM"; builder = "/bin/sh";
      args = [ "-c" "echo daemon-src-ok > $out" "${src}" ];
    };
  };
}
EOF
sed -i "s|SYSTEM|$system|g" "$flake/flake.nix"
git -C "$flake" add -A
git -C "$flake" commit -m init

startDaemon

ref="git+file://$flake#packages.$system.default"
out=$(timeout 60 nix build --no-link --print-out-paths \
        --option eval-cache false --option lazy-derivations true "$ref") \
    || fail "lazy flake-source build over the daemon did not complete (deadlock regression?)"

nix-store --check-validity "$out" || fail "daemon source-build output is missing"
[ "$(cat "$out")" = "daemon-src-ok" ] || fail "daemon source-build output has wrong contents"

killDaemon
echo "daemon-source-build: ok (deferred flake source builds over a daemon)"
