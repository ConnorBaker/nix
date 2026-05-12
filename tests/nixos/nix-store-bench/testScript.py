# NixOS test driver script for the nix-store-bench rig.
#
# This file is *not* a stand-alone Python module. `mk-test.nix`
# prepends a constants prelude (FS, USE_BLOCK_DEV, DM_DELAY_MS, …)
# computed from the Nix-side `bench-options.nix` config, then appends
# this file's contents verbatim to form the `testScript` consumed by
# `nixos-lib.runTest`. The runTest harness then runs it inside the
# Python test driver, with `machine` and `vlan1` exposed as globals.
#
# Because the constants are bound by the prelude, this file references
# them as if they were already in scope. Lint pedantically by reading
# `mk-test.nix`'s prelude block for the authoritative list.
#
# pyright: reportUndefinedVariable=false


# ----- bench taxonomy (derived from BENCH_NAME) -----
#
# Computed in Python (next to BENCH_NAME) rather than threaded
# through Nix as separate config options. Each is a single-line
# predicate; declaring them as Nix-side derived options would add
# boilerplate without buying us anything to introspect.
HAS_DISPATCH = BENCH_NAME in ("gc_barabasi", "gc_clusters")
HAS_VARIANT = BENCH_NAME not in ("optimise_migrate", "invalidate_paths")
HAS_THREADS2 = BENCH_NAME == "optimise_with_concurrent_gc"


def make_filter(dispatch: str | None) -> str:
    """Build a `--benchmark_filter` regex for one bench cell.

    google-benchmark expands BENCHMARK_CAPTURE rows as
    `<bench>/<capture-tag>/<args>/manual_time`. For GC benches the
    capture tag is `<dispatch>_<variant>`; for non-dispatch benches
    with a variant it's just `<variant>`; for benches without a
    variant the capture portion is absent.

    The `^...$` anchors prevent a longer-named cell (e.g. a future
    `1200/4/manual_time`) from matching a filter for
    `200/4/manual_time`.
    """
    parts = [BENCH_NAME]
    if HAS_VARIANT:
        # BENCHMARK_CAPTURE variant tag — `optimise-bench.cc` uses
        # this exact compound name as the C++ tag.
        variant = f"{LAYOUT}_{REPLICA}_replica_hardlink"
        if HAS_DISPATCH:
            assert dispatch is not None
            parts.append(f"{dispatch}_{variant}")
        else:
            parts.append(variant)
    parts.append(str(NPATHS))
    if BENCH_NAME != "invalidate_paths":
        parts.append(str(THREADS))
        if HAS_THREADS2:
            assert THREADS2 is not None
            parts.append(str(THREADS2))
    parts.append("manual_time")
    return "^" + "/".join(parts) + "$"


machine.start()
machine.wait_for_unit("multi-user.target")

if USE_BLOCK_DEV:
    if DM_DELAY_MS > 0:
        machine.succeed("modprobe dm-delay")
    dev = machine.succeed(
        "readlink -f /dev/disk/by-id/virtio-bench-disk"
    ).strip()
    print(f"empty disk: dev={dev}")

    # dm-delay is created with delay=0 initially regardless of the
    # cell's configured DM_DELAY_MS. The throttle daemon reloads it
    # to DM_DELAY_MS ms only while `timedCall` is running, so setup
    # writes don't pay the artificial latency.
    if DM_DELAY_MS > 0:
        size = machine.succeed(f"blockdev --getsz {dev}").strip()
        machine.succeed(
            f"dmsetup create slow --table "
            f"'0 {size} delay {dev} 0 0 {dev} 0 0'"
        )
        target_dev = "/dev/mapper/slow"
    else:
        target_dev = dev

    if FS == "zfs":
        # ZFS: zpool not mkfs. ashift=12 = 4 KiB blocks (right for
        # any modern SSD); atime=off avoids write-amp on our
        # read-heavy workloads.
        machine.succeed(
            f"zpool create -f -o ashift=12 "
            f"-O atime=off -O mountpoint=/mnt/slow testpool {target_dev}"
        )
    else:
        machine.succeed(f"mkfs.{FS} {MKFS_FLAG} {target_dev}")
        machine.succeed(
            f"mkdir -p /mnt/slow && mount {target_dev} /mnt/slow"
        )

    # cgroup `io.max` replaces the QEMU block-level throttle. The
    # cgroup is VM-controllable (QMP is host-only) so the in-VM
    # throttle daemon can toggle it around each measured call.
    if THROTTLE_GATE:
        # Pre-create the slice with a `sleep infinity` placeholder.
        # Until something runs in the slice its cgroup dir doesn't
        # exist and the daemon can't write `io.max`.
        machine.succeed(
            "systemd-run --slice=benchthrottle.slice "
            "--unit=bench-throttle-keepalive.service "
            "/run/current-system/sw/bin/sleep infinity"
        )
        machine.succeed(
            "for i in $(seq 1 200); do "
            "  [ -d /sys/fs/cgroup/benchthrottle.slice ] && break; "
            "  sleep 0.05; "
            "done; "
            "test -d /sys/fs/cgroup/benchthrottle.slice"
        )
        daemon_env = {
            "THROTTLE_DEV": dev,
            "THROTTLE_IOPS": THROTTLE_IOPS or 0,
            "THROTTLE_BPS": THROTTLE_BPS or 0,
            "DM_DELAY_MS": DM_DELAY_MS,
        }
        setenv_args = " ".join(
            f"--setenv={k}={v}" for k, v in daemon_env.items()
        )
        machine.succeed(
            f"systemd-run --unit=bench-throttle-daemon.service "
            f"{setenv_args} bench-throttle-daemon"
        )
        machine.succeed(
            "for i in $(seq 1 100); do "
            "  systemctl is-active --quiet bench-throttle-daemon "
            "    && break; "
            "  sleep 0.05; "
            "done; "
            "systemctl is-active --quiet bench-throttle-daemon"
        )
        print(f"throttle daemon started: {daemon_env}")
else:
    machine.succeed(
        "mkdir -p /mnt/slow && "
        "mount -t tmpfs -o size=8G tmpfs /mnt/slow"
    )

# Dispatches we exercise. GC benches default to both syscall and
# iouring (the A/B `decide.py` compares); `DISPATCH_ONLY` restricts
# to one. Non-GC benches run as a single un-prefixed cell.
if HAS_DISPATCH:
    dispatches: list[str | None] = (
        [DISPATCH_ONLY] if DISPATCH_ONLY is not None
        else ["syscall", "iouring"]
    )
else:
    dispatches = [None]

# bpftrace gates counters on the bench's `/tmp/.bench_gc_start` and
# `/tmp/.bench_gc_end` openat markers so counts reflect only the
# timed call, not the fixture build / warm-up. Only the GC benches
# emit these markers.
#
# The `comm == "nix-bench"` filter matches what `bench-main.cc` sets
# via `pthread_setname_np`. Stable across binary renames; don't
# change to match the truncated binary name.
for dispatch in dispatches:
    tag = dispatch if dispatch is not None else "single"
    if HAS_DISPATCH:
        # `pid` is the userspace PID (= kernel TGID), `tid` is the
        # kernel TID (per-thread). We record `pid` at the start
        # marker and match `pid == @bench_pid` on every syscall, so
        # TBB worker threads are counted along with main.
        #
        # The do_unlinkat / vfs_unlink kprobes are gated by `@on`
        # only (not pid) so io-wq kernel workers serving the bench's
        # ring are counted. In multi-process mode, a sibling
        # `bench-noise.service` runs optimise on the same store; we
        # additionally gate kprobes on `pid == @bench_pid` then to
        # avoid contamination. (The io_uring variant isn't measured
        # under multi-process anyway; this is for decide.py's
        # VFS-parity check on the syscall side.)
        kprobe_gate = (
            "/@on == 1 && pid == @bench_pid/" if MULTI_PROCESS
            else "/@on == 1/"
        )
        machine.succeed(f"""
          ulimit -n 4096
          bpftrace -e '
            tracepoint:syscalls:sys_enter_openat
              /str(args->filename) == "/tmp/.bench_gc_start"
                && comm == "nix-bench"/
              {{ @on = 1; @bench_pid = pid; }}
            tracepoint:syscalls:sys_enter_openat
              /str(args->filename) == "/tmp/.bench_gc_end"
                && comm == "nix-bench"/
              {{ @on = 0; }}
            kprobe:do_unlinkat {kprobe_gate} {{
              @vfs["kprobe:do_unlinkat"] = count();
            }}
            kprobe:vfs_unlink {kprobe_gate} {{
              @vfs["kprobe:vfs_unlink"] = count();
            }}
            tracepoint:raw_syscalls:sys_enter
              /@on == 1 && pid == @bench_pid/
              {{ @sc[args->id] = count(); }}
          ' </dev/null >/tmp/{tag}.bpf.txt 2>/tmp/{tag}.bpf.log &
          echo $! >/tmp/bpf.pid
          disown
          for i in $(seq 1 30); do
            if grep -q "Attaching" /tmp/{tag}.bpf.log 2>/dev/null; then
              break
            fi
            sleep 0.5
          done
          sleep 1
          if grep -q "ERROR:" /tmp/{tag}.bpf.log 2>/dev/null; then
            echo "bpftrace failed to start; see /tmp/{tag}.bpf.log" >&2
            cat /tmp/{tag}.bpf.log >&2
            exit 1
          fi
        """)

    filt = make_filter(dispatch)

    # Multi-process mode runs a concurrent `nix-store --optimise`
    # against the same store the bench is operating on. Models the
    # Hydra/build-farm contention pattern the in-process
    # `optimise_with_concurrent_gc` bench can't reach.
    if MULTI_PROCESS:
        shared_root = "/mnt/slow/shared-bench"
        sentinel = "/tmp/bench-noise-cycle"
        machine.succeed(f"rm -rf {shared_root} && mkdir -p {shared_root}")
        machine.succeed(f"rm -f {sentinel}")
        machine.succeed(
            "systemctl reset-failed bench-noise.service 2>/dev/null || true"
        )
        # The noise loop touches `sentinel` after each optimise
        # cycle. We wait for the first touch before launching the
        # bench so the first iteration runs against a populated
        # `.links/`, not an empty one.
        machine.succeed(
            f"systemd-run --unit=bench-noise --collect -- "
            f"bash -c 'while :; do "
            f"nix store --store \"local?root={shared_root}\" "
            f"optimise >/dev/null 2>&1 || true; "
            f"touch {sentinel}; "
            f"done'"
        )
        # Up to 60s for the sentinel — first optimise cycle on big
        # nPaths can take a while.
        machine.succeed(
            f"for i in $(seq 1 120); do "
            f"  [ -f {sentinel} ] && break; "
            f"  sleep 0.5; "
            f"done; "
            f"test -f {sentinel}"
        )
        bench_env = f"NIX_BENCH_STORE_ROOT={shared_root} "
        bench_reps = 1
    else:
        bench_env = ""
        bench_reps = REPS

    # When the throttle gate is active, run the bench inside
    # `benchthrottle.slice` so the daemon's `io.max` apply/clear
    # actually affects its I/O. Bench opts in via
    # NIX_BENCH_THROTTLE_GATE=1; without it the marker handshake is
    # skipped (daemon idles, slice remains unlimited).
    if THROTTLE_GATE:
        machine.succeed(
            "rm -f /tmp/.bench_throttle_on /tmp/.bench_throttle_off "
            "        /tmp/.bench_throttle_ack_on "
            "        /tmp/.bench_throttle_ack_off"
        )
        gate_env = "NIX_BENCH_THROTTLE_GATE=1 "
        # `--scope` runs the bench in the foreground under
        # `systemd-run` without spawning a transient service; same
        # cgroup placement as `--unit` but `machine.succeed` can
        # block on completion.
        run_prefix = "systemd-run --scope --slice=benchthrottle.slice -- "
    else:
        gate_env = ""
        run_prefix = ""

    machine.succeed(
        f"cd /mnt/slow && TMPDIR=/mnt/slow {bench_env}{gate_env}"
        f"{run_prefix}nix-store-benchmarks "
        f"--benchmark_filter='{filt}' "
        f"--benchmark_repetitions={bench_reps} "
        f"--benchmark_min_time=1x "
        f"--benchmark_out=/tmp/{tag}.json "
        f"--benchmark_out_format=json "
        f">/tmp/{tag}.stdout 2>&1"
    )

    # google-benchmark exits 0 even when the filter matches no cells
    # (it logs "Failed to match any benchmarks against regex: ..." and
    # writes an empty JSON). Fail loudly here.
    stdout_text = machine.succeed(f"cat /tmp/{tag}.stdout || true")
    if "Failed to match any benchmarks" in stdout_text:
        raise Exception(
            f"bench filter matched no benchmarks (filter={filt!r}). "
            f"Check BENCHMARK_CAPTURE rows in "
            f"src/libstore-tests/optimise-bench.cc — typical valid "
            f"nPaths are 200/2000/10000/50000, and not every "
            f"(variant, dispatch, threads) is registered at every size."
        )

    if MULTI_PROCESS:
        machine.succeed(
            "systemctl stop bench-noise.service 2>/dev/null || true"
        )
        machine.succeed(f"rm -rf {shared_root}")

    if HAS_DISPATCH:
        machine.succeed(
            "PID=$(cat /tmp/bpf.pid); "
            "kill -INT $PID 2>/dev/null || true; "
            "for i in $(seq 1 10); do "
            "  if ! kill -0 $PID 2>/dev/null; then break; fi; "
            "  sleep 1; "
            "done; "
            "kill -KILL $PID 2>/dev/null || true; "
            "sync"
        )

if THROTTLE_GATE:
    # Daemon + keepalive live for the test's lifetime; teardown is
    # best-effort (a failed stop on a not-running unit is fine).
    machine.succeed(
        "systemctl stop bench-throttle-daemon.service "
        "bench-throttle-keepalive.service 2>/dev/null || true"
    )

# Derive `out_tags` from the dispatches that actually ran. Hardcoding
# `["syscall", "iouring"]` for HAS_DISPATCH would dump `(read failed:
# status 1)` noise for the unused dispatch under DISPATCH_ONLY.
out_tags = [d if d is not None else "single" for d in dispatches]
out_exts = ["json", "stdout"] + (["bpf.txt", "bpf.log"] if HAS_DISPATCH else [])

for tag in out_tags:
    for ext in out_exts:
        path = f"/tmp/{tag}.{ext}"
        try:
            machine.copy_from_vm(path, "")
        except Exception as e:
            print(f"warning: failed to copy {path}: {e}")

for tag in out_tags:
    for ext in ("json", "stdout") + (("bpf.txt",) if HAS_DISPATCH else ()):
        print(f"=== {tag}.{ext} ===")
        status, output = machine.execute(f"cat /tmp/{tag}.{ext}")
        print(output if status == 0 else f"(read failed: status {status})")

# Both decide.py modes return non-zero on failure conditions (empty
# iteration rows, uncaught throws, threshold violations); propagate
# that as a NixOS test failure rather than just logging.
if HAS_DISPATCH:
    # Symmetric existence check over the dispatches that actually ran.
    # An earlier version hardcoded `test -s /tmp/syscall.json` and so
    # always tripped under DISPATCH_ONLY="iouring" (syscall never runs).
    # Reuse the `dispatch` loop variable from the run loop above so its
    # `str | None` type is preserved; the assert encodes the invariant
    # that `dispatches` holds only strings under HAS_DISPATCH.
    for dispatch in dispatches:
        assert dispatch is not None
        status, _ = machine.execute(f"test -s /tmp/{dispatch}.json")
        if status != 0:
            raise Exception(
                f"{dispatch} bench JSON missing or empty after run "
                f"(test -s exit={status}); see stdout dumps above."
            )
    # A/B mode needs both JSONs; when dispatchOnly is set we skip the
    # comparison and just summarise the one dispatch that did run.
    if DISPATCH_ONLY is None:
        status, output = machine.execute(
            "bench-decide ab "
            "/tmp/syscall.json /tmp/iouring.json "
            "/tmp/syscall.bpf.txt /tmp/iouring.bpf.txt"
        )
        print(output)
        print(f"decide exit status: {status}")
        if status != 0:
            raise Exception(
                f"bench-decide A/B FAILED (exit {status}). "
                f"See output above for which criterion missed."
            )
    else:
        one = DISPATCH_ONLY
        status, output = machine.execute(
            f"bench-decide summary /tmp/{one}.json"
        )
        print(output)
        print(f"summarise exit status: {status}")
        if status != 0:
            raise Exception(
                f"bench-decide summary FAILED for {one} (exit {status})."
            )
else:
    status_one, _ = machine.execute("test -s /tmp/single.json")
    if status_one != 0:
        raise Exception(
            f"bench JSON missing or empty (test -s exit={status_one})."
        )
    status, output = machine.execute("bench-decide summary /tmp/single.json")
    print(output)
    print(f"summarise exit status: {status}")
    if status != 0:
        raise Exception(
            f"bench-decide summary FAILED (exit {status}). "
            f"This indicates no iteration rows in the JSON or "
            f"the bench reported uncaught throws."
        )
