# Configuration schema for the nix-store-bench VM scenarios.
# Module imported into `adhoc.nix`'s evalTest. Type-checks inputs,
# applies defaults, cross-validates combinations via assertions.
#
# `runTest`'s nixosTest class doesn't declare `options.assertions` or
# `options.warnings` (see `nixos/lib/testing/legacy.nix:11`), so this
# module declares both and `adhoc.nix` surfaces them manually after
# evalTest — `throw` on failed assertions, `lib.showWarnings` for
# soft warnings.
{ config, lib, ... }:

let
  inherit (lib) mkOption types;
  cfg = config.bench;
in

{
  options.bench = {
    name = mkOption {
      type = types.str;
      default =
        # Auto-derived name. Same shape as the current-branch rig
        # (`-<dispatch>-<fs>-<throttle>-<layout>-<replica>-n<N>-t<T>`)
        # so the existing `bench.py` NAME_RE / `bench.py ab` pair a
        # baseline cell with its current-branch counterpart by
        # derivation-name parsing alone. Baseline synthesises the
        # fixed axes: GC benches embed `-syscall` (io_uring doesn't
        # exist here); all benches carry `-flat-multi` (no sharded
        # layout, no replica cap at this commit).
        let
          dash = s: builtins.replaceStrings [ "_" ] [ "-" ] s;
          isGCBench = cfg.benchName == "gc_barabasi" || cfg.benchName == "gc_clusters";
          dispatchSuffix = if isGCBench then "-syscall" else "";
          multiSuffix = if cfg.multiProcess then "-multi" else "";
        in
        "adhoc-${dash cfg.benchName}${dispatchSuffix}"
        + "-${cfg.fs}-${cfg.throttle}"
        + "-flat-multi"
        + "-n${toString cfg.nPaths}-t${toString cfg.threads}"
        + "${multiSuffix}";
      defaultText = lib.literalExpression ''
        "adhoc-<benchName>[-syscall]-<fs>-<throttle>-flat-multi-n<nPaths>-t<threads>[-multi]"
      '';
      description = ''
        Test derivation name. Defaults to a shape that matches the
        full-rig NAME_RE so `bench.py ab` parses baseline and
        current-branch names identically — the fixed `-syscall` /
        `-flat-multi` substrings stand in for the axes that don't
        exist at this commit.
      '';
    };

    fs = mkOption {
      type = types.enum [ "ext4" "xfs" "btrfs" "zfs" "tmpfs" ];
      default = "ext4";
      description = ''
        Filesystem mounted on the bench's empty disk. `tmpfs` skips
        the block device and uses a RAM mount (no throttle applies).
      '';
    };

    throttle = mkOption {
      type = types.enum [ "gp3" "io2" "nvme" "none" ];
      default = "gp3";
      description = ''
        cgroup `io.max` profile applied while the gate is active:
        `gp3` ~ 3000 IOPS / 125 MiB/s, `io2` ~ 16000 / 500,
        `nvme` ~ 100k / 1 GiB (effectively unthrottled), `none`
        disables the cgroup limit. Ignored when `fs == "tmpfs"`.
      '';
    };

    dmDelayMs = mkOption {
      type = types.ints.unsigned;
      default = 5;
      description = ''
        Per-I/O dm-delay (ms) while the gate is active; 0 disables.
        Reloaded around `timedCall` so fixture build is unthrottled.
      '';
    };

    nPaths = mkOption {
      type = types.ints.positive;
      default = 50000;
      description = ''
        Fixture size. Must match a registered BENCHMARK row — the
        baseline port registers {2000, 10000, 50000} paths for each
        of `optimise`, `gc_barabasi`, `gc_clusters`.
      '';
    };

    threads = mkOption {
      type = types.ints.positive;
      default = 4;
      description = ''
        Worker thread count. Recorded in the bench name for
        host-side A/B parity with the current-branch rig; inert at
        baseline (no optimise-threads / gc-links-threads settings
        exist at this commit — bench bodies are single-threaded).
      '';
    };

    threads2 = mkOption {
      type = types.nullOr types.ints.positive;
      default = null;
      description = ''
        Vestigial option retained only so
        `bench.py` run-matrix code that sets `threads2` on non-GC
        cells doesn't fail at schema time. Unused here.
      '';
    };

    benchRepetitions = mkOption {
      type = types.ints.positive;
      default = 3;
      description = ''
        `--benchmark_repetitions`. Forced to 1 when `multiProcess`
        is set (BenchFixture can't re-init a populated store).
      '';
    };

    benchName = mkOption {
      type = types.enum [
        "gc_barabasi"
        "gc_clusters"
        "optimise"
      ];
      default = "gc_barabasi";
      description = ''
        Bench function in `optimise-bench.cc`. The baseline-port
        bench binary only registers `optimise`, `gc_barabasi`, and
        `gc_clusters` — all in a single serial variant, since the
        baseline code pre-dates the sharded-links, replica-spill,
        and io_uring-dispatch work. The
        `optimise_migrate` / `optimise_with_concurrent_gc` /
        `invalidate_paths` benches from the full rig are absent here
        because the APIs they exercise don't exist at this commit.
      '';
    };

    cores = mkOption {
      type = types.ints.positive;
      default = 8;
      description = ''
        VM hardware-thread count. Should be at least `threads + 2`;
        `threads = 16` cells use `cores = 24`.
      '';
    };

    memorySize = mkOption {
      type = types.ints.positive;
      default = 8192;
      description = "VM RAM in MiB.";
    };

    diskSize = mkOption {
      type = types.ints.positive;
      default = 16384;
      description = ''
        Empty disk image size in MiB. Unused when `fs == "tmpfs"`.
        Roughly 1 GiB per 10k paths at the canonical density.
      '';
    };

    multiProcess = mkOption {
      type = types.bool;
      default = false;
      description = ''
        Run a sibling `nix-store --optimise` loop against the same
        store via NIX_BENCH_STORE_ROOT. Models cross-process
        contention; forces `benchRepetitions = 1`.
      '';
    };

    # Derived data — `readOnly` so users can only set the inputs
    # above; the consumer (mk-test.nix) reads `cfg.<name>` directly
    # instead of duplicating these lookups in its own let-block.

    useBlockDev = mkOption {
      internal = true;
      readOnly = true;
      type = types.bool;
      description = "True iff `fs` mandates a real block device.";
    };

    throttleParams = mkOption {
      internal = true;
      readOnly = true;
      type = types.nullOr (types.submodule {
        options = {
          iops = mkOption { type = types.ints.positive; };
          bps = mkOption { type = types.ints.positive; };
        };
      });
      description = ''
        Per-throttle IOPS/BPS pair, or null when `throttle == "none"`.
      '';
    };

    fsMkfsFlag = mkOption {
      internal = true;
      readOnly = true;
      type = types.str;
      description = ''
        `mkfs.<fs>` flags; empty string when no mkfs runs (tmpfs / zfs).
      '';
    };

    fsModule = mkOption {
      internal = true;
      readOnly = true;
      type = types.nullOr types.str;
      description = ''
        Kernel module / userspace package name for
        `boot.supportedFilesystems`, or null for tmpfs.
      '';
    };

    throttleGate = mkOption {
      internal = true;
      readOnly = true;
      type = types.bool;
      description = ''
        True iff the dynamic-throttle daemon should run for this
        scenario (i.e. there's a real device and at least one of
        cgroup throttle or dm-delay is configured).
      '';
    };
  };

  options.assertions = mkOption {
    type = types.listOf (types.submodule {
      options = {
        assertion = mkOption { type = types.bool; };
        message = mkOption { type = types.str; };
      };
    });
    default = [ ];
    internal = true;
    description = "Cross-option preconditions; surfaced by adhoc.nix.";
  };

  options.warnings = mkOption {
    type = types.listOf types.str;
    default = [ ];
    internal = true;
    description = "Soft warnings printed by adhoc.nix via lib.showWarnings.";
  };

  config.bench = {
    useBlockDev = cfg.fs != "tmpfs";

    throttleParams = {
      gp3 = { iops = 3000; bps = 131072000; };
      io2 = { iops = 16000; bps = 524288000; };
      nvme = { iops = 100000; bps = 1073741824; };
      none = null;
    }.${cfg.throttle};

    # ext4/xfs/btrfs use mkfs; zfs uses zpool; tmpfs is mount-only.
    fsMkfsFlag = {
      ext4 = "-F";
      xfs = "-f -m crc=1";
      btrfs = "-f";
      zfs = "";
      tmpfs = "";
    }.${cfg.fs};

    fsModule = {
      ext4 = "ext4";
      xfs = "xfs";
      btrfs = "btrfs";
      zfs = "zfs";
      tmpfs = null;
    }.${cfg.fs};

    throttleGate =
      cfg.useBlockDev && (cfg.throttleParams != null || cfg.dmDelayMs > 0);
  };

  config.warnings = lib.optionals (cfg.multiProcess && cfg.benchRepetitions != 1) [
    ''
      bench.multiProcess = true silently overrides benchRepetitions
      to 1 (BenchFixture's ctor can't re-init a populated store).
      You supplied benchRepetitions = ${toString cfg.benchRepetitions};
      the VM will use 1 anyway. Set benchRepetitions = 1 to silence.
    ''
  ];

  config.assertions = [ ];
  # `nPaths` is validated by the bench binary itself: an unregistered
  # value surfaces as "Failed to match any benchmarks" at run time
  # (see test_script.py). The registered cells live in
  # `src/libstore-tests/optimise-bench.cc`.
}
