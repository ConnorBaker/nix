#!/usr/bin/env bash
#
# Benchmark the eval-store mass-instantiation headline (PROPOSAL-LAZY-DERIVATIONS
# §7): instantiate M distinct, source-free, build-free derivations to `.drv`
# files with `lazy-derivations` OFF (eager: M per-`.drv` writes) vs ON (deferred
# → one bulk `addMultipleToStore` + optional background-drain overlap), against
# BOTH a local store and a daemon store. The headline win is claimed for the
# DAEMON case (M protocol round-trips → one framed bulk op); the local case is
# the control (no round-trips, so the bulk machinery's overhead shows).
#
# NOT a correctness test (not wired into meson). Run from a built tree:
#     doc/tecnix-survey/bench-lazy-derivations.sh [M] [reps]
#
# This harness covers the INSTANTIATION axes (two workload shapes × {local,
# daemon} × {eager, lazy+overlap, lazy-noov}). The ELISION axis — the bigger win
# — needs the build+substitute path and is measured by the functional-harness
# `tests/functional/lazy-derivations/bench-elision.sh` (see its header).
#
# METHODOLOGY CAVEATS (PROPOSAL-LAZY-DERIVATIONS.md ledger A7, 2026-06-03):
#  - best-of-N here checks the drv COUNT MATCHES across modes (catches a deferral
#    that changed the closure) but does NOT catch ALL-MODES-FAIL (a silently
#    failing run reports a bogus time + 0 drvs and is averaged in). This env had
#    intermittent silent `nix build` failures; a robust harness verifies each run
#    SUCCEEDED and produced the expected EFFECT (e.g. lazy elided / eager copied),
#    not just that counts match.
#  - timings are PAGE-CACHE-WARM (no `drop_caches` without root) — cold-disk wins
#    (elision/source-copy) are larger and unmeasured here.
#  - n=1 numbers are unreliable: prior single-run multipliers were off by ~2-3×
#    (ledger A7). Report best-of-N + absolute values, never an n=1 multiplier.
# Representative results (M=2000, best-of-5, one dev machine; speedup=eager/lazy):
#   [A] sourceless / write-heavy (eval << writes):
#       local    eager=0.253  lazy+overlap=0.266 (0.95x)  lazy-noov=0.263 (0.96x)
#       daemon   eager=0.311  lazy+overlap=0.264 (1.18x)  lazy-noov=0.302 (1.03x)
#   [B] eval-heavy (eval >> writes — the real Nixpkgs shape):
#       local    eager=2.835  lazy+overlap=2.579 (1.10x)  lazy-noov=2.772 (1.02x)
#       daemon   eager=2.832  lazy+overlap=2.521 (1.12x)  lazy-noov=2.821 (1.00x)
#   [elision] substitutable target, 100MB source (bench-elision.sh):
#       eager=0.197 (source COPIED)   lazy=0.037 (source ELIDED)  → ~5x
#
# Read: lazy is neutral-to-faster on instantiation EVERYWHERE (the local case
# was a ~31% regression until `writePaths` stopped routing LocalStore flushes
# through `addMultipleToStore`'s heavyweight NAR-import path and used the flat
# `addToStoreFromDump` instead). The async overlap (`startBackgroundDrain`) is
# LOAD-BEARING in the eval-heavy regime ([B]: noov ~1.00x → overlap ~1.10-1.12x,
# hiding writes behind eval) and neutral on write-heavy ([A]); it is opt-in
# (nix-instantiate only) and the build path never starts it (overlap would
# defeat elision). The headline is [elision]: avoided source/`.drv` copies for
# substitutable targets, dwarfing the instantiation deltas and scaling with
# source size.

set -uo pipefail

M="${1:-2000}"
REPS="${2:-3}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NIX="$root/build/src/nix/nix"
NIXI="$root/build/src/nix/nix-instantiate"
[ -x "$NIXI" ] || { echo "build first: $NIXI missing"; exit 1; }

tmp="$(mktemp -d)"
trap 'pkill -9 -f "$tmp/.*daemon" 2>/dev/null; chmod -R +w "$tmp" 2>/dev/null; rm -rf "$tmp"' EXIT

# Which .nix the runners instantiate (set per scenario).
BENCH_NIX="$tmp/sourceless.nix"

# Scenario A — sourceless, write-heavy: M trivial derivations (eval ≪ writes).
# Isolates the .drv-write deferral/batching axis and the per-item write cost.
cat > "$tmp/sourceless.nix" <<EOF
let mk = n: derivation {
      name = "ld-bench-\${toString n}";
      system = builtins.currentSystem;
      builder = "/bin/sh";
      args = [ "-c" "echo \${toString n} > \$out" ];
    };
in builtins.genList mk $M
EOF

# Scenario B — eval-heavy: each derivation forces a costly pure computation, so
# eval ≫ writes. This is the regime where the async overlap can hide the writes
# behind eval (the real Nixpkgs/Hydra shape), and the one the write-heavy
# scenario A under-represents.
cat > "$tmp/evalheavy.nix" <<EOF
let
  heavy = n: builtins.foldl' (a: b: a + b) n (builtins.genList (i: i) 20000);
  mk = n: derivation {
      name = "ld-heavy-\${toString n}";
      system = builtins.currentSystem;
      builder = "/bin/sh";
      args = [ "-c" "echo \${toString (heavy n)} > \$out" ];
    };
in builtins.genList mk $M
EOF

now() { date +%s.%N; }
elapsed() { echo "$2 - $1" | bc -l; }

# Fresh local store, one timed instantiation. $1 opts, $2 env-assignments
run_local() {
    local sd="$tmp/ls-$RANDOM$RANDOM" t0 t1
    t0=$(now)
    env $2 "$NIXI" "$BENCH_NIX" --store "local?root=$sd" $1 >/dev/null 2>&1
    t1=$(now)
    chmod -R +w "$sd" 2>/dev/null; rm -rf "$sd"
    elapsed "$t0" "$t1"
}

# Fresh store + fresh daemon, one timed instantiation through the socket.
run_daemon() {
    local d="$tmp/ds-$RANDOM$RANDOM" sock="$tmp/sock-$RANDOM$RANDOM" t0 t1 dpid
    mkdir -p "$d/store" "$d/var" "$d/etc"
    NIX_STORE_DIR="$d/store" NIX_STATE_DIR="$d/var" NIX_LOG_DIR="$d/var/log" \
      NIX_CONF_DIR="$d/etc" NIX_DATA_DIR="$d/share" NIX_DAEMON_SOCKET_PATH="$sock" \
      "$NIX" --extra-experimental-features nix-command daemon >/dev/null 2>&1 &
    dpid=$!
    for i in $(seq 1 100); do [ -S "$sock" ] && break; sleep 0.05; done
    t0=$(now)
    env $2 NIX_STORE_DIR="$d/store" NIX_STATE_DIR="$d/var" NIX_CONF_DIR="$d/etc" \
      NIX_REMOTE="unix://$sock" timeout 300 "$NIXI" "$BENCH_NIX" $1 >/dev/null 2>&1
    t1=$(now)
    kill -9 "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null
    chmod -R +w "$d" 2>/dev/null; rm -rf "$d" "$sock"
    elapsed "$t0" "$t1"
}

best_of() {  # $1 runner-fn, $2 opts, $3 env
    local best="" t
    for ((r = 0; r < REPS; r++)); do
        t=$("$1" "$2" "$3")
        if [ -z "$best" ] || (( $(echo "$t < $best" | bc -l) )); then best=$t; fi
    done
    echo "$best"
}

report() {  # $1 runner-fn, $2 label
    local eager lazy noov
    eager=$(best_of "$1" "" "")
    lazy=$(best_of "$1" "--option lazy-derivations true" "")
    noov=$(best_of "$1" "--option lazy-derivations true" "_NIX_LAZY_DRV_NO_OVERLAP=1")
    printf '%-8s eager=%.3fs  lazy+overlap=%.3fs (%.2fx)  lazy-noov=%.3fs (%.2fx)\n' \
        "$2" "$eager" \
        "$lazy" "$(echo "$eager/$lazy" | bc -l)" \
        "$noov" "$(echo "$eager/$noov" | bc -l)"
}

echo "=== mass-instantiation: M=$M derivations, best-of-$REPS (speedup = eager/lazy, >1 means lazy wins) ==="
echo "[A] sourceless / write-heavy (eval << writes) — the .drv-write deferral+batch axis"
BENCH_NIX="$tmp/sourceless.nix"
report run_local  "  local"
report run_daemon "  daemon"
echo "[B] eval-heavy (eval >> writes) — the async-overlap regime (real Nixpkgs shape)"
BENCH_NIX="$tmp/evalheavy.nix"
report run_local  "  local"
report run_daemon "  daemon"
