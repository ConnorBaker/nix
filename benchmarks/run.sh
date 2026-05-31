#!/usr/bin/env bash
# Lazy-store benchmark runner.
#
# For each workload and each tree (baseline / ours / detsys), measures:
#   - SEMANTIC: tree walks ("hashing"), copies ("copying"), uncacheable
#     bypasses, and cache hits, by grepping a -vvvv eval log. Reported for a
#     COLD store and a WARM re-eval.
#   - WALLCLOCK (optional, --time): hyperfine cold (store wiped each run) and
#     warm timings.
#
# Each (tree, workload, temperature) runs in a private chroot store + cache so
# states are controlled and trees never share a cache. See lib.sh.
#
# Usage:
#   run.sh                 # all workloads, semantic metrics, all trees
#   run.sh --time          # also collect hyperfine wall-clock
#   run.sh --json OUT.json # emit machine-readable results
#   run.sh -w filtered     # only the named workload(s) (repeatable)
#   BENCH_TREES="ours baseline" run.sh
#
# Workloads (the PROPOSAL §4 / §8.5 claims):
#   filtered     §6.6/§7.12  filtered source (lib.cleanSource shape) — cache bypass
#   cargo        §4          monorepo: N builtins.path over subtrees of one rev
#   crossrev     §6.1        same subtree across two git revs — walk once?
#   pureflake    §8.5        pure-eval flake input read metadata-only — 0 walks (parity)
#   interp       §8.2(2)     interpolate a virtual source into a string — copy or not?
#   general      —           broad path-heavy flake eval (general perf)

set -euo pipefail
BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
source "$BENCH_DIR/lib.sh"

DO_TIME=0
JSON_OUT=""
SELECT_WORKLOADS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --time) DO_TIME=1; shift ;;
        --json) JSON_OUT="$2"; shift 2 ;;
        -w) SELECT_WORKLOADS+=("$2"); shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

# Regenerate fixtures if missing.
[[ -f "$BENCH_DIR/.fixtures/meta.env" ]] || bash "$BENCH_DIR/fixtures.sh"
# shellcheck disable=SC1091
source "$BENCH_DIR/.fixtures/meta.env"

WORK="$BENCH_DIR/.work"; cleanup_dir "$WORK"; mkdir -p "$WORK"
JSON_ROWS=()

want_workload() {  # honour -w filters
    [[ ${#SELECT_WORKLOADS[@]} -eq 0 ]] && return 0
    local w; for w in "${SELECT_WORKLOADS[@]}"; do [[ "$w" == "$1" ]] && return 0; done
    return 1
}

# --- workload expression emitters: echo a nix expr for the given tree ---------
# (Most exprs are tree-agnostic; impure fetchGit lets us avoid flake-lock noise.)

expr_filtered() {
    cat <<NIX
let t = builtins.fetchGit { url = "file://$SHARED_REPO"; rev = "$SHARED_REV_A"; };
in builtins.path { path = t.outPath; name = "filt"; filter = (p: type: true); }
NIX
}

expr_cargo() {
    # N distinct-named builtins.path over subtrees of ONE rev: the cargo-workspace
    # shape. Force all outPaths so each materialises.
    cat <<NIX
let
  t = builtins.fetchGit { url = "file://$MONO_REPO"; rev = "$MONO_REV"; };
  pkg = n: builtins.path { path = t.outPath + "/pkgs/pkg\${toString n}"; name = "pkg\${toString n}"; };
in builtins.concatStringsSep " " (map (n: pkg n) (builtins.genList (i: i + 1) $MONO_N_PKGS))
NIX
}

expr_crossrev() {
    # Same /sub subtree under two different git revs (README §6.1). Honest note:
    # this workload slices /sub off an already-materialised fetchGit.outPath, so
    # each rev's WHOLE-REPO input is DryRun-hashed first — that dominates the cold
    # "walks" count and is identical work on all trees (ours shows more "walks"
    # only because it DryRun-hashes the deferred subtree separately rather than
    # cp-ing from a pre-materialised repo path like the eager trees do). The
    # cross-pipeline tree-OID bridge (xpipe_hits) is narrowly reached and does NOT
    # fire here; it targets whole-input / tarball-vs-git tree-SHA sharing, not a
    # subtree sliced from a materialised store path. The meaningful signal in this
    # row is the fetcher-cache ROW count (does the shared subtree dedup?), not the
    # raw walk count. Reported as-is rather than massaged into a win.
    cat <<NIX
let
  a = builtins.fetchGit { url = "file://$SHARED_REPO"; rev = "$SHARED_REV_A"; };
  b = builtins.fetchGit { url = "file://$SHARED_REPO"; rev = "$SHARED_REV_B"; };
in builtins.path { path = a.outPath + "/sub"; name = "sub"; }
 + " "
 + builtins.path { path = b.outPath + "/sub"; name = "sub"; }
NIX
}

expr_pureflake() {
    # Metadata-only read of a flake input — never reads bytes, never narHash.
    # Parity claim: 0 walks on all lazy trees.
    cat <<NIX
let t = builtins.fetchGit { url = "file://$FLAKE_REPO"; rev = "$FLAKE_REV"; };
in t.rev
NIX
}

expr_interp() {
    # The ZonePath footgun (PROPOSAL §8.2(2)). Build a virtual source, interpolate
    # it into a string, but DISCARD the path from final output (return the string
    # length). Because the CLI devirtualises every path that reaches output
    # (README §2.4), the win is only observable when the path does NOT reach
    # output. ours: threads the placeholder through nString → 0 copies; eager
    # trees (baseline, detsys) coerce-and-copy regardless. Output via --json (int).
    cat <<NIX
let
  t = builtins.fetchGit { url = "file://$SHARED_REPO"; rev = "$SHARED_REV_A"; };
  p = builtins.path { path = t.outPath + "/sub"; name = "sub"; };
in builtins.stringLength "prefix:\${p}:suffix"
NIX
}

emit_expr() {  # <workload>
    case "$1" in
        filtered) expr_filtered ;;
        cargo)    expr_cargo ;;
        crossrev) expr_crossrev ;;
        pureflake)expr_pureflake ;;
        interp)   expr_interp ;;
        *) echo "no expr for workload $1" >&2; return 1 ;;
    esac
}

# eval flags per workload. interp returns an int → --json; the rest return
# strings → --raw. All --impure (impure fetchGit + no flake lock).
eval_one() {  # <tree> <root> <workload> <logfile>
    local tree="$1" root="$2" wl="$3" log="$4"
    local expr fmt; expr="$(emit_expr "$wl")"
    case "$wl" in
        interp) fmt=--json ;;
        *)      fmt=--raw ;;
    esac
    nix_run "$tree" "$root" eval --impure "$fmt" --expr "$expr" -vvvv >/dev/null 2>"$log"
}

# --- semantic run: cold + warm in one persistent sandbox --------------------
run_semantic() {  # <tree> <workload>  -> prints a table row, appends JSON
    local tree="$1" wl="$2"
    local root="$WORK/$wl-$tree"; make_sandbox "$tree" "$root"
    local cold="$root/cold.log" warm="$root/warm.log"
    if ! eval_one "$tree" "$root" "$wl" "$cold"; then
        printf "  %-9s | %-50s | FAILED (cold)\n" "$tree" ""
        note "$(tail -2 "$cold")"; return
    fi
    eval_one "$tree" "$root" "$wl" "$warm" || true
    local rows; rows="$(fetchercache_rows "$root")"
    printf "  %-9s | %-50s | %-50s | rows=%s\n" \
        "$tree" "$(log_metrics "$cold")" "$(log_metrics "$warm")" "$rows"

    # JSON record
    local cw cc cu ch ww wc wu wh
    read -r cw cc cu ch _ < <(metrics_nums "$cold") || true
    read -r ww wc wu wh _ < <(metrics_nums "$warm") || true
    JSON_ROWS+=("$(printf '{"workload":"%s","tree":"%s","cold":{"walks":%s,"copies":%s,"uncacheable":%s,"hits":%s},"warm":{"walks":%s,"copies":%s,"uncacheable":%s,"hits":%s},"cache_rows":%s}' \
        "$wl" "$tree" "$cw" "$cc" "$cu" "$ch" "$ww" "$wc" "$wu" "$wh" "$rows")")
}

# numeric-only metric extraction for JSON
metrics_nums() {  # <log> -> "walks copies uncacheable hits xpipe"
    local log="$1"
    printf '%s %s %s %s %s\n' \
        "$(count_marker "hashing '" "$log")" \
        "$(count_marker "copying '" "$log")" \
        "$(count_marker "is uncacheable" "$log")" \
        "$(count_marker "cache hit" "$log")" \
        "$(count_marker "cross-pipeline cache hit" "$log")"
}

# --- wall-clock run via hyperfine (optional) --------------------------------
run_timing() {  # <tree> <workload>
    command -v hyperfine >/dev/null || { note "hyperfine missing; skipping timing"; return; }
    local tree="$1" wl="$2"
    local root="$WORK/time-$wl-$tree"; make_sandbox "$tree" "$root"
    local expr fmt; expr="$(emit_expr "$wl")"
    case "$wl" in interp) fmt=--json ;; *) fmt=--raw ;; esac
    # Write a tiny runner script so hyperfine invokes the isolated env.
    local runner="$root/run.sh"
    {
        echo '#!/usr/bin/env bash'
        echo "exec env -i PATH='/usr/bin:/bin:$(dirname "$(bin_for "$tree")")' \\"
        echo "  HOME='$root/home' XDG_CACHE_HOME='$root/cache' \\"
        echo "  NIX_STORE_DIR='$root/store' NIX_STATE_DIR='$root/var/nix' \\"
        echo "  NIX_LOG_DIR='$root/var/log/nix' NIX_CONF_DIR='$root/conf' NIX_REMOTE='' \\"
        echo "  '$(bin_for "$tree")' eval --impure $fmt --expr '$expr' >/dev/null"
    } > "$runner"; chmod +x "$runner"
    # WARM timing: store/cache pre-populated (one untimed run), then measured.
    "$runner" >/dev/null 2>&1 || true
    local warm_json="$root/warm.hf.json"
    hyperfine --warmup 1 --runs 8 --export-json "$warm_json" "$runner" >/dev/null 2>&1 || { note "$tree warm timing failed"; return; }
    local warm_mean; warm_mean=$(jq -r '.results[0].mean' "$warm_json" 2>/dev/null)
    # COLD timing: wipe store+cache before each run via --prepare.
    local cold_json="$root/cold.hf.json"
    hyperfine --runs 5 \
        --prepare "chmod -R u+w '$root/store' '$root/cache' 2>/dev/null; rm -rf '$root/store' '$root/cache'; mkdir -p '$root/store' '$root/cache'" \
        --export-json "$cold_json" "$runner" >/dev/null 2>&1 || { note "$tree cold timing failed"; return; }
    local cold_mean; cold_mean=$(jq -r '.results[0].mean' "$cold_json" 2>/dev/null)
    printf "  %-9s | cold %8.3f s | warm %8.3f s\n" "$tree" "$cold_mean" "$warm_mean"
}

# ---------------------------------------------------------------------------
ALL_WORKLOADS=(filtered cargo crossrev pureflake interp)
echo "Trees: $BENCH_TREES"
for t in $BENCH_TREES; do tree_available "$t" || echo "  WARNING: tree '$t' unavailable, skipping"; done
echo

for wl in "${ALL_WORKLOADS[@]}"; do
    want_workload "$wl" || continue
    hr 120
    echo "WORKLOAD: $wl"
    printf "  %-9s | %-50s | %-50s | %s\n" tree "COLD  (walks/copies/uncacheable/hits)" "WARM  (re-eval)" "cache"
    hr 120
    for t in $BENCH_TREES; do
        tree_available "$t" || continue
        run_semantic "$t" "$wl"
    done
    if [[ $DO_TIME -eq 1 ]]; then
        echo "  --- wall-clock (hyperfine) ---"
        for t in $BENCH_TREES; do tree_available "$t" || continue; run_timing "$t" "$wl"; done
    fi
    echo
done

if [[ -n "$JSON_OUT" ]]; then
    {
        printf '[\n'
        json_first=1
        for r in "${JSON_ROWS[@]}"; do
            [[ $json_first -eq 1 ]] && json_first=0 || printf ',\n'
            printf '  %s' "$r"
        done
        printf '\n]\n'
    } > "$JSON_OUT"
    echo "JSON written: $JSON_OUT"
fi

cleanup_dir "$WORK"
