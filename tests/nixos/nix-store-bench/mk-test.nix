# nix-store-bench NixOS test module. Imported into `adhoc.nix`'s
# `evalTest` call alongside `bench-options.nix`. Reads bench config
# from `cfg = config.bench` and emits `nodes.machine` + `testScript`.
#
# The Python testScript proper lives in `./test_script.py`; this file
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
  # depends on, materialised as a top-level binding. test_script.py
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
    MKFS_FLAG = "${cfg.fsMkfsFlag}"
    NPATHS = ${toString cfg.nPaths}
    THREADS = ${toString cfg.threads}
    THREADS2: int | None = ${pyVal cfg.threads2}
    REPS = ${toString cfg.benchRepetitions}
    BENCH_NAME = "${cfg.benchName}"
    LAYOUT = "${cfg.layout}"
    REPLICA = "${cfg.replica}"
    MULTI_PROCESS = ${pyVal cfg.multiProcess}
    DISPATCH: str | None = ${pyVal cfg.dispatch}
    THROTTLE_GATE = ${pyVal cfg.throttleGate}
  '';
in

{
  inherit (cfg) name;

  nodes.machine = _: {
    virtualisation = {
      inherit (cfg) cores memorySize;
      # The bench doesn't talk to the network — `machine.succeed`
      # goes over virtio-serial, `copy_from_vm` goes over 9p, QMP
      # is a host-side UNIX socket. Drop the vlan NIC and the
      # default SLIRP NIC so we skip `systemd-networkd` /
      # `dhcpcd` / `network-online.target` startup cost. Saves
      # ~1-3 s per boot; across a 2500-cell matrix that's real.
      vlans = [ ];
      qemu.networkingOptions = lib.mkForce [ ];
      # `+x2apic` reduces APIC-access overhead on KVM guests
      # (microvm.nix also uses this). Does not change machine type
      # or device topology, so compatible with the default test
      # framework's -pc/-q35/virtio-pci wiring.
      #
      # x2apic is an x86 feature; on aarch64 qemu rejects the flag.
      # Use plain `-cpu host` there.
      #
      # Measured rejected / surprising:
      # - `+invtsc,+tsc-deadline,-pmu` + `-overcommit cpu-pm=on`:
      #     Boot was faster but the bench work itself slowed from
      #     ~150ms to ~225ms per iter on ext4+gp3+ram (~50%
      #     regression in the measured region). Best guess:
      #     interaction with `steady_clock` → vDSO TSC reads or
      #     HLT-exit disabling hurting the TBB workers' idle
      #     pathing. Either way, the whole point is fast *and*
      #     correct bench numbers; won't take a boot-time win for
      #     a runtime regression.
      # - `-nodefaults`: haven't measured in isolation; likely
      #     harmless but untested.
      qemu.options =
        if pkgs.stdenv.hostPlatform.isx86_64
        then [ "-cpu" "host,+x2apic" ]
        else [ "-cpu" "host" ];
      # No framebuffer / virtio-gpu: the test driver talks to the
      # guest over hvc0, never over a vt. Removing the GPU device
      # also skips a virtio_gpu initrd module.
      graphics = false;
      # The bench never writes into /nix/store — binaries are read
      # from the mounted host store, working data lives under
      # /mnt/slow or /tmp. Skip the tmpfs overlay that
      # `mountHostNixStore` would otherwise stack on top of the
      # host-store 9p mount, saving the overlay setup cost in
      # stage 1.
      writableStore = false;
      # Intentionally *not* setting `useNixStoreImage = true`. Per
      # its docstring it builds the closure image "just-in-time"
      # every VM start — a tar extract running in stage 1 before
      # systemd starts. For our short bench cells (tmpfs ~5s total)
      # that extract alone added ~5s on tmpfs and +7% on xfs;
      # measured net-negative across every fs tier. The win comes
      # from eliminating 9p mmap/dlopen latency, which only pays
      # off on a much-longer bench that actually hits the store
      # hundreds of times per run.
    } // lib.optionalAttrs cfg.useBlockDev {
      # The bench's block device is a qcow2 empty disk attached via
      # virtio-blk. For cross-host-comparable wall times, point the
      # Nix daemon's `build-dir` at a tmpfs (e.g. set
      # `--option build-dir /dev/shm` on the `nix build` invocation
      # or configure the daemon globally) so the qcow2 lives in RAM
      # rather than on whichever filesystem the host happens to
      # mount /tmp from. See README > "Running the matrix" for
      # the full portability note.
      emptyDiskImages = [
        {
          size = cfg.diskSize;
          driveConfig.deviceExtraOpts.serial = "bench-disk";
        }
      ];
    };

    # Matches the `vlans = [ ]` above: with no NIC there's nothing
    # for DHCP or the resolver to manage, and keeping them enabled
    # wastes startup time on units that will never settle.
    networking.useDHCP = false;
    networking.useNetworkd = false;
    services.resolved.enable = false;

    # Fast-boot knobs for a bench that only touches filesystems +
    # TBB workers + optional bpftrace. Every service/unit disabled
    # here costs ~30-500 ms of startup; aggregated across the
    # 2500-cell matrix, they compound.
    #
    # Safe because: no DNS lookups → no nscd; no crashes expected
    # in a passing bench → no coredump pipe; no memory pressure
    # model → no oomd; no inbound/outbound packets → no firewall;
    # no vtty usage → no console-setup; no on-disk log consumers
    # → volatile journald (still forwarded to the serial console
    # by test-instrumentation, so failures remain debuggable).
    services.nscd.enable = false;
    system.nssModules = lib.mkForce [ ];
    systemd.coredump.enable = false;
    systemd.oomd.enable = false;
    networking.firewall.enable = false;
    console.enable = false;
    services.journald.storage = "volatile";

    # Profile-driven strips (from `systemd-analyze blame` on a
    # probe VM). Together these account for >500 ms of userspace
    # startup that the bench cells pay for nothing.
    services.dbus.enable = lib.mkForce false;  # ~72 ms + knock-on (logind/hostnamed); mkForce overrides systemd.nix's default
    networking.resolvconf.enable = false;# ~125 ms; no DNS anyway
    security.sudo.enable = false;        # bench runs as root
    security.wrappers = { };             # ~299 ms suid-sgid-wrappers.service
    services.timesyncd.enable = false;   # already off in qemu-vm, keep explicit

    # Trim boot-time chatter on the serial console. Kernel/udev
    # logging at the default level serialises every printk through
    # hvc0, which under KVM is a surprising fraction of boot wall
    # time. `mitigations=off` skips guest-entry retpolines on a
    # trusted bench host; `nowatchdog` skips softlockup detector
    # init; `random.trust_cpu=on` short-circuits the entropy-wait
    # on hosts with RDRAND. `8250.nr_uarts=1` probes just one
    # legacy UART instead of the default four (microvm.nix trick).
    boot.kernelParams = [
      "quiet"
      "loglevel=3"
      "udev.log_level=3"
      "systemd.show_status=false"
      "mitigations=off"
      "nowatchdog"
      "nmi_watchdog=0"
      "random.trust_cpu=on"
      "8250.nr_uarts=1"
      # `test-instrumentation.nix` forces `clocksource=acpi_pm` for
      # determinism under host load. We want fast, accurate timing
      # for the bench, not a frozen clock. `mkAfter` wins against
      # the forced default because later entries override earlier
      # ones on the kernel cmdline.
      "clocksource=kvm-clock"
    ];

    # Kill a few more disabled-by-default-but-enabled-by-module-
    # chain features. TPM2 is irrelevant in a bench; `swraid` and
    # the initrd TPM2 hook both run at stage-1-equivalent time and
    # cost metadata-table setup we don't need.
    boot.swraid.enable = false;
    systemd.tpm2.enable = false;
    boot.initrd.systemd.tpm2.enable = false;

    # One-shot units that don't contribute to a bench run but do
    # serialise early-boot: journal flush (no on-disk journal
    # anyway), utmp bookkeeping, random-seed persistence. The
    # network-setup/resolvconf units would re-appear via their
    # module's other hooks even with networkd off, so pin them
    # off here too. `lastlog2-import` parses /var/log/lastlog2 on
    # every boot; nobody's logging in.
    systemd.suppressedSystemUnits = [
      "systemd-journal-flush.service"
      "systemd-journal-catalog-update.service"
      "systemd-random-seed.service"
      "systemd-update-utmp.service"
      "systemd-update-done.service"
      "network-setup.service"
      "resolvconf.service"
      "lastlog2-import.service"
      "systemd-logind.service"
      # More units from the research agent's minimal-NixOS surveys:
      # we never wait on the network, never process binfmt rules,
      # and don't need audit event routing.
      "systemd-networkd-wait-online.service"
      "systemd-binfmt.service"
      "audit.service"
    ];

    programs.bcc.enable = true;

    boot.kernelModules = lib.optionals (cfg.useBlockDev && cfg.dmDelayMs > 0) [ "dm-delay" ];
    boot.supportedFilesystems =
      lib.optionals (cfg.useBlockDev && cfg.fsModule != null) [ cfg.fsModule ];

    # The default initrd pulls in ~30 modules for common real-world
    # hardware (AHCI, SATA variants, NVMe controllers, USB stacks,
    # HID vendor drivers, i8042, pcips2, dm_mod). A QEMU guest only
    # ever sees virtio devices (loaded by `profiles/qemu-guest.nix`),
    # so dropping the default set means udev doesn't modprobe and
    # cold-plug-probe each one.
    boot.initrd.includeDefaultModules = false;

    # Not disabling `services.lvm.enable`: we use `dmsetup` (from
    # `lvm2.bin`, which services.lvm pulls in) for the dm-delay
    # wrapper around the bench's block device. Turning lvm off
    # removes dmsetup from the system PATH even though the udev
    # rules would be the actual savings.

    # qemu-guest-agent serves host→guest commands via a virtio-serial
    # port; the test-driver talks to us over the hvc0 backdoor, not
    # qga. Skip the agent + the udev rule that triggers its socket.
    # `qemu-vm.nix` enables it by default for VM tests, so mkForce.
    services.qemuGuest.enable = lib.mkForce false;

    # Intentionally *not* setting `boot.initrd.systemd.enable = true`.
    # Measured ~+1-2s on ext4/xfs/btrfs ram-backed cells: the
    # systemd-in-initrd image is larger and its unit-graph
    # construction dwarfs the parallelism win on a VM that reaches
    # multi-user in <10s already. The bash-based stage 1 is
    # strictly faster here.

    # ZFS pool create needs an 8-hex hostId.
    networking.hostId = lib.mkIf (cfg.fs == "zfs") "abcd1234";

    environment.systemPackages = with pkgs; [
      bpftrace
      perf
      jq
      benchPkg
    ]
    ++ lib.optionals cfg.multiProcess [ nixComponents.nix-cli ]
    ++ lib.optionals cfg.throttleGate [ throttleDaemon ];
  };

  testScript = _: testScriptPrelude + builtins.readFile ./test_script.py;
}
