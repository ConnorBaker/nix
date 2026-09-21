#!/usr/bin/env bash
# 05-validation.md section 6: the macOS first entry of a 40 000-file tree, one run per binary, /usr/bin/time -l.
# Parameters: LAZY_STORE_SCRATCH (the scratch directory holding the generated repository), the CLI paths below.
set -uo pipefail
SC=${LAZY_STORE_SCRATCH:?a scratch directory}
W=$SC/macentry/run40; R=${MAC_REPO:-$SC/entry/macrun/repo}
BRANCH=$SC/macentry-wt/build/src/nix
MASTER=${NIX_REFERENCE_BIN:?a bin directory of a Nix built from master}
mkdir -p "$W"; export HOME=$W/home; mkdir -p "$HOME"
echo "== repo $R files: $(find "$R" -type f -not -path '*/.git/*' | wc -l)  loose objects: $(find "$R/.git/objects" -type f | wc -l)"
echo "== load at start: $(uptime)"
run() { # label attr
  local label=$1 attr=$2 BIN CFG root
  case $label in master*) BIN=$MASTER; CFG=$'extra-experimental-features = nix-command flakes\nauto-optimise-store = false\n';;
    *) BIN=$BRANCH; CFG=$'extra-experimental-features = nix-command flakes\n';; esac
  root=$W/root-$label
  NIX_CONFIG="$CFG" XDG_CACHE_HOME="$W/cache-$label" /usr/bin/time -l \
    "$BIN/nix" --store "$root" eval --raw --impure \
    --expr "(builtins.fetchGit { url = \"$R\"; }).$attr" > /dev/null 2> "$W/t-$label-$attr.txt"
  printf '%-7s %-9s %s   load %s\n' "$label" "$attr" \
    "$(grep -E 'real' "$W/t-$label-$attr.txt" | tr -s ' ')" "$(uptime | sed 's/.*averages: //')"
}
for label in branch master; do
  case $label in master*) a=narHash;; *) a=treeHash;; esac
  root=$W/root-$label; [ -e "$root" ] && chmod -R u+w "$root" 2>/dev/null; rm -rf "$root" "$W/cache-$label"
  run "$label" "$a"; run "$label" "$a"; run "$label" outPath; run "$label" outPath
done
echo "== done: $(uptime)"
