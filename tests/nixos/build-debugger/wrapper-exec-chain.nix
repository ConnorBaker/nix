# `exec` in the sourced builder replaces the wrapper's bash, losing the
# EXIT trap and `failureHook` function. The build fails, the wrapper
# pid dies without writing env-vars, and `nix debug-attach` must emit
# the distinct "exec-chain" diagnostic instead of the generic "wait
# for the failure" message.
#
# Reproduce the bug authentically: build a bare-bash drv whose builder
# script `exec`s a failing command. While that build is in flight,
# snapshot the attach-info file (the daemon removes it on cleanupBuild).
# Then, after the build has completed, run `nix debug-attach` against
# the drv — with the snapshotted attach-info restored — and verify the
# exec-chain diagnostic fires.

{ lib, ... }:

{
  imports = [ ./common.nix ];

  name = "build-debugger-wrapper-exec-chain";
  enableOCR = false;

  nodes.machine =
    { pkgs, ... }:
    {
      environment.etc."test-drvs/exec-chain.nix".text = ''
        let
          system = builtins.currentSystem;
          bash = "''${builtins.storePath "${pkgs.bash}"}/bin/bash";
          coreutils = builtins.storePath "${pkgs.coreutils}";
          script = builtins.toFile "builder.sh" '''
            export PATH="''${coreutils}/bin:$PATH"
            # `exec` replaces the wrapper bash → EXIT trap is lost.
            # `sleep 30 && false` gives the test a window to snapshot
            # the attach-info before the exec'd process exits non-zero.
            exec ''${coreutils}/bin/sleep 30
            false
          ''';
        in derivation {
          name = "bd-exec-chain";
          inherit system coreutils;
          builder = bash;
          args = [ "-e" script ];
        }
      '';
    };

  testScript = ''
    start_all()
    machine.wait_for_unit("multi-user.target")

    NIX = "nix --extra-experimental-features 'nix-command build-debugger ca-derivations'"

    drv = machine.succeed(
        "nix-instantiate --extra-experimental-features "
        "'nix-command build-debugger ca-derivations' --impure "
        "/etc/test-drvs/exec-chain.nix"
    ).strip().splitlines()[-1]
    drv_hash = drv.split("/")[-1][:32]
    attach = f"/nix/var/nix/debugger/{drv_hash}.attach"

    machine.succeed(
        "cat > /tmp/bd-exec.sh <<'EOF'\n"
        "#!/bin/sh\n"
        "export PATH=/run/current-system/sw/bin:$PATH\n"
        + NIX + " build --build-debugger " + drv + "^out > /tmp/build.log 2>&1\n"
        "echo $? > /tmp/build.exit\n"
        "EOF"
    )
    machine.succeed(
        "chmod +x /tmp/bd-exec.sh && "
        "systemd-run --quiet --unit=bd-exec --no-block "
        "--property=StandardInput=null /tmp/bd-exec.sh"
    )

    # Wait for attach-info to be published (at startBuild, before the
    # builder's `exec` replaces bash). Snapshot it immediately — the
    # daemon removes it in cleanupBuild once the exec'd `sleep 30`
    # exits and the overall build is reported as failed.
    machine.wait_until_succeeds(f"test -f {attach}", timeout=300)
    machine.succeed(f"cp {attach} /tmp/attach-snapshot.json")

    # Wait for the whole build to complete. At this point the wrapper
    # pid is gone (exec chained away; `sleep 30` finished; bash exited),
    # env-vars was never written, and the daemon has unpublished the
    # real attach-info.
    machine.wait_until_succeeds("test -f /tmp/build.exit", timeout=300)

    # Restore the snapshot so `debug-attach` has something to read.
    # The recorded pid in the snapshot is the dead wrapper's pid;
    # `pidfdAlive`/`kill` both report dead, env-vars is absent, and
    # the diagnostic should fire.
    machine.succeed(
        "mkdir -p /nix/var/nix/debugger && chmod 0700 /nix/var/nix/debugger && "
        f"cp /tmp/attach-snapshot.json {attach}"
    )
    out = machine.fail(f"{NIX} debug-attach {drv} 2>&1")
    assert "exec" in out.lower(), (
        f"expected exec-chain diagnostic, got:\n{out}"
    )
    # The error message names `exec` as the most-likely cause and
    # mentions SIGKILL/OOM as the alternative. Only the orthogonal
    # causes appear — the message no longer lists non-orthogonal
    # cases like 1-hour timeout.
    assert "SIGKILL" in out or "OOM" in out, (
        f"expected alternative-cause mention, got:\n{out}"
    )

    machine.succeed(f"rm -f {attach}")
  '';
}
