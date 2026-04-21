# Core happy-path: a failing stdenv build pauses; `nix debug-attach`
# enters the sandbox; the attached shell sees the failing phase's
# environment and partial artefacts; on exit the original `nix build`
# reports a non-zero status.

{ lib, ... }:

{
  imports = [ ./common.nix ];

  name = "build-debugger-attach-happy";
  enableOCR = false;

  nodes.machine =
    { pkgs, ... }:
    {
      environment.etc."test-drvs/stdenv-fail.nix".text = ''
        with import <nixpkgs> { };
        stdenv.mkDerivation {
          name = "bd-stdenv-fail";
          src = null;
          dontUnpack = true;
          buildPhase = '''
            export MY_PHASE_SENTINEL="visible-from-buildPhase"
            echo "phase starting; out=$out; top=$NIX_BUILD_TOP"
            mkdir -p "$NIX_BUILD_TOP/scratch"
            echo "partial-data" > "$NIX_BUILD_TOP/scratch/note.txt"
            ls /nonexistent/path/that/does/not/exist
          ''';
        }
      '';
    };

  testScript = ''
    start_all()
    machine.wait_for_unit("multi-user.target")

    NIX = "nix --extra-experimental-features 'nix-command build-debugger ca-derivations'"

    drv = machine.succeed(
        "nix-instantiate --extra-experimental-features "
        "'nix-command build-debugger ca-derivations' /etc/test-drvs/stdenv-fail.nix"
    ).strip().splitlines()[-1]

    machine.succeed(
        "rm -f /tmp/build.log /tmp/build.exit /tmp/attach.log"
    )
    machine.succeed(
        "cat > /tmp/bd-run.sh <<'EOF'\n"
        + "#!/bin/sh\n"
        + "export PATH=/run/current-system/sw/bin:$PATH\n"
        + NIX + " build --print-out-paths --build-debugger "
        + drv + "^out > /tmp/build.log 2>&1\n"
        + "echo $? > /tmp/build.exit\n"
        + "EOF"
    )
    machine.succeed(
        "chmod +x /tmp/bd-run.sh && "
        "systemd-run --quiet --unit=bd-build --no-block "
        "--property=StandardInput=null "
        "/tmp/bd-run.sh"
    )

    drv_hash = drv.split("/")[-1][:32]
    attach_file = f"/nix/var/nix/debugger/{drv_hash}.attach"

    try:
        machine.wait_until_succeeds(
            f"test -f {attach_file} && "
            "tmp=$(sed -n 's/.*\"hostTmpdir\":\"\\([^\"]*\\)\".*/\\1/p' "
            f"{attach_file}) && "
            "test -n \"$tmp\" && test -f \"$tmp/env-vars\"",
            timeout=300,
        )
    except Exception:
        print("=== build.log ===")
        print(machine.execute("cat /tmp/build.log 2>&1")[1])
        print("=== bd-build status ===")
        print(machine.execute("systemctl status bd-build.service --no-pager -l 2>&1")[1])
        raise

    machine.succeed(
        "cat > /tmp/attach-commands.sh <<'CMDS'\n"
        "printf 'SENT=%s\\n' \"$MY_PHASE_SENTINEL\"\n"
        "printf 'TOP=%s\\n' \"$NIX_BUILD_TOP\"\n"
        "printf 'NOTE=%s\\n' \"$(cat \"$NIX_BUILD_TOP/scratch/note.txt\")\"\n"
        "exit\n"
        "CMDS"
    )
    machine.succeed(
        "cat > /tmp/bd-attach.sh <<'EOF'\n"
        + "#!/bin/sh\n"
        + "export PATH=/run/current-system/sw/bin:$PATH\n"
        + "exec " + NIX + " debug-attach " + drv + "\n"
        + "EOF"
    )
    machine.succeed(
        "chmod +x /tmp/bd-attach.sh && "
        "script -qc /tmp/bd-attach.sh /tmp/attach.log < /tmp/attach-commands.sh"
    )

    attach_log = machine.succeed("cat /tmp/attach.log")
    for expected in ["SENT=visible-from-buildPhase", "NOTE=partial-data"]:
        assert expected in attach_log, (
            f"expected `{expected}` in attach shell output, got:\n{attach_log}"
        )

    machine.wait_until_succeeds("test -f /tmp/build.exit", timeout=120)
    exit_code = machine.succeed("cat /tmp/build.exit").strip()
    assert exit_code != "0", (
        f"expected nonzero exit from paused build, got {exit_code}"
    )
  '';
}
