#!/usr/bin/env bash
#
# REAL nix-eval-jobs benchmark for lazy-derivations. nix-eval-jobs is THE
# eval-store mass-instantiation workload (§2): it forks worker processes that
# evaluate many flake/attrset jobs and write their `.drv` closures to a store.
# Because it LINKS the evaluator (libnixexpr/libnixstore), measuring our
# lazy-derivations work requires nix-eval-jobs built AGAINST OUR nix:
#
#   nix build github:nix-community/nix-eval-jobs \
#     --override-input nix path:$HOME/Packages/nix --out-link /tmp/nej-result
#
# Then: bench-lazy-nix-eval-jobs.sh [nix-eval-jobs-bin] [nixpkgs-path] [reps]
#
# FINDINGS (2026-06-02) — the headline workload does NOT transparently benefit;
# building+running it against our nix required TWO source patches and still does
# not win (and is broken over a daemon):
#   1. COMPILE: our fork's 4th NixStringContextElem variant (`SourceVirtual`,
#      from the source-virtualization track) breaks nix-eval-jobs' exhaustive
#      `std::visit` in extractConstituents (the R3 hazard). Patch: a SourceVirtual
#      handler in src/worker.cc.
#   2. CORRECTNESS: with lazy-derivations, nix-eval-jobs SILENTLY LOSES .drvs —
#      it reports drvPaths but never calls `waitForAllPaths`, so each worker's
#      deferred .drvs are DISCARDED at EvalState destruction (dtor-discard =
#      elision). Measured: lazy wrote 364/855 (local) and 0/855 (daemon). The
#      reliable fix is a PER-JOB flush (the worker-exit flush is unreachable —
#      the collector KILLS workers), in src/worker.cc processDerivation before
#      registerGCRoot. That FORFEITS batching (the entire win).
# Result after the per-job-flush patch (15-pkg jobset, cold, best-of-3):
#   local   eager 0.527  lazy 0.520 (1.01x)  noov 0.493 (1.07x)   852/855 drvs
#           (~neutral — per-job flush = no batching; 3 root-eval drvs still lost)
#   daemon  BROKEN — registerGCRoot: "path '…drv' is not a valid store path"
#           (the per-job flush doesn't register the deferred closure over a
#           daemon → 0 drvs, every job errors). The Hydra eval-store setup.
# CONCLUSION: lazy-derivations is transparent for IN-TREE CLI commands (each
# audited + flushed, LD-X4) but NOT for library/evaluator consumers like
# nix-eval-jobs — the deferred-write contract (a reported drvPath may have no
# .drv) breaks them. The eval-store mass-instantiation "headline" is, in
# practice on the real tool, non-transparent (needs patching), non-winning
# (per-job flush kills batching), and broken over a daemon.
#
# Cold-instantiates a real nixpkgs package set (each a worker job, shared stdenv
# closure) into a FRESH store, eager vs lazy(+ovrlap) vs lazy-noov, local AND
# daemon eval-stores; asserts the .drv count matches across modes.
set -uo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NIX="$root/build/src/nix/nix"; XP="--extra-experimental-features"
NEJ="${1:-/tmp/nej-result/bin/nix-eval-jobs}"
NIXPKGS="${2:-$("$NIX" eval --impure --raw --expr '(builtins.getFlake "nixpkgs").outPath' $XP "nix-command flakes" 2>/dev/null)}"
REPS="${3:-3}"
[ -x "$NEJ" ] || { echo "no nix-eval-jobs at '$NEJ' (build it first, see header)"; exit 1; }

tmp="$(mktemp -d)"; trap 'chmod -R +w "$tmp" 2>/dev/null; rm -rf "$tmp"' EXIT
# An attrset of packages = the eval-jobs job shape (many roots, one shared closure).
EXPR='let pkgs = import '"$NIXPKGS"' {}; in {
  inherit (pkgs) hello git python3 gnumake curl openssl coreutils bash vim
                 ripgrep jq cmake nodejs rustc go; }'

run_local() {  # $1 env, $2 opts -> "time drvs"
    local d="$tmp/s$RANDOM$RANDOM" g="$tmp/g$RANDOM$RANDOM" t0 t1 n
    mkdir -p "$g"
    t0=$(date +%s.%N)
    env $1 "$NEJ" --expr "$EXPR" --gc-roots-dir "$g" --store "local?root=$d" $2 >/dev/null 2>&1
    t1=$(date +%s.%N)
    n=$(ls "$d/nix/store/"*.drv 2>/dev/null | wc -l)
    chmod -R +w "$d" "$g" 2>/dev/null; rm -rf "$d" "$g"
    echo "$(echo "$t1 - $t0" | bc -l) $n"
}
run_daemon() {  # $1 env, $2 opts
    local d="$tmp/d$RANDOM$RANDOM" g="$tmp/g$RANDOM$RANDOM" sock="$tmp/k$RANDOM" t0 t1 n dpid
    mkdir -p "$d/store" "$d/var" "$d/etc" "$g"
    NIX_STORE_DIR="$d/store" NIX_STATE_DIR="$d/var" NIX_LOG_DIR="$d/var/log" NIX_CONF_DIR="$d/etc" \
      NIX_DAEMON_SOCKET_PATH="$sock" "$NIX" $XP "nix-command" daemon >/dev/null 2>&1 &
    dpid=$!; for i in $(seq 1 100); do [ -S "$sock" ] && break; sleep 0.05; done
    t0=$(date +%s.%N)
    env $1 NIX_STORE_DIR="$d/store" NIX_STATE_DIR="$d/var" NIX_CONF_DIR="$d/etc" NIX_REMOTE="unix://$sock" \
      "$NEJ" --expr "$EXPR" --gc-roots-dir "$g" $2 >/dev/null 2>&1
    t1=$(date +%s.%N)
    n=$(ls "$d/store/"*.drv 2>/dev/null | wc -l)
    kill -9 "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null
    chmod -R +w "$d" "$g" 2>/dev/null; rm -rf "$d" "$g" "$sock"
    echo "$(echo "$t1 - $t0" | bc -l) $n"
}
best() { local fn="$1" env="$2" opts="$3" best="" drv t n; for ((r=0;r<REPS;r++)); do read -r t n < <("$fn" "$env" "$opts"); drv=$n; [ -z "$best" ] || (( $(echo "$t<$best"|bc -l) )) && best=$t; done; echo "$best $drv"; }
bench() {  # $1 runner $2 label
    local et en lt ln nt nn
    read -r et en < <(best "$1" "" "")
    read -r lt ln < <(best "$1" "" "--option lazy-derivations true")
    read -r nt nn < <(best "$1" "_NIX_LAZY_DRV_NO_OVERLAP=1" "--option lazy-derivations true")
    printf '%-8s eager=%.3fs(%s)  lazy=%.3fs(%s,%.2fx)  noov=%.3fs(%s,%.2fx)\n' \
      "$2" "$et" "$en" "$lt" "$ln" "$(echo "$et/$lt"|bc -l)" "$nt" "$nn" "$(echo "$et/$nt"|bc -l)"
    [ "$en" = "$ln" ] && [ "$en" = "$nn" ] || echo "  !! drv counts differ ($en/$ln/$nn)"
}
echo "=== nix-eval-jobs (built against our nix), 15-pkg jobset, cold, best-of-$REPS ==="
echo "nix-eval-jobs: $NEJ"
bench run_local  "local"
bench run_daemon "daemon"
