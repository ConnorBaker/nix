#!/usr/bin/env bash
#
# REAL-WORKLOAD benchmark for lazy-derivations — the representative companion to
# the synthetic bench-lazy-derivations.sh. It cold-instantiates real nixpkgs
# targets (deep DAGs, heavy stdenv sharing, real `mkDerivation`, real source /
# setup-hook patterns) into a FRESH chroot store, so the WHOLE `.drv` closure is
# written from scratch — the eval-store mass-instantiation shape (§2). Three
# shapes the synthetic harness can't represent:
#
#   * single package  (`hello`)            — one real closure (~218 .drvs)
#   * a package SET    (nix-eval-jobs proxy: many roots, one shared closure)
#   * a NixOS system   (container config)  — the deepest closure (~2.6k .drvs)
#
# Eager vs lazy(+overlap) vs lazy(no-overlap, via the _NIX_LAZY_DRV_NO_OVERLAP
# env escape). Reports best-of-N wall-clock AND the .drv count per mode (which
# MUST match across modes — deferral cannot change the closure). NOT a gate test.
#
#   doc/tecnix-survey/bench-lazy-nixpkgs.sh [nixpkgs-path] [reps]
#
# Representative result (best-of-3, one dev machine; speedup = eager/lazy):
#   LOCAL eval-store (eval-dominated — writes are a small fraction):
#     hello   eager 0.253  lazy 0.258-0.268  (~1.0x)
#     15 pkgs eager 1.280  lazy 1.197-1.201  (1.07x)
#     nixos   eager 2.300  lazy 2.255-2.306  (~1.0x)      → ~NEUTRAL
#   DAEMON eval-store (each write is a protocol round-trip):
#     15 pkgs eager 1.995  lazy 1.451-1.520  (1.31-1.37x)
#     nixos   eager 2.700  lazy 2.561-2.599  (1.06-1.09x; noisy, up to ~1.4x)
#   WARM LOCAL (re-eval, closure already present — the steady-state dev/CI case):
#     15 pkgs eager 1.055  lazy 1.034-1.057  (~1.0x, NEUTRAL)
#     (A warm-store re-eval was ~8% SLOWER until addPath learned to black-hole
#      already-valid .drvs at ENQUEUE for a LocalStore — see async-path-writer.cc
#      `storeIsLocal`. The warm FLAKE eval-cache case is neutral by construction:
#      a cache hit skips derivationStrict, so there is nothing for lazy to defer.)
# READ THIS HONESTLY: the eval-store mass-instantiation win is REAL for a DAEMON
# eval-store (round-trip coalescing, 1.1-1.4x, scaling with closure size) but
# ~NEUTRAL for a LOCAL eval-store, because real nixpkgs/NixOS eval is dominated
# by EVALUATION (stdenv/mkDerivation thunk-forcing + GC), so deferring the writes
# — a small fraction — saves little. The synthetic bench-lazy-derivations.sh
# overstated the local win by making writes a disproportionate fraction. Note
# lazy+overlap ≈ lazy-noov for the daemon: the background drain is a no-op for
# remote stores (it would fragment the one framed op into per-wave round-trips —
# measured 0.86x before that fix); overlap runs only for a LocalStore.
#
set -uo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NIX="$root/build/src/nix/nix"
NIXI="$root/build/src/nix/nix-instantiate"
XP="--extra-experimental-features"
NIXPKGS="${1:-$("$NIX" eval --impure --raw --expr '(builtins.getFlake "nixpkgs").outPath' $XP "nix-command flakes" 2>/dev/null)}"
REPS="${2:-3}"
[ -e "$NIXPKGS/default.nix" ] || { echo "no nixpkgs at '$NIXPKGS'"; exit 1; }

tmp="$(mktemp -d)"; trap 'chmod -R +w "$tmp" 2>/dev/null; rm -rf "$tmp"' EXIT

RUNNER=run_one   # set to run_one_daemon / run_warm for other eval-store axes
WARMSTORE=""     # a pre-warmed persistent LocalStore (set by the warm scenario)

run_one() {  # LOCAL eval-store. $1 env-assignments, rest = instantiate args -> "time drvs"
    local env="$1"; shift
    local fresh="$tmp/s-$RANDOM$RANDOM" t0 t1 n
    t0=$(date +%s.%N)
    env $env "$NIXI" "$@" --store "local?root=$fresh" >/dev/null 2>&1
    t1=$(date +%s.%N)
    n=$(ls "$fresh/nix/store/"*.drv 2>/dev/null | wc -l)
    chmod -R +w "$fresh" 2>/dev/null; rm -rf "$fresh"
    echo "$(echo "$t1 - $t0" | bc -l) $n"
}

run_one_daemon() {  # DAEMON eval-store (the nix-eval-jobs-to-daemon shape).
    local env="$1"; shift
    local d="$tmp/d-$RANDOM$RANDOM" sock t0 t1 dpid n
    sock="$d/sock"; mkdir -p "$d/store" "$d/var" "$d/etc"
    NIX_STORE_DIR="$d/store" NIX_STATE_DIR="$d/var" NIX_LOG_DIR="$d/var/log" NIX_CONF_DIR="$d/etc" \
      NIX_DAEMON_SOCKET_PATH="$sock" "$NIX" $XP "nix-command" daemon >/dev/null 2>&1 &
    dpid=$!; for i in $(seq 1 100); do [ -S "$sock" ] && break; sleep 0.05; done
    t0=$(date +%s.%N)
    env $env NIX_STORE_DIR="$d/store" NIX_STATE_DIR="$d/var" NIX_CONF_DIR="$d/etc" NIX_REMOTE="unix://$sock" \
      "$NIXI" "$@" >/dev/null 2>&1
    t1=$(date +%s.%N)
    n=$(ls "$d/store/"*.drv 2>/dev/null | wc -l)
    kill -9 "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null
    chmod -R +w "$d" 2>/dev/null; rm -rf "$d"
    echo "$(echo "$t1 - $t0" | bc -l) $n"
}

run_warm() {  # WARM LocalStore re-eval: $WARMSTORE already holds the closure (writes skipped)
    local env="$1"; shift
    local t0 t1 n
    t0=$(date +%s.%N)
    env $env "$NIXI" "$@" --store "local?root=$WARMSTORE" >/dev/null 2>&1
    t1=$(date +%s.%N)
    n=$(ls "$WARMSTORE/nix/store/"*.drv 2>/dev/null | wc -l)
    echo "$(echo "$t1 - $t0" | bc -l) $n"
}

best_of() {  # $1 env, rest = args -> "besttime drvs"
    local env="$1"; shift
    local best="" drvs t n
    for ((r = 0; r < REPS; r++)); do
        read -r t n < <("$RUNNER" "$env" "$@")
        drvs=$n
        if [ -z "$best" ] || (( $(echo "$t < $best" | bc -l) )); then best=$t; fi
    done
    echo "$best $drvs"
}

bench() {  # $1 label, rest = instantiate args
    local label="$1"; shift
    local et en lt ln nt nn
    read -r et en < <(best_of "" "$@")
    read -r lt ln < <(best_of "" --option lazy-derivations true "$@")
    read -r nt nn < <(best_of "_NIX_LAZY_DRV_NO_OVERLAP=1" --option lazy-derivations true "$@")
    printf '%-26s eager=%.3fs(%s)  lazy+ov=%.3fs(%s,%.2fx)  lazy-noov=%.3fs(%s,%.2fx)\n' \
        "$label" "$et" "$en" \
        "$lt" "$ln" "$(echo "$et/$lt" | bc -l)" \
        "$nt" "$nn" "$(echo "$et/$nt" | bc -l)"
    [ "$en" = "$ln" ] && [ "$en" = "$nn" ] || echo "  !! drv counts differ across modes ($en/$ln/$nn) — deferral changed the closure!"
}

SET=(-A hello -A git -A python3 -A gnumake -A curl -A openssl -A coreutils -A bash -A vim \
     -A ripgrep -A jq -A cmake -A nodejs -A rustc -A go)
NIXOSARGS=(-A system --arg configuration '{ boot.isContainer = true; system.stateVersion = "24.11"; }')

echo "=== real-nixpkgs COLD mass-instantiation: best-of-$REPS, speedup=eager/lazy ==="
echo "nixpkgs: $NIXPKGS"
echo "--- LOCAL eval-store (eval-dominated; writes are a small fraction) ---"
RUNNER=run_one
bench "hello (1 pkg)"       "$NIXPKGS" -A hello
bench "15 pkgs (eval-jobs)" "$NIXPKGS" "${SET[@]}"
bench "nixos system"        "$NIXPKGS/nixos" "${NIXOSARGS[@]}"
echo "--- DAEMON eval-store (each write is a protocol round-trip) ---"
RUNNER=run_one_daemon
bench "15 pkgs (eval-jobs)" "$NIXPKGS" "${SET[@]}"
bench "nixos system"        "$NIXPKGS/nixos" "${NIXOSARGS[@]}"
echo "--- WARM LOCAL eval-store (re-eval; the closure is already present, writes skipped) ---"
WARMSTORE="$tmp/warm"; mkdir -p "$WARMSTORE"
"$NIXI" "$NIXPKGS" "${SET[@]}" --store "local?root=$WARMSTORE" >/dev/null 2>&1  # warm-up (untimed)
RUNNER=run_warm
bench "15 pkgs (warm)"      "$NIXPKGS" "${SET[@]}"
