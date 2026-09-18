#!/usr/bin/env bash
# count-ops.sh <label> <nix package> <workload.nix> [attr]
#
# Runs `nix-instantiate` through a private daemon started with --debug, and
# counts the daemon's worker operations by opcode from its log.  The opcode
# count is deterministic and machine-independent: it is the number of
# protocol round trips, which is what the write buffer is meant to reduce;
# run once with the reference binary (one `AddToStore` per object) and once
# with the branch's (the buffer's batches).  Scratch under $MEASURE_ROOT
# (required).  Prints one TSV line: label total AddToStore
# AddMultipleToStore AddTempRoot QueryValidPaths IsValidPath valid objects.
set -eu
label=$1; pkg=$2; workload=$3; attr=${4:-}
root=${MEASURE_ROOT:?set MEASURE_ROOT to a scratch directory}/$label
[ -d "$root" ] && chmod -R u+w "$root" 2>/dev/null || true
rm -rf "$root"; mkdir -p "$root/store" "$root/var/nix" "$root/etc"
export NIX_STORE_DIR=$root/store NIX_STATE_DIR=$root/var/nix NIX_LOG_DIR=$root/var/log/nix
export NIX_CONF_DIR=$root/etc NIX_DATA_DIR=$root/share NIX_LOCALSTATE_DIR=$root/var
printf 'experimental-features = nix-command\nsandbox = false\nsubstituters =\nbuild-users-group =\n' > "$root/etc/nix.conf"
export PATH=$pkg/bin:$PATH
sock=$root/var/nix/daemon-socket/socket
mkdir -p "$(dirname "$sock")"

# The daemon writes each op as "performing daemon worker op: N" at debug.
NIX_DAEMON_SOCKET_PATH=$sock nix-daemon --debug > "$root/daemon.log" 2>&1 &
dpid=$!
for _ in $(seq 1 100); do [ -S "$sock" ] && break; sleep 0.05; done

argsN=("$workload"); [ -n "$attr" ] && argsN=(-A "$attr" "$workload")
NIX_REMOTE=unix://$sock nix-instantiate "${argsN[@]}" > "$root/drvpaths.txt" 2> "$root/instantiate.log" || true
# shellcheck disable=SC2046 # drvpaths.txt is a list of store paths, one per line; the word splitting is the point
valid=$(NIX_STORE_DIR=$root/store NIX_STATE_DIR=$root/var/nix nix-store -q --requisites $(cat "$root/drvpaths.txt") 2>/dev/null | wc -l | tr -d ' ')
kill "$dpid" 2>/dev/null || true; wait "$dpid" 2>/dev/null || true

op() { grep -c "performing daemon worker op: $1\$" "$root/daemon.log" || true; }
total=$(grep -c 'performing daemon worker op:' "$root/daemon.log" || true)
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
  "$label" "$total" "$(op 7)" "$(op 44)" "$(op 11)" "$(op 31)" "$(op 1)" "$valid" \
  "$(wc -l < "$root/drvpaths.txt" | tr -d ' ')"
