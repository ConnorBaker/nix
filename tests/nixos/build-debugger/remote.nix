# Two-node end-to-end exercise of `--build-debugger` over a remote
# builder. The client has `nix.buildMachines` pointing at a builder; a
# failing build is dispatched via the build-hook mechanism, the
# builder publishes attach-info on its own node, and `nix debug-attach`
# on the client auto-SSHes to the builder to drive the interactive
# shell.
#
# Subtests:
#   1. Hook dispatches; local redirect is written on client.
#   2. `sudo nix debug-attach` on client auto-SSHes and the attach
#      shell sees the build-phase sentinel.
#   3. The background `nix build` on client reports the failure.
#   4. Cleanup: client's redirect + builder's attach-info removed.
#   5. Explicit `--on builder` bypasses redirect and uses the
#      configured SSH key/user (work-item 04).

test@{
  config,
  lib,
  hostPkgs,
  ...
}:

let
  pkgs = config.nodes.client.nixpkgs.pkgs;
in

{
  name = "build-debugger-remote";
  enableOCR = false;

  nodes = {
    builder =
      { config, pkgs, ... }:
      {
        services.openssh.enable = true;
        virtualisation.writableStore = true;
        virtualisation.additionalPaths = [
          pkgs.bash
          pkgs.coreutils
          pkgs.stdenv
          pkgs.python3
        ];
        nix.settings.sandbox = true;
        nix.settings.substituters = lib.mkForce [ ];
        nix.settings.experimental-features = [
          "nix-command"
          "build-debugger"
          "ca-derivations"
        ];
        nix.settings.trusted-users = [ "root" ];
        environment.systemPackages = [
          pkgs.util-linux
          pkgs.python3
        ];
        nix.nixPath = [ "nixpkgs=${pkgs.path}" ];
      };

    client =
      { config, lib, pkgs, ... }:
      {
        nix.settings.max-jobs = 0; # force remote dispatch
        nix.distributedBuilds = true;
        nix.buildMachines = [
          {
            hostName = "builder";
            sshUser = "root";
            sshKey = "/root/.ssh/id_ed25519";
            systems = [ pkgs.stdenv.hostPlatform.system ];
            maxJobs = 1;
          }
        ];
        virtualisation.writableStore = true;
        virtualisation.additionalPaths = [
          pkgs.bash
          pkgs.python3
          pkgs.coreutils
          pkgs.stdenv
        ];
        nix.settings.substituters = lib.mkForce [ ];
        nix.settings.experimental-features = [
          "nix-command"
          "build-debugger"
          "ca-derivations"
        ];
        nix.settings.trusted-users = [ "root" ];
        programs.ssh.extraConfig = "ConnectTimeout 30";
        environment.systemPackages = [
          pkgs.util-linux
          pkgs.python3
        ];
        nix.nixPath = [ "nixpkgs=${pkgs.path}" ];

        environment.etc."test-drvs/remote-fail.nix".text = ''
          with import <nixpkgs> { };
          stdenv.mkDerivation {
            name = "bd-remote-fail";
            src = null;
            dontUnpack = true;
            buildPhase = '''
              export MY_PHASE_SENTINEL="remote-build-phase"
              echo "on remote: $(hostname); out=$out"
              exit 4
            ''';
          }
        '';
      };
  };

  testScript =
    { nodes }:
    ''
      # fmt: off
      import subprocess

      start_all()

      # Create an SSH key on the client.
      subprocess.run([
        "${hostPkgs.openssh}/bin/ssh-keygen", "-t", "ed25519", "-f", "key", "-N", ""
      ], capture_output=True, check=True)
      client.succeed("mkdir -p -m 700 /root/.ssh")
      client.copy_from_host("key", "/root/.ssh/id_ed25519")
      client.succeed("chmod 600 /root/.ssh/id_ed25519")

      # Install the SSH key on the builder.
      client.wait_for_unit("network-addresses-eth1.service")
      builder.succeed("mkdir -p -m 700 /root/.ssh")
      builder.copy_from_host("key.pub", "/root/.ssh/authorized_keys")
      builder.wait_for_unit("sshd")
      builder.wait_for_unit("network-addresses-eth1.service")
      builder.wait_for_unit("multi-user.target")
      client.wait_for_unit("multi-user.target")
      client.succeed(
          "ssh -o StrictHostKeyChecking=no builder 'echo hello >&2'"
      )

      NIX = "nix --extra-experimental-features 'nix-command build-debugger ca-derivations'"
      drv = client.succeed(
          "nix-instantiate --extra-experimental-features "
          "'nix-command build-debugger ca-derivations' "
          "/etc/test-drvs/remote-fail.nix"
      ).strip().splitlines()[-1]
      drv_hash = drv.split("/")[-1][:32]

      # ----------------------------------------------------------------
      # 1. Hook dispatches; local redirect is written on client.
      # ----------------------------------------------------------------
      client.succeed(
          "cat > /tmp/bd-run.sh <<'EOF'\n"
          + "#!/bin/sh\n"
          + "export PATH=/run/current-system/sw/bin:$PATH\n"
          + NIX + " build --build-debugger " + drv + "^out > /tmp/build.log 2>&1\n"
          + "echo $? > /tmp/build.exit\n"
          + "EOF"
      )
      client.succeed(
          "chmod +x /tmp/bd-run.sh && "
          "systemd-run --quiet --unit=bd-remote --no-block "
          "--property=StandardInput=null /tmp/bd-run.sh"
      )
      redirect_attach = f"/nix/var/nix/debugger/{drv_hash}.attach"
      client.wait_until_succeeds(
          f"test -f {redirect_attach} && "
          f"grep -q remoteHost {redirect_attach}",
          timeout=600,
      )

      # ----------------------------------------------------------------
      # 2. `sudo nix debug-attach` on client follows the redirect,
      #    SSHes to builder, and the attach shell sees the build-phase
      #    sentinel.
      # ----------------------------------------------------------------
      # Wait for the builder's own attach-info to be published.
      builder_attach = f"/nix/var/nix/debugger/{drv_hash}.attach"
      builder.wait_until_succeeds(
          f"test -f {builder_attach} && "
          "tmp=$(sed -n 's/.*\"hostTmpdir\":\"\\([^\"]*\\)\".*/\\1/p' "
          + builder_attach + ") && "
          "test -n \"$tmp\" && test -f \"$tmp/env-vars\"",
          timeout=600,
      )

      client.succeed(
          "cat > /tmp/attach-cmds.sh <<'CMDS'\n"
          "printf 'SENT=%s\\n' \"$MY_PHASE_SENTINEL\"\n"
          "exit\n"
          "CMDS"
      )
      client.succeed(
          "cat > /tmp/bd-attach.sh <<'EOF'\n"
          + "#!/bin/sh\n"
          + "export PATH=/run/current-system/sw/bin:$PATH\n"
          + "exec " + NIX + " debug-attach " + drv + "\n"
          + "EOF"
      )
      client.succeed(
          "chmod +x /tmp/bd-attach.sh && "
          "script -qc /tmp/bd-attach.sh /tmp/attach.log < /tmp/attach-cmds.sh"
      )
      attach_log = client.succeed("cat /tmp/attach.log")
      assert "SENT=remote-build-phase" in attach_log, (
          f"expected `SENT=remote-build-phase` in attach output, got:\n{attach_log}"
      )

      # ----------------------------------------------------------------
      # 3. The client's `nix build` reports failure.
      # ----------------------------------------------------------------
      client.wait_until_succeeds("test -f /tmp/build.exit", timeout=120)
      exit_code = client.succeed("cat /tmp/build.exit").strip()
      assert exit_code != "0", (
          f"expected nonzero exit from remote build, got {exit_code}"
      )

      # ----------------------------------------------------------------
      # 4. Cleanup: client's redirect is removed (via
      #    ~DerivationBuildingGoal); builder's real attach-info is
      #    removed (via unpublishDebuggerAttachInfo on cleanupBuild).
      # ----------------------------------------------------------------
      client.fail(f"test -f {redirect_attach}")
      # Builder may race a bit; give it a short grace period.
      builder.wait_until_fails(f"test -f {builder_attach}", timeout=30)

      # ----------------------------------------------------------------
      # 5. Explicit `--on builder`: bypass redirect, use the machine's
      #    SSH config (work-item 04).
      # ----------------------------------------------------------------
      # Kick off another remote build and wait for attach-info on the
      # builder (but delete the redirect on the client so we must use
      # `--on`).
      client.succeed("rm -f /tmp/build.log /tmp/build.exit /tmp/attach.log")
      client.succeed(
          "cat > /tmp/bd-run2.sh <<'EOF'\n"
          + "#!/bin/sh\n"
          + "export PATH=/run/current-system/sw/bin:$PATH\n"
          + NIX + " build --build-debugger " + drv + "^out > /tmp/build.log 2>&1\n"
          + "echo $? > /tmp/build.exit\n"
          + "EOF"
      )
      client.succeed(
          "chmod +x /tmp/bd-run2.sh && "
          "systemctl reset-failed bd-remote.service 2>/dev/null || true; "
          "systemd-run --quiet --unit=bd-remote2 --no-block "
          "--property=StandardInput=null /tmp/bd-run2.sh"
      )
      builder.wait_until_succeeds(
          f"test -f {builder_attach} && "
          "tmp=$(sed -n 's/.*\"hostTmpdir\":\"\\([^\"]*\\)\".*/\\1/p' "
          + builder_attach + ") && "
          "test -n \"$tmp\" && test -f \"$tmp/env-vars\"",
          timeout=600,
      )
      client.succeed(f"rm -f {redirect_attach}")

      client.succeed(
          "cat > /tmp/attach-cmds2.sh <<'CMDS'\n"
          "echo ON-BUILDER-VIA-EXPLICIT-ON\n"
          "exit\n"
          "CMDS"
      )
      client.succeed(
          "cat > /tmp/bd-attach2.sh <<'EOF'\n"
          + "#!/bin/sh\n"
          + "export PATH=/run/current-system/sw/bin:$PATH\n"
          + "exec " + NIX + " debug-attach --on builder " + drv + "\n"
          + "EOF"
      )
      client.succeed(
          "chmod +x /tmp/bd-attach2.sh && "
          "script -qc /tmp/bd-attach2.sh /tmp/attach2.log < /tmp/attach-cmds2.sh"
      )
      attach_log2 = client.succeed("cat /tmp/attach2.log")
      assert "ON-BUILDER-VIA-EXPLICIT-ON" in attach_log2, (
          f"expected explicit --on to drive attach, got:\n{attach_log2}"
      )

      client.wait_until_succeeds("test -f /tmp/build.exit", timeout=120)

      # ----------------------------------------------------------------
      # 6. `--on HOST` fallback: HOST is NOT in `nix.buildMachines`,
      #    so `findConfiguredMachine` returns nullopt and `dispatchRemote`
      #    falls through to a bare `ssh HOST -- sudo nix debug-attach`.
      #    We use a hostname alias (`builder-alt`) that isn't the
      #    configured `builder` so `findConfiguredMachine` declines.
      #    NixOS's `/etc/hosts` is read-only, so we wire the alias via
      #    `~/.ssh/config` and disable host-key checking there too —
      #    the fallback path intentionally doesn't force `BatchMode=yes`
      #    so the user gets a friendly prompt, but this test is
      #    non-interactive so we short-circuit via ssh config.
      # ----------------------------------------------------------------
      client.succeed("rm -f /tmp/build.log /tmp/build.exit /tmp/attach3.log")
      # ~/.ssh/config entry mapping `builder-alt` → builder's real
      # hostname, with strict-host-key-checking off so the bare-ssh
      # fallback (which doesn't set BatchMode) connects non-interactively.
      client.succeed(
          "cat >> /root/.ssh/config <<'EOF'\n"
          "Host builder-alt\n"
          "    HostName builder\n"
          "    User root\n"
          "    IdentityFile /root/.ssh/id_ed25519\n"
          "    StrictHostKeyChecking no\n"
          "    UserKnownHostsFile /dev/null\n"
          "    LogLevel ERROR\n"
          "EOF\n"
          "chmod 600 /root/.ssh/config"
      )

      # Kick off another remote build and wait for the builder's
      # attach-info.
      client.succeed(
          "cat > /tmp/bd-run3.sh <<'EOF'\n"
          + "#!/bin/sh\n"
          + "export PATH=/run/current-system/sw/bin:$PATH\n"
          + NIX + " build --build-debugger " + drv + "^out > /tmp/build.log 2>&1\n"
          + "echo $? > /tmp/build.exit\n"
          + "EOF"
      )
      client.succeed(
          "chmod +x /tmp/bd-run3.sh && "
          "systemctl reset-failed bd-remote2.service 2>/dev/null || true; "
          "systemd-run --quiet --unit=bd-remote3 --no-block "
          "--property=StandardInput=null /tmp/bd-run3.sh"
      )
      builder.wait_until_succeeds(
          f"test -f {builder_attach} && "
          "tmp=$(sed -n 's/.*\"hostTmpdir\":\"\\([^\"]*\\)\".*/\\1/p' "
          + builder_attach + ") && "
          "test -n \"$tmp\" && test -f \"$tmp/env-vars\"",
          timeout=600,
      )
      client.succeed(f"rm -f {redirect_attach}")

      # `--on builder-alt`: the alias isn't in `nix.buildMachines`
      # (which lists `builder`), so `findConfiguredMachine` returns
      # nullopt and we hit the bare-ssh fallback. The alias resolves
      # to the real builder via ~/.ssh/config.
      client.succeed(
          "cat > /tmp/attach-cmds3.sh <<'CMDS'\n"
          "echo FALLBACK-PATH-VIA-ALIAS\n"
          "exit\n"
          "CMDS"
      )
      client.succeed(
          "cat > /tmp/bd-attach3.sh <<'EOF'\n"
          + "#!/bin/sh\n"
          + "export PATH=/run/current-system/sw/bin:$PATH\n"
          + "exec " + NIX + " debug-attach --on builder-alt " + drv + "\n"
          + "EOF"
      )
      client.succeed(
          "chmod +x /tmp/bd-attach3.sh && "
          "script -qc /tmp/bd-attach3.sh /tmp/attach3.log < /tmp/attach-cmds3.sh"
      )
      attach_log3 = client.succeed("cat /tmp/attach3.log")
      assert "FALLBACK-PATH-VIA-ALIAS" in attach_log3, (
          f"expected bare-ssh fallback to drive attach via alias, "
          f"got:\n{attach_log3}"
      )

      client.wait_until_succeeds("test -f /tmp/build.exit", timeout=120)
    '';
}
