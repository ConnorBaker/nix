# nix-store-bench NixOS test module. Imported into `adhoc.nix`'s
# `evalTest` call alongside `bench-options.nix`. Reads bench config
# from `cfg = config.bench` and emits `nodes.machine` + `testScript`.
#
# The Python testScript proper lives in `./testScript.py`; this file
# only constructs a small constants prelude (the Nix-side config
# values that the Python references) and concatenates it.
{
  config,
  lib,
  pkgs,
  nixComponents,
  ...
}:

let
  cfg = config.bench;

  benchPkg = nixComponents.nix-store-tests.override {
    withBenchmarks = true;
  };

  decide = pkgs.writers.writePython3Bin "bench-decide" { } (
    builtins.readFile ./decide.py
  );

  # Throttle daemon (see ./throttle-daemon.sh). `writeShellApplication`
  # wraps the script with an explicit PATH built from `runtimeInputs`
  # so `stat` / `blockdev` / `dmsetup` resolve under `systemd-run`'s
  # stripped-down environment.
  throttleDaemon = pkgs.writeShellApplication {
    name = "bench-throttle-daemon";
    runtimeInputs = with pkgs; [
      coreutils
      util-linux
      lvm2.bin
    ];
    text = builtins.readFile ./throttle-daemon.sh;
  };

  # Render a Nix value as a Python literal. Booleans, numbers, and
  # null map to their Python keywords; strings round-trip through
  # `builtins.toJSON` so embedded quotes/backslashes survive.
  pyVal = v:
    if v == null then "None"
    else if v == true then "True"
    else if v == false then "False"
    else if builtins.isInt v then toString v
    else if builtins.isString v then builtins.toJSON v
    else throw "pyVal: unsupported type for value ${toString v}";

  # Constants prelude: every dynamic value the Python testScript
  # depends on, materialised as a top-level binding. testScript.py
  # references these names assuming they're already in scope (it's
  # not a stand-alone module — see its header comment).
  testScriptPrelude = ''
    FS = "${cfg.fs}"
    USE_BLOCK_DEV = ${pyVal cfg.useBlockDev}
    DM_DELAY_MS = ${toString cfg.dmDelayMs}
    THROTTLE_IOPS: int | None = ${pyVal (
      if cfg.throttleParams != null then cfg.throttleParams.iops else null
    )}
    THROTTLE_BPS: int | None = ${pyVal (
      if cfg.throttleParams != null then cfg.throttleParams.bps else null
    )}
    MKFS_FLAG = "${if cfg.fsMkfsFlag != null then cfg.fsMkfsFlag else ""}"
    NPATHS = ${toString cfg.nPaths}
    THREADS = ${toString cfg.threads}
    THREADS2: int | None = ${pyVal cfg.threads2}
    REPS = ${toString cfg.benchRepetitions}
    BENCH_NAME = "${cfg.benchName}"
    LAYOUT = "${cfg.layout}"
    REPLICA = "${cfg.replica}"
    MULTI_PROCESS = ${pyVal cfg.multiProcess}
    DISPATCH_ONLY: str | None = ${pyVal cfg.dispatchOnly}
    THROTTLE_GATE = ${pyVal cfg.throttleGate}
  '';
in

{
  inherit (cfg) name;

  nodes.machine = _: {
    virtualisation = {
      inherit (cfg) cores memorySize;
    } // lib.optionalAttrs cfg.useBlockDev {
      emptyDiskImages = [
        {
          size = cfg.diskSize;
          driveConfig.deviceExtraOpts.serial = "bench-disk";
        }
      ];
    };

    programs.bcc.enable = true;

    boot.kernelModules =
      lib.optionals (cfg.useBlockDev && cfg.dmDelayMs > 0) [ "dm-delay" ];
    boot.supportedFilesystems =
      lib.optionals (cfg.useBlockDev && cfg.fsModule != null) [ cfg.fsModule ];

    # ZFS pool create needs an 8-hex hostId.
    networking.hostId = lib.mkIf (cfg.fs == "zfs") "abcd1234";

    environment.systemPackages = with pkgs; [
      bpftrace
      perf
      jq
      benchPkg
      decide
    ]
    ++ lib.optionals cfg.multiProcess [ nixComponents.nix-cli ]
    ++ lib.optionals cfg.throttleGate [ throttleDaemon ];
  };

  testScript = _: testScriptPrelude + builtins.readFile ./testScript.py;
}
