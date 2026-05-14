#!/usr/bin/env bash
# Dynamic-throttle daemon. Watches for the bench-process's marker
# files in /tmp and toggles two artificial-throttle knobs around the
# bench's measured call:
#
#   1. The cgroup `io.max` of `benchthrottle.slice`. The bench is
#      run under this slice via `systemd-run`. While the throttle is
#      off the slice has unlimited I/O on the bench device; while
#      on, the configured IOPS/BPS limits apply.
#
#   2. The `dm-delay` virtual block device that wraps the bench's
#      target device (when DM_DELAY_MS > 0). Off → delay 0; on →
#      delay $DM_DELAY_MS.
#
# Protocol (in lock-step with `timedCall` in optimise-bench.cc):
#
#   bench touches /tmp/.bench_throttle_on
#   daemon applies cgroup io.max + dm-delay
#   daemon touches /tmp/.bench_throttle_ack_on    (bench unblocks)
#
#   bench runs measured call
#
#   bench touches /tmp/.bench_throttle_off
#   daemon clears cgroup io.max + dm-delay
#   daemon touches /tmp/.bench_throttle_ack_off   (bench unblocks)
#
# Required env (set by mk-test.nix when starting the daemon):
#   THROTTLE_DEV         absolute path to the bench's underlying
#                        block device (e.g. /dev/vdb). The cgroup
#                        io.max table is keyed on its major:minor.
#   THROTTLE_IOPS        IOPS limit for the "on" state. Set to 0
#                        to skip io.max manipulation (e.g. dm-delay
#                        only). Treated as "max" when negative.
#   THROTTLE_BPS         BPS limit for the "on" state. Same.
#   DM_DELAY_MS          ms of dm-delay to apply per I/O when on.
#                        0 = skip dm-delay manipulation.
#   DM_DELAY_TARGET      dmsetup target name (defaults to "slow",
#                        matching what mk-test.nix's setup phase
#                        creates).

set -euo pipefail

: "${THROTTLE_DEV:?must be set}"
: "${THROTTLE_IOPS:=0}"
: "${THROTTLE_BPS:=0}"
: "${DM_DELAY_MS:=0}"
: "${DM_DELAY_TARGET:=slow}"

# Resolve major:minor for the target device. cgroup v2 `io.max` is
# keyed by this pair, not by path. THROTTLE_DEV is the underlying
# virtio disk (set by test_script.py), not the dm-delay wrapper —
# I/O against `/dev/mapper/slow` charges to the underlying disk's
# major:minor at the block layer, which is what cgroup throttles.
maj_min=$(stat -c '%t:%T' "$THROTTLE_DEV")
# `stat` prints these in hex. cgroup wants decimal.
maj_hex="${maj_min%:*}"
min_hex="${maj_min#*:}"
maj_dec=$((16#$maj_hex))
min_dec=$((16#$min_hex))
dev_id="$maj_dec:$min_dec"

slice="benchthrottle.slice"
cg_root="/sys/fs/cgroup/$slice"
cg_io_max="$cg_root/io.max"

# Reload the dm-delay table to the given (read_delay write_delay) ms
# pair. Both deltas are written as a single table; we re-emit the
# full table rather than `--notable` so the size field stays right.
#
# `set -e` would normally bail before `dmsetup resume` if `reload`
# fails — leaving the device suspended and any subsequent I/O on it
# blocked indefinitely. Guard against that by forcing `resume` to
# run before propagating the error.
reload_delay() {
  local rd_ms="$1" wr_ms="$2" size
  size=$(blockdev --getsz "$THROTTLE_DEV")
  dmsetup suspend "$DM_DELAY_TARGET"
  if ! dmsetup reload "$DM_DELAY_TARGET" \
      --table "0 $size delay $THROTTLE_DEV 0 $rd_ms $THROTTLE_DEV 0 $wr_ms"; then
    dmsetup resume "$DM_DELAY_TARGET" || true
    return 1
  fi
  dmsetup resume "$DM_DELAY_TARGET"
}

apply_throttle() {
  # cgroup IO limits — only touched if non-zero. We write all four
  # axes (rbps/wbps/riops/wiops) explicitly so a previous "max"
  # entry doesn't leak through when we re-apply.
  if [ "$THROTTLE_IOPS" -gt 0 ] || [ "$THROTTLE_BPS" -gt 0 ]; then
    local rbps=max wbps=max riops=max wiops=max
    if [ "$THROTTLE_BPS" -gt 0 ]; then
      rbps="$THROTTLE_BPS"
      wbps="$THROTTLE_BPS"
    fi
    if [ "$THROTTLE_IOPS" -gt 0 ]; then
      riops="$THROTTLE_IOPS"
      wiops="$THROTTLE_IOPS"
    fi
    echo "$dev_id rbps=$rbps wbps=$wbps riops=$riops wiops=$wiops" \
      > "$cg_io_max"
  fi
  if [ "$DM_DELAY_MS" -gt 0 ]; then
    reload_delay "$DM_DELAY_MS" "$DM_DELAY_MS"
  fi
}

clear_throttle() {
  if [ "$THROTTLE_IOPS" -gt 0 ] || [ "$THROTTLE_BPS" -gt 0 ]; then
    echo "$dev_id rbps=max wbps=max riops=max wiops=max" > "$cg_io_max"
  fi
  if [ "$DM_DELAY_MS" -gt 0 ]; then
    reload_delay 0 0
  fi
}

# Daemon shutdown handler. `systemctl stop` sends SIGTERM and waits
# briefly before SIGKILL. Without this trap, a `stop` mid-`apply`
# leaves dm-delay suspended (correctness hazard #26/#27 from the
# adversarial review). We clear the throttle on the way out so the
# device is guaranteed unsuspended for any subsequent cleanup.
shutdown() {
  clear_throttle 2>/dev/null || true
  exit 0
}
trap shutdown TERM INT

# Start in "throttle off" state so fixture build / pre-warm /
# cleanup all run unthrottled.
clear_throttle

# Tight polling loop. /tmp is tmpfs in the bench setup so each
# `test -e` is sub-microsecond; the 1 ms sleep matches the bench
# side's poll cadence. We don't use inotifywait because the
# polling-side overhead is already in the noise floor of the
# measurements (~ms ≪ s) and a plain bash loop is one less
# package in the closure.
while true; do
  if [ -e /tmp/.bench_throttle_on ]; then
    apply_throttle
    : > /tmp/.bench_throttle_ack_on
    # Bench unlinks the request before continuing; if it doesn't
    # we'd loop and re-apply identically, which is a no-op.
    while [ -e /tmp/.bench_throttle_on ]; do
      sleep 0.001
    done
  fi
  if [ -e /tmp/.bench_throttle_off ]; then
    clear_throttle
    : > /tmp/.bench_throttle_ack_off
    while [ -e /tmp/.bench_throttle_off ]; do
      sleep 0.001
    done
  fi
  sleep 0.001
done
