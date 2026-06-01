#!/usr/bin/env bash
#
# Layer 2a crash-consistency reproduction (redesign-plan follow-up #24).
#
# Question (soundness go/no-go for deferred producer recording): does deferring
# `publishStateChange` (Layer 2a) actually close the Sessions-before-Traces
# ordering inversion that `deferFlush` alone (Layer 1) opens? The hazard: Layer 1
# commits a durable per-producer Sessions/History(trace_id=N) row while N's Traces
# row stays buffered until teardown. nextTraceId is reloaded as MAX(Traces.id) at
# open (sqlite-trace-storage-lifecycle.cc:611), so a crash between the commit and
# the teardown flush lets the next process REUSE id N -> the stale durable
# History(N) aliases a different trace. The aggressive shape would stale-serve on
# that; the conservative shape's flattened-dep backstop masks it today.
#
# Invariant under test (post-crash, post-reopen): no Sessions/History row
# references a trace_id absent from Traces. A non-zero dangling count is the #24
# aliasing precondition.
#
# Method (NO rebuild required for the Layer-2a arm — runs against an existing
# `result/bin/nix`/`build/src/nix/nix` that has Layer 2a present, default-OFF):
# evaluate a derivation-dense ATTRSET via `-f` (so many TracedExpr children
# finalize mid-eval -> consumer flush()es interleave and drain producer buffers
# while the eval is in flight -> the DB is genuinely non-empty mid-eval, which is
# the window the crash must land in; a bare `--expr` keeps the DB empty until
# teardown and cannot exercise the hazard). SIGKILL at staggered offsets, reopen,
# and query `COUNT(*) ... WHERE trace_id NOT IN (SELECT id FROM Traces)`.
#
# Non-vacuity (CRITICAL — a crash test that only ever passes is worthless): to
# prove the harness DETECTS the hazard, also run the Layer-1-ALONE variant, which
# is the #24-hazardous state (defer flush, synchronous publish). That arm is NOT
# default-reachable (there is no env flag for it — Layer 2a folds publish-defer
# into the same `deferFlush` flag by design), so producing it requires a one-line
# source patch + rebuild. Set REBUILD_L1_ALONE=1 to do that automatically; the
# patch is reverted afterward. Without it, only the Layer-2a arm runs (a passing
# arm whose detection power is asserted only by reference to the recorded result).
#
# RESULT (2026-05-31, see doc/eval-trace-cache-redesign-plan.md follow-up #24 and
# plans/async-producer-recording-plan.md §8):
#   Layer-1-alone : 6/6 non-empty crashes -> 175..760 dangling Sessions,
#                   194..1024 dangling History  (#24 hazard REPRODUCED)
#   Layer 2a      : 6/6 non-empty crashes -> 0 dangling Sessions, 0 dangling History
# The contrast proves the harness is non-vacuous AND that Layer 2a closes the
# hazard. SIGKILL timing makes this NOT a CI-stable test, so it lives here as a
# reproducible experiment rather than a committed gtest/functional test; the
# committed unit tests (store/deferred-publish-ordering.cc) are within-session +
# clean-flush regression guards only and explicitly do NOT discriminate #24.
#
# Usage (Layer-2a arm only, no rebuild):
#   NIXPKGS=/nix/store/<hash>-source bash benchmarks/eval-trace-bench/experiments/layer2a-crash-consistency.sh
# Usage (both arms, rebuilds the L1-alone variant via nix develop, then reverts):
#   REBUILD_L1_ALONE=1 NIXPKGS=... bash benchmarks/eval-trace-bench/experiments/layer2a-crash-consistency.sh
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 1

# A nixpkgs source tree (store path or checkout) with pkgs/top-level/all-packages.nix.
NIXPKGS="${NIXPKGS:-}"
if [ -z "$NIXPKGS" ] || [ ! -e "$NIXPKGS/default.nix" ]; then
  echo "Set NIXPKGS to a nixpkgs source tree (e.g. a /nix/store/<hash>-source with default.nix)." >&2
  exit 2
fi

RECORDER_CC="src/libexpr/eval-trace/store/recorder.cc"
LDP="build/src/libutil:build/src/libstore:build/src/libfetchers:build/src/libexpr:build/src/libflake:build/src/libmain:build/src/libcmd"
# Prefer the incremental build-tree binary (matches a freshly-patched rebuild);
# fall back to result/bin/nix.
if [ -x build/src/nix/nix ]; then NIX="build/src/nix/nix"; RUN_LDP="$LDP"; else NIX="./result/bin/nix"; RUN_LDP=""; fi

run_arm () {  # $1 = label
  local label="$1"
  local work; work="$(mktemp -d "/tmp/l2a-crash-$label.XXXXXX")"
  # Derivation-dense attrset: each value forces a derivation -> a TracedExpr child
  # -> interleaved consumer flush()es that drain producer buffers mid-eval.
  cat > "$work/top.nix" <<NIXEOF
let p = import $NIXPKGS { system = "x86_64-linux"; };
    L = [ "hello" "ncurses" "zlib" "openssl" "curl" "git" "python3" "gnumake"
          "coreutils" "bash" "gcc" "binutils" "gawk" "gnused" "gnugrep" "perl"
          "ruby" "nodejs" "go" "rustc" "cmake" "ninja" "meson" "pkg-config"
          "autoconf" "automake" "libtool" "bison" "flex" "m4" "gettext"
          "texinfo" "help2man" "gdb" "file" "which" "findutils" "diffutils"
          "patch" "gnutar" "gzip" "bzip2" "xz" "zstd" "lz4" "wget" ];
in builtins.listToAttrs (map (n: { name = n; value = (builtins.getAttr n p).drvPath; }) L)
NIXEOF
  local fail=0 nonempty=0
  for i in 1 2 3 4 5 6; do
    local cache="$work/c$i"; mkdir -p "$cache"
    LD_LIBRARY_PATH="$RUN_LDP" NIX_CONFIG="builders =" NIX_ENABLE_CA_PRODUCER=1 \
      NIX_PRODUCER_DEFER_FLUSH=1 XDG_CACHE_HOME="$cache" \
      "$NIX" eval --impure -f "$work/top.nix" >/dev/null 2>&1 &
    local pid=$!
    sleep "$(awk "BEGIN{print 0.4 + $i*0.35}")"
    local killed=no
    if kill -0 $pid 2>/dev/null; then kill -9 $pid 2>/dev/null; killed=yes; fi
    wait $pid 2>/dev/null
    local db; db="$(find "$cache" -name 'eval-trace-v*.sqlite' 2>/dev/null | head -1)"
    [ -z "$db" ] && { echo "  kill#$i killed=$killed (no db)"; continue; }
    local tr se hi ds dh
    tr=$(sqlite3 "$db" "SELECT COUNT(*) FROM Traces;" 2>/dev/null)
    se=$(sqlite3 "$db" "SELECT COUNT(*) FROM Sessions;" 2>/dev/null)
    hi=$(sqlite3 "$db" "SELECT COUNT(*) FROM History;" 2>/dev/null)
    ds=$(sqlite3 "$db" "SELECT COUNT(*) FROM Sessions WHERE trace_id NOT IN (SELECT id FROM Traces);" 2>/dev/null)
    dh=$(sqlite3 "$db" "SELECT COUNT(*) FROM History  WHERE trace_id NOT IN (SELECT id FROM Traces);" 2>/dev/null)
    [ "${tr:-0}" -gt 0 ] 2>/dev/null && nonempty=$((nonempty+1))
    [ "${ds:-0}" != "0" ] && fail=1
    [ "${dh:-0}" != "0" ] && fail=1
    printf "  kill#%s killed=%-4s Traces=%-6s Sessions=%-6s(dangling=%s) History=%-6s(dangling=%s)\n" \
      "$i" "$killed" "${tr:-?}" "${se:-?}" "${ds:-?}" "${hi:-?}" "${dh:-?}"
  done
  echo "  non-empty-DB kills: $nonempty/6"
  if [ "$fail" -eq 0 ] && [ "$nonempty" -gt 0 ]; then
    echo "  RESULT [$label]: INVARIANT HELD — no dangling trace_id (no #24 aliasing precondition)"
  elif [ "$nonempty" -eq 0 ]; then
    echo "  RESULT [$label]: INCONCLUSIVE — all kills hit empty DB (widen kill offsets)"
  else
    echo "  RESULT [$label]: **VIOLATED** — dangling trace_id present (#24 hazard reachable)"
  fi
  rm -rf "$work"
}

echo "=== Layer 2a (as shipped: deferPublish=deferFlush) ==="
run_arm "l2a"

if [ "${REBUILD_L1_ALONE:-0}" = "1" ]; then
  echo
  echo "=== Layer-1-ALONE (temporary deferPublish=false patch — the #24-hazardous state) ==="
  cp "$RECORDER_CC" "$RECORDER_CC.crashtest-bak"
  sed -i 's|/\*deferPublish=\*/deferFlush|/*deferPublish=*/false /*L1-ALONE-CRASHTEST*/|g' "$RECORDER_CC"
  nix develop .#native-clangStdenv --command bash -c 'meson compile -C build' >/dev/null 2>&1
  run_arm "l1alone"
  # Revert + rebuild to restore the shipped binary.
  cp "$RECORDER_CC.crashtest-bak" "$RECORDER_CC"; rm -f "$RECORDER_CC.crashtest-bak"
  nix develop .#native-clangStdenv --command bash -c 'meson compile -C build' >/dev/null 2>&1
  echo "  (reverted L1-alone patch; L1-ALONE-CRASHTEST markers remaining: $(grep -c 'L1-ALONE-CRASHTEST' "$RECORDER_CC"))"
fi
