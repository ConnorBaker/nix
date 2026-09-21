#!/usr/bin/env bash
# 05-validation.md section 6, "The collector on a dead schema-10 row": an 800 MB unrooted path whose row is
# rewritten to the schema-10 form, then `nix-store --gc` timed; run against build/src/nix of a worktree.
# Parameters: LAZY_STORE_TREE (the worktree), LAZY_STORE_SCRATCH (the test root).
# E5: a whole-store collection on an unmigrated store walks every dead path before deleting it
# (topoSortPaths -> queryPathInfo -> migratedPathInfo).  Timed against the same store with a current row.
set -u
export TEST_ROOT=${LAZY_STORE_SCRATCH:?a scratch directory}/e5/test-root-e5
# shellcheck source=macos-isolated-store-env.sh
source "$(dirname "$0")/macos-isolated-store-env.sh"
big=$TEST_ROOT/big
run() { # $1 = old|new
  freshStore; mkdir -p "$big"; head -c 800000000 /dev/urandom > "$big/blob.bin"
  p=$(nix store add --mode nar "$big")      # unrooted: garbage
  [[ $1 == old ]] && plantOldRow "$p"
  echo "column before gc ($1): $(hashColumn "$p" | cut -c1-20)..."
  sync; /usr/bin/time -p nix-store --gc 2>&1 | sed "s/^/  gc[$1]| /"
}
run new; run old; run new; run old
