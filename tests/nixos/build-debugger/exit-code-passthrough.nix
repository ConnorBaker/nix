# Explicit `exit N` in the buildPhase must propagate to `nix build`'s
# reported builder status — the wrapper's EXIT-preserving trap should
# not overwrite it.

{ lib, ... }:

{
  imports = [ ./common.nix ];

  name = "build-debugger-exit-code-passthrough";
  enableOCR = false;

  nodes.machine =
    { pkgs, ... }:
    {
      environment.etc."test-drvs/exit13.nix".text = ''
        with import <nixpkgs> { };
        stdenv.mkDerivation {
          name = "bd-exit13";
          src = null;
          dontUnpack = true;
          buildPhase = "exit 13";
        }
      '';
    };

  testScript = ''
    start_all()
    machine.wait_for_unit("multi-user.target")

    NIX = "nix --extra-experimental-features 'nix-command build-debugger ca-derivations'"

    drv = machine.succeed(
        "nix-instantiate --extra-experimental-features "
        "'nix-command build-debugger ca-derivations' /etc/test-drvs/exit13.nix"
    ).strip().splitlines()[-1]
    drv_hash = drv.split("/")[-1][:32]
    attach = f"/nix/var/nix/debugger/{drv_hash}.attach"

    machine.succeed("rm -f /tmp/build.log /tmp/build.exit")
    machine.succeed(
        "cat > /tmp/bd-run.sh <<'EOF'\n"
        + "#!/bin/sh\n"
        + "export PATH=/run/current-system/sw/bin:$PATH\n"
        + NIX + " build --build-debugger " + drv + "^out"
        + " > /tmp/build.log 2>&1\n"
        + "echo $? > /tmp/build.exit\n"
        + "EOF"
    )
    machine.succeed(
        "chmod +x /tmp/bd-run.sh && "
        "systemd-run --quiet --unit=bd-exit13 --no-block "
        "--property=StandardInput=null /tmp/bd-run.sh"
    )
    # Wait until the wrapper's EXIT trap has fired (which writes
    # `env-vars`) before sending SIGTERM. Attach-info is published at
    # startBuild — long before the build actually fails — so gating on
    # its existence alone would race with the running buildPhase: a
    # SIGTERM delivered before `_nix_debug_attach` installs its
    # TERM trap kills bash with 143 and masks the real exit 13.
    machine.wait_until_succeeds(
        f"test -f {attach} && "
        "tmp=$(sed -n 's/.*\"hostTmpdir\":\"\\([^\"]*\\)\".*/\\1/p' "
        + attach + ") && "
        "test -n \"$tmp\" && test -f \"$tmp/env-vars\"",
        timeout=300,
    )
    machine.succeed(
        "pid=$(sed -n 's/.*\"pid\":\\([0-9]*\\).*/\\1/p' "
        + attach + ") && kill -TERM $pid"
    )
    machine.wait_until_succeeds("test -f /tmp/build.exit", timeout=60)
    build_log = machine.succeed("cat /tmp/build.log")
    assert "exit code 13" in build_log or "status 13" in build_log, (
        f"expected `exit code 13` in build log, got:\n{build_log}"
    )
  '';
}
