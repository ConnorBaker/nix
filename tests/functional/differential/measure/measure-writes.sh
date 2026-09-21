#!/usr/bin/env bash
# measure-writes.sh <label> <nix package> <workload.nix>
#
# Local mode, no daemon.  Counts the write buffer's flushes and their batch
# sizes from the talkative activity "writing N store objects", and the size
# of the .drv closure the command wrote.  These are deterministic and do not
# depend on the machine.  Under the reference binary each object is one
# `addToStore` and the write count is the closure size (it emits no flush
# lines); under the branch's the flushes and their batches are what the
# buffer turns those into.  Scratch under $MEASURE_ROOT (required).  Also reports wall
# time and peak resident memory from `/usr/bin/time -l`, for DIRECTION ONLY:
# this is macOS on APFS, noisy and not representative of a Linux install.
# Prints a TSV line.
set -eu
label=$1; pkg=$2; workload=$3
root=${MEASURE_ROOT:?set MEASURE_ROOT to a scratch directory}/$label
[ -d "$root" ] && chmod -R u+w "$root" 2>/dev/null || true
rm -rf "$root"; mkdir -p "$root/store" "$root/var/nix" "$root/etc"
export NIX_STORE_DIR=$root/store NIX_STATE_DIR=$root/var/nix NIX_LOG_DIR=$root/var/log/nix
export NIX_CONF_DIR=$root/etc NIX_DATA_DIR=$root/share NIX_LOCALSTATE_DIR=$root/var
printf 'experimental-features = nix-command\nsandbox = false\nsubstituters =\nbuild-users-group =\n' > "$root/etc/nix.conf"

/usr/bin/time -l "$pkg/bin/nix-instantiate" -vv "$workload" \
  > "$root/drvpaths.txt" 2> "$root/log" || true

# Flushes and their batch sizes (the branch binary alone emits these).
flushes=$(grep -c 'writing [0-9]* store objects' "$root/log" || true)
# Batch sizes; awk sums them and tracks the peak, robust to an empty list
# (the reference binary emits no flushes).
read -r objects peak < <(
  grep -o 'writing [0-9]* store objects' "$root/log" | grep -o '[0-9]*' |
  awk '{ s += $1; if ($1 > m) m = $1 } END { print s+0, m+0 }')
# The .drv closure the command produced: the same under both binaries (no elision).
# shellcheck disable=SC2046 # drvpaths.txt is a list of store paths, one per line; the word splitting is the point
closure=$("$pkg/bin/nix-store" -q --requisites $(cat "$root/drvpaths.txt") 2>/dev/null | wc -l | tr -d ' ')
# Direction only.
secs=$(grep -o '[0-9.]* real' "$root/log" | grep -o '^[0-9.]*' || echo NA)
rssMB=$(( $(grep -o '[0-9]* *maximum resident set size' "$root/log" | grep -o '^[0-9]*' || echo 0) / 1048576 ))

printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
  "$label" "$closure" "$flushes" "$objects" "$peak" "$secs" "$rssMB"
