#!/usr/bin/env bash
# 05-validation.md section 6, "The memo's writes at a cold naming": the first naming of a 40 000-file tree
# (fetchGit { url; rev; } .treeHash) with the memo rows deleted before each run; the clone and cache stay warm.
# Parameters: LAZY_STORE_SCRATCH (holds repo/, home/, cache/, etc/, store-root), NIX (the CLI under test), $1 = label.
# C1: the first naming of a 40000-file tree (fetchGit { url; rev; } .treeHash, the naming walk alone) with the
# memo rows deleted before each run; the clone (gitv3) and the cache file stay warm.  $1 = label.
set -u
S=${LAZY_STORE_SCRATCH:?a scratch directory}
NIX=${NIX:-${LAZY_STORE_TREE:?the worktree}/build/src/nix/nix}
R=$S/c1/repo; REV=$(git -C "$R" rev-parse HEAD)
export HOME=$S/c1/home XDG_CACHE_HOME=$S/c1/cache; mkdir -p "$HOME" "$XDG_CACHE_HOME"
export NIX_CONFIG=$'extra-experimental-features = nix-command flakes'
export _NIX_TEST_NO_LSOF=1
export NIX_CONF_DIR=$S/c1/etc; mkdir -p "$NIX_CONF_DIR"; : > "$NIX_CONF_DIR/nix.conf"; unset NIX_USER_CONF_FILES
ROOT=$S/c1/store-root
DB=$XDG_CACHE_HOME/nix/fetcher-cache-v4.sqlite
label=$1
cmd=("$NIX" --store "$ROOT" eval --raw --impure --expr "(builtins.fetchGit { url = \"$R\"; rev = \"$REV\"; }).treeHash")
ev() { "${cmd[@]}"; }
echo "== $label  load: $(sysctl -n vm.loadavg)"
# warm-up: clone into the cache, create the cache file
if [ ! -e "$DB" ]; then echo "warm-up (clone + cache file): $(ev)"; fi
echo "rows before: $(sqlite3 "$DB" "select count(*) from Cache where domain='sourcePathToHash'")"
for i in 1 2 3; do
  sqlite3 "$DB" "delete from Cache where domain='sourcePathToHash'"
  out=$( { /usr/bin/time -p "${cmd[@]}" 2>&1 1>"$S/c1/out-$label-$i.txt"; } 2>&1 )
  printf '%s run %d: %s  real %s\n' "$label" "$i" "$(cat "$S/c1/out-$label-$i.txt")" "$(echo "$out" | awk '/^real/ {print $2}')"
done
echo "rows after: $(sqlite3 "$DB" "select count(*) from Cache where domain='sourcePathToHash'")"
