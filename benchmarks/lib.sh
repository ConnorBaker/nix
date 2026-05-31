#!/usr/bin/env bash
# Lazy-store benchmark harness — shared library.
#
# Compares three (optionally four) Nix builds on the source-materialisation
# workloads the doc/tecnix-survey proposal claims wins on:
#
#   baseline  — upstream master @ 2d309b18e (our merge-base; "prior to our changes")
#   ours      — this tree's build (the lazy-store work)
#   detsys    — DeterminateSystems/nix-src, run with lazy-trees=true
#   tecnix    — Shopify/tecnix (optional; only if built)
#
# Two classes of metric, per the proposal's own framing:
#
#   SEMANTIC  — tree *walks* and *copies*, counted by grepping a -vvvv eval log
#               for the fetch-to-store Activity markers, plus the fetcher-cache
#               sqlite row delta. These are deterministic and machine-independent;
#               they are how PROPOSAL §4 / §8.5 state the claims ("1 walk vs N").
#   WALLCLOCK — hyperfine end-to-end timing. Tangible but noisy/machine-specific.
#
# Isolation: every invocation runs against a private chroot store + state dir +
# clean nix.conf + redirected HOME (so the per-user ~/.cache/nix fetcher cache is
# private too). No daemon. This guarantees controlled cold/warm states and stops
# the trees cross-polluting each other's caches. Modelled on the functional-test
# isolation in tests/functional/common/vars.sh.

set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
NIX_ROOT="$(cd "$BENCH_DIR/.." && pwd)"

# ---------------------------------------------------------------------------
# Binary resolution. Override any path via env (BENCH_BIN_<name>).
# ---------------------------------------------------------------------------
BIN_BASELINE="${BENCH_BIN_BASELINE:-$BENCH_DIR/.baseline-nix/bin/nix}"
BIN_OURS="${BENCH_BIN_OURS:-$NIX_ROOT/build/src/nix/nix}"
BIN_DETSYS="${BENCH_BIN_DETSYS:-/home/cbaker2/ext-sources/nix-src/build/src/nix/nix}"
BIN_TECNIX="${BENCH_BIN_TECNIX:-/home/cbaker2/ext-sources/tecnix/build/src/nix/nix}"

# Per-tree extra nix.conf lines. DetSys gets lazy-trees=true (the mode the
# proposal compares against); everything else gets a vanilla config. Each tree
# only sees settings it understands, so no "unknown setting" warnings leak into
# the -vvvv logs we grep.
conf_for() {
    case "$1" in
        detsys) printf 'lazy-trees = true\n' ;;
        *)      : ;;
    esac
}

# Experimental features needed by all trees for flakes / nix-command.
EXPERIMENTAL_FEATURES="nix-command flakes"

# The ordered list of trees to run. Override with BENCH_TREES="ours baseline".
BENCH_TREES="${BENCH_TREES:-baseline ours detsys}"

bin_for() {
    case "$1" in
        baseline) echo "$BIN_BASELINE" ;;
        ours)     echo "$BIN_OURS" ;;
        detsys)   echo "$BIN_DETSYS" ;;
        tecnix)   echo "$BIN_TECNIX" ;;
        *) echo "unknown tree: $1" >&2; return 1 ;;
    esac
}

tree_available() {
    local b; b="$(bin_for "$1")" || return 1
    [[ -x "$b" ]] && "$b" --version >/dev/null 2>&1
}

# ---------------------------------------------------------------------------
# Isolated environment for one (tree, scratch-root) pair.
# Sets up a private store/state/conf/HOME under $root and echoes nothing;
# callers run `nix_env <tree> <root> -- <bin> <args...>`.
# ---------------------------------------------------------------------------
make_sandbox() {
    # $1 = tree, $2 = root dir (created fresh by caller for "cold")
    local tree="$1" root="$2"
    mkdir -p "$root/store" "$root/var/nix" "$root/conf" "$root/home" "$root/cache"
    {
        printf 'experimental-features = %s\n' "$EXPERIMENTAL_FEATURES"
        printf 'build-users-group =\n'
        printf 'substituters =\n'           # fully offline; no network in the hot path
        printf 'flake-registry =\n'
        printf 'warn-dirty = false\n'
        conf_for "$tree"
    } > "$root/conf/nix.conf"
}

# Run a nix binary for <tree> in the sandbox rooted at <root>.
#   nix_run <tree> <root> <args...>
# stdout/stderr pass through; caller redirects as needed.
nix_run() {
    local tree="$1" root="$2"; shift 2
    local bin; bin="$(bin_for "$tree")"
    env -i \
        PATH="/usr/bin:/bin:$(dirname "$bin")" \
        HOME="$root/home" \
        XDG_CACHE_HOME="$root/cache" \
        NIX_STORE_DIR="$root/store" \
        NIX_STATE_DIR="$root/var/nix" \
        NIX_LOG_DIR="$root/var/log/nix" \
        NIX_CONF_DIR="$root/conf" \
        NIX_REMOTE="" \
        TERM="${TERM:-dumb}" \
        "$bin" "$@"
}

# ---------------------------------------------------------------------------
# Semantic metric: count walks/copies/cache events in a -vvvv eval log.
# The markers are the fetch-to-store Activity strings shared across all trees
# (master/DetSys: "hashing"/"copying"/"is uncacheable"/"cache hit"; ours adds
# "cross-pipeline cache hit"). See src/libfetchers/fetch-to-store.cc.
# ---------------------------------------------------------------------------
count_marker() {  # <pattern> <logfile>  -> always prints an integer, never fails
    local n
    n=$(grep -cE "$1" "$2" 2>/dev/null) || n=0
    printf '%s' "${n:-0}"
}

# Emit a one-line metric record: walks (DryRun "hashing"), copies ("copying"),
# uncacheable bypasses, cache hits. These map directly onto the proposal claims.
log_metrics() {  # <logfile>  ->  "walks=.. copies=.. uncacheable=.. hits=.."
    local log="$1"
    local walks copies uncache hits xpipe
    walks=$(count_marker "hashing '" "$log")
    copies=$(count_marker "copying '" "$log")
    uncache=$(count_marker "is uncacheable" "$log")
    hits=$(count_marker "cache hit" "$log")
    xpipe=$(count_marker "cross-pipeline cache hit" "$log")
    printf 'walks=%s copies=%s uncacheable=%s cache_hits=%s xpipe_hits=%s' \
        "${walks:-0}" "${copies:-0}" "${uncache:-0}" "${hits:-0}" "${xpipe:-0}"
}

# fetcher-cache row count for a sandbox (how many sourcePathToHash etc. rows the
# eval wrote). A cold eval that writes a row => cacheable; one that writes none
# while still copying => the README §6.6 bypass.
fetchercache_rows() {  # <root>  ->  integer (0 if no db yet)
    local db
    db=$(find "$1/cache/nix" -maxdepth 1 -name 'fetcher-cache*.sqlite' 2>/dev/null | head -1)
    [[ -n "$db" ]] || { echo 0; return; }
    sqlite3 "$db" "SELECT COUNT(*) FROM Cache;" 2>/dev/null || echo 0
}

# Whether an eval-cache SQLite exists + has attributes (the warm-eval substrate).
# A populated eval-cache means a warm flake-attr re-eval can skip re-evaluation.
evalcache_rows() {  # <root>  ->  integer (0 if none)
    local db
    db=$(find "$1/cache/nix/eval-cache-v6" -name '*.sqlite' 2>/dev/null | head -1)
    [[ -n "$db" ]] || { echo 0; return; }
    sqlite3 "$db" "SELECT COUNT(*) FROM Attributes;" 2>/dev/null || echo 0
}

# Remove a sandbox/fixture dir. Nix writes store paths read-only, so chmod first.
cleanup_dir() {  # <dir>
    [[ -n "${1:-}" && -e "$1" ]] || return 0
    chmod -R u+w "$1" 2>/dev/null || true
    rm -rf "$1"
}

# ---------------------------------------------------------------------------
# Pretty output helpers.
# ---------------------------------------------------------------------------
hr()  { printf '%.0s─' $(seq 1 "${1:-72}"); printf '\n'; }
note(){ printf '  %s\n' "$*"; }
