#!/usr/bin/env bash
#
# §3b CA-producer gate: sibling-share fire-rate experiment.
#
# Question (go/no-go for the CA-producer direction): on a sibling-share-heavy
# workload (the nix-eval-jobs shape — many python3Packages sharing one closure),
# does the replay-gate-fire to producer-record ratio E:P invert toward/past the
# closures.gnome baseline of 614:6419 ≈ 0.096 as the sibling count grows?
#
# Method (no rebuild — runs against an existing `result/bin/nix` that has §3b
# present and default-OFF): `NIX_ENABLE_CA_PRODUCER=1` flips the hook. For each
# package-list size, evaluate OFF and ON from a fresh isolated cache and read two
# NIX_SHOW_STATS counters:
#   P (producer records) = evalTrace.record.count(ON) - record.count(OFF)
#       (consumer-trace count is identical across modes; the delta is the
#        producer traces §3b adds)
#   E (gate fires)        = evalTrace.replay.producerEdges
#
# RESULT (2026-05-31, see doc/eval-trace-cache-redesign-plan.md follow-up #5):
# FALSIFIED. E:P stays ~0.03 (worse than baseline) and marginal dE:dP -> 0.
# Root cause is structural: the producer is keyed on the `strict`
# (derivationStrict-result) Bindings*, but derivation.nix wraps it as
# `commonAttrs // { outPath = ...; }`, so siblings re-force the //-wrapper /
# read the string outPath and never re-force the keyed `strict` value.
#
# Usage: run from the repo root with result/bin/nix built and nixpkgs available.
#   NIXPKGS=$HOME/ext-sources/nixpkgs bash benchmarks/eval-trace-bench/experiments/ca-producer-sibling-firerate.sh
set -u

NIX="${NIX:-./result/bin/nix}"
NIXPKGS="${NIXPKGS:-$HOME/ext-sources/nixpkgs}"
SYSTEM="${SYSTEM:-x86_64-linux}"
OUT="${OUT:-/tmp/ca-producer-firerate}"
mkdir -p "$OUT"

# Package lists by size. Pick widely-used, deep-closure python packages so the
# shared stdenv/python closure is large (maximizing the sibling-share signal).
declare -A LISTS
LISTS[1]='"numpy"'
LISTS[2]='"numpy" "scipy"'
LISTS[3]='"numpy" "scipy" "pandas"'
LISTS[5]='"numpy" "scipy" "pandas" "matplotlib" "scikit-learn"'
LISTS[8]='"numpy" "scipy" "pandas" "matplotlib" "scikit-learn" "requests" "flask" "django"'

run() {
  local n="$1" tag="$2" envv=""
  [ "$tag" = "on" ] && envv="NIX_ENABLE_CA_PRODUCER=1"
  local CACHE; CACHE=$(mktemp -d "$OUT/cache-$n-$tag.XXXX")
  local EXPR="let pkgs = import (builtins.getEnv \"NIXPKGS\") { system = \"$SYSTEM\"; }; ps = pkgs.python3Packages; in map (n: ps.\${n}.outPath) [ ${LISTS[$n]} ]"
  env $envv NIXPKGS="$NIXPKGS" XDG_CACHE_HOME="$CACHE" \
    NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$OUT/sweep_${n}_${tag}.json" \
    "$NIX" eval --impure --json --expr "$EXPR" >/dev/null 2>"$OUT/sweep_${n}_${tag}.err"
  echo "  [n=$n/$tag] exit=$?"
  rm -rf "$CACHE"
}

for n in 1 2 3 5 8; do
  echo "=== $n package(s) ==="
  run "$n" off
  run "$n" on
done

python3 - "$OUT" <<'PY'
import json, sys
out = sys.argv[1]
print(f'\n{"pkgs":>5} {"recOFF":>7} {"recON":>7} {"P=prod":>7} {"E=fires":>8} {"E:P":>7}')
rows = []
for n in [1, 2, 3, 5, 8]:
    def g(t):
        d = json.load(open(f'{out}/sweep_{n}_{t}.json')); et = d.get('evalTrace', {})
        return et.get('replay', {}).get('producerEdges', 0), et.get('record', {}).get('count', 0)
    eo, ro = g('off'); en, rn = g('on'); P = rn - ro
    rows.append((n, ro, rn, P, en))
    print(f'{n:>5} {ro:>7} {rn:>7} {P:>7} {en:>8} {(en/P if P else float("nan")):>7.3f}')
print('\nclosures.gnome baseline E:P = 614:6419 = 0.096')
print('\nMARGINAL (delta per added batch):')
print(f'{"step":>10} {"dP":>6} {"dE":>6} {"dE:dP":>7}')
for i in range(1, len(rows)):
    dP = rows[i][3] - rows[i-1][3]; dE = rows[i][4] - rows[i-1][4]
    print(f'{rows[i-1][0]}->{rows[i][0]:>2}    {dP:>6} {dE:>6} {(dE/dP if dP else float("nan")):>7.3f}')
PY
