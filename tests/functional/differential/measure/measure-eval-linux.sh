#!/usr/bin/env bash
# measure-eval-linux.sh <nix-bin-dir> ["N1 N2 ..."]
#
# Linux peak-resident-memory measurement for the write buffer's memory gate
# (doc/lazy-store/04-derivation.md, the write buffer; 05-validation.md, the
# measurements).  Under the branch binary `nix eval --json` over a list of N
# derivation paths holds the whole serialised value in the write buffer,
# because the JSON serialiser is pure: it flushes once, at the sink.  Run once
# with the reference binary (master's: the eager rows of 05) and once with the
# branch's (deferral is unconditional there), swept over several N, to show
# whether the memory gap between them is linear in the number of pending
# objects (it is: one flush holds them all), which is what decides whether the
# accumulating sinks must flush incrementally.  GNU `time -v` gives Maximum
# resident set size (kbytes).  nix-instantiate over a list of derivations is
# the control: it prints each drvPath as it goes, so its pending set is one
# object at a time and the two binaries should match.  The cap runs at the
# end set `deferred-store-writes-max-pending`, the branch's one setting, and
# mean nothing under the reference.  Nothing is built.  Prints TSV:
#   command  N  rssKB  wall  flushes  objects
set -eu

bindir=${1:?usage: measure-eval-linux.sh <nix-bin-dir> ["N1 N2 ..."]}
Ns=${2:-"1000 4000 16000"}
root=${MEASURE_ROOT:-/tmp/lazy-measure}
TIME=${TIME_BIN:-/usr/bin/time}

run() { # <label> <N> <argv...>
  local label=$1 n=$2; shift 2
  local d=$root/$label-$n
  rm -rf "$d"; mkdir -p "$d/store" "$d/var/nix" "$d/etc"
  export NIX_STORE_DIR=$d/store NIX_STATE_DIR=$d/var/nix NIX_LOG_DIR=$d/var/log/nix
  export NIX_CONF_DIR=$d/etc NIX_DATA_DIR=$d/share NIX_LOCALSTATE_DIR=$d/var NIX_REMOTE=
  unset NIX_CONFIG || true
  printf 'experimental-features = nix-command\nsandbox = false\nsubstituters =\nbuild-users-group =\n' > "$d/etc/nix.conf"
  [ -n "${MAXPENDING:-}" ] && \
    echo "deferred-store-writes-max-pending = $MAXPENDING" >> "$d/etc/nix.conf"
  "$TIME" -v "$@" > "$d/out" 2> "$d/timelog" || true
  local rss wall flushes objects
  rss=$(grep -o 'Maximum resident set size (kbytes): [0-9]*' "$d/timelog" | grep -o '[0-9]*$' || echo NA)
  wall=$(grep 'Elapsed (wall clock) time' "$d/timelog" | sed 's/.*: //' || echo NA)
  flushes=$(grep -c 'writing [0-9]* store objects' "$d/timelog" || true)
  objects=$(grep -o 'writing [0-9]* store objects' "$d/timelog" | grep -o '[0-9]*' | awk '{s+=$1} END{print s+0}')
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$label" "$n" "$rss" "$wall" "$flushes" "$objects"
}

# shellcheck disable=SC2016 # the ${} is Nix string interpolation, not shell
drv='derivation { name = "m-${toString i}"; system = "x86_64-linux"; builder = "/bin/sh"; args = [ "-c" "echo ${toString i} > $out" ]; }'

printf 'command\tN\trssKB\twall\tflushes\tobjects\n'
for n in $Ns; do
  drvlist="builtins.genList (i: ($drv).drvPath) $n"
  run eval-json "$n" "$bindir/nix" eval -vv --json --expr "$drvlist"
done

# Control at the largest N: a list of derivations (not drvPath strings), which
# nix-instantiate prints one at a time.
nmax=$(echo "$Ns" | awk '{print $NF}')
wf=$root/fan.nix; printf 'builtins.genList (i: %s) %s\n' "$drv" "$nmax" > "$wf"
run instantiate "$nmax" "$bindir/nix-instantiate" -vv "$wf"

# The setting controls the cap: at a fixed N the flush count scales as N/cap.
capN=16000
capdrv="builtins.genList (i: ($drv).drvPath) $capN"
for cap in 1024 4096 16000; do
  MAXPENDING=$cap run "cap-$cap" "$capN" "$bindir/nix" eval -vv --json --expr "$capdrv"
done
