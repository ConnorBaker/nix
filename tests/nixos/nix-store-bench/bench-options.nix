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
        # Auto-derived: every distinguishing field in the test name
        # so two scenarios don't collide on the same derivation hash.
        # Underscores → hyphens for nix-friendliness.
        let
          dash = s: builtins.replaceStrings [ "_" ] [ "-" ] s;
          suffix = if cfg.multiProcess then "-multi" else "";
        in
        "adhoc-${dash cfg.benchName}-${cfg.fs}-${cfg.throttle}"
        + "-${cfg.layout}-${cfg.replica}"
        + "-n${toString cfg.nPaths}-t${toString cfg.threads}${suffix}";
      defaultText = lib.literalExpression ''
        "adhoc-<benchName>-<fs>-<throttle>-<layout>-<replica>-n<nPaths>-t<threads>[-multi]"
      '';
      description = ''
        Test derivation name. Defaults to an auto-derived string
        that includes every distinguishing field.
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
        Fixture size. Must match a registered BENCHMARK_CAPTURE cell
        for the chosen `benchName` / `layout` / `replica` — checked
        by the `nPaths` assertion below.
      '';
    };

    threads = mkOption {
      type = types.ints.positive;
      default = 4;
      description = ''
        Worker thread count. Drives gc-links-threads for GC benches,
        optimise-threads for optimise benches, ignored for
        `invalidate_paths`.
      '';
    };

    threads2 = mkOption {
      type = types.nullOr types.ints.positive;
      default = null;
      description = ''
        Second thread axis (gc-links-threads). Required for
        `optimise_with_concurrent_gc`; ignored otherwise.
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
        "optimise_migrate"
        "optimise_with_concurrent_gc"
        "invalidate_paths"
      ];
      default = "gc_barabasi";
      description = ''
        Bench function in `optimise-bench.cc`. GC families
        (`gc_barabasi`/`gc_clusters`) have an io_uring×syscall A/B
        axis; others run a single dispatch.
      '';
    };

    layout = mkOption {
      type = types.enum [ "flat" "sharded" ];
      default = "flat";
      description = ''
        `.links/` layout: `flat` (legacy) or `sharded`
        (`Xp::ShardedLinks`). Composed with `replica` into the
        BENCHMARK_CAPTURE tag `<layout>_<replica>_replica_hardlink`
        at filter-build time. Ignored for `invalidate_paths` and
        `optimise_migrate`.
      '';
    };

    replica = mkOption {
      type = types.enum [ "single" "multi" ];
      default = "single";
      description = ''
        Replica policy: `single` (max-link-replicas=1) or `multi`
        (max-link-replicas=100 + _link-max-override=100 so the spill
        path is exercised even on tmpfs/ZFS).
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

    dispatchOnly = mkOption {
      type = types.nullOr (types.enum [ "syscall" "iouring" ]);
      default = null;
      description = ''
        Restrict GC benches to one dispatch. `null` runs both for
        an A/B comparison. Set when a (variant, threads) cell is
        only registered on one side.
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
      type = types.nullOr types.str;
      description = ''
        `mkfs.<fs>` flags, or null when no mkfs runs (tmpfs / zfs).
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
      zfs = null;
      tmpfs = null;
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

  config.warnings = lib.optionals
    (cfg.multiProcess && cfg.benchRepetitions != 1)
    [
      ''
        bench.multiProcess = true silently overrides benchRepetitions
        to 1 (BenchFixture's ctor can't re-init a populated store).
        You supplied benchRepetitions = ${toString cfg.benchRepetitions};
        the VM will use 1 anyway. Set benchRepetitions = 1 to silence.
      ''
    ];

  config.assertions =
    let
      isGCBench =
        cfg.benchName == "gc_barabasi" || cfg.benchName == "gc_clusters";

      # Dispatches the rig actually exercises for this scenario. For
      # GC benches with `dispatchOnly = null` it's both; the valid
      # nPaths set is the intersection.
      dispatchesToRun =
        if !isGCBench then [ null ]
        else if cfg.dispatchOnly != null then [ cfg.dispatchOnly ]
        else [ "syscall" "iouring" ];

      # The 200-path cell is only registered on baseline scenarios.
      # On GC benches it's syscall-only, so a dispatchOnly=null GC
      # run excludes it (iouring leg would fail).
      isBaseline = cfg.layout == "flat" && cfg.replica == "single";
      smallForDispatch = dispatch:
        (cfg.benchName == "gc_barabasi" && dispatch == "syscall" && isBaseline)
        || ((cfg.benchName == "optimise"
              || cfg.benchName == "optimise_with_concurrent_gc")
            && isBaseline);
      hasSmallCell = lib.all smallForDispatch dispatchesToRun;

      validNPaths =
        if cfg.benchName == "invalidate_paths" then [ 100 500 2000 10000 50000 ]
        else if hasSmallCell                   then [ 200 2000 10000 50000 ]
        else                                        [ 2000 10000 50000 ];
    in
    [
      {
        assertion =
          cfg.benchName == "optimise_with_concurrent_gc" -> cfg.threads2 != null;
        message = ''
          bench.benchName = "${cfg.benchName}" requires bench.threads2
          (the gc-links-threads axis).
        '';
      }
      {
        assertion = cfg.dispatchOnly == null || isGCBench;
        message = ''
          bench.dispatchOnly is only meaningful for GC benches
          (gc_barabasi / gc_clusters); got bench.benchName =
          "${cfg.benchName}".
        '';
      }
      {
        assertion = lib.elem cfg.nPaths validNPaths;
        message = ''
          bench.nPaths = ${toString cfg.nPaths} is not a registered
          BENCHMARK_CAPTURE cell for bench.benchName =
          "${cfg.benchName}" with bench.layout = "${cfg.layout}",
          bench.replica = "${cfg.replica}"${
            if cfg.dispatchOnly != null
            then " and bench.dispatchOnly = \"${cfg.dispatchOnly}\""
            else ""
          }.
          Valid: ${lib.concatStringsSep ", " (map toString validNPaths)}.
          See BENCHMARK_CAPTURE rows in
          src/libstore-tests/optimise-bench.cc.
        '';
      }
    ];
}
