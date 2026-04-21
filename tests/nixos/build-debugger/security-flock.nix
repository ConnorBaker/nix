# Two `nix debug-attach` sessions against the same paused build must
# serialise: the second must refuse (without --force) once the first
# holds the attach-lock.

{ lib, ... }:

{
  imports = [ ./common.nix ];

  name = "build-debugger-security-flock";
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
          buildPhase = "false";
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
    drv_hash = drv.split("/")[-1][:32]
    attach = f"/nix/var/nix/debugger/{drv_hash}.attach"
    lock = f"/nix/var/nix/debugger/{drv_hash}.lock"

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
        "systemd-run --quiet --unit=bd-flock --no-block "
        "--property=StandardInput=null /tmp/bd-run.sh"
    )
    machine.wait_until_succeeds(
        f"test -f {attach} && "
        "tmp=$(sed -n 's/.*\"hostTmpdir\":\"\\([^\"]*\\)\".*/\\1/p' "
        + attach + ") && "
        "test -n \"$tmp\" && test -f \"$tmp/env-vars\"",
        timeout=300,
    )

    # Hold the attach-lock from a detached helper. `machine.succeed`
    # waits for its command to exit, so `&` alone doesn't detach. Write
    # a small helper script, launch it with `setsid` + stdin/stdout
    # closed + `&`, and rely on it staying alive past the driver's
    # command reap.
    # Hold the attach-lock from a detached helper that records its PID
    # so we can target it precisely at cleanup time — `pkill -f 'flock'`
    # is a footgun because it can match the shell running the pkill
    # command itself via its argv in `/proc/<pid>/cmdline`.
    machine.succeed(
        "cat > /tmp/hold-lock.sh <<EOF\n"
        "#!/bin/sh\n"
        "echo \\$\\$ > /tmp/hold-lock.pid\n"
        f"exec flock -n {lock} -c 'sleep 60'\n"
        "EOF\n"
        "chmod +x /tmp/hold-lock.sh && "
        "setsid -f /tmp/hold-lock.sh >/dev/null 2>&1 < /dev/null"
    )
    # Poll until the lock is held (flock -n fails if we can't acquire
    # a second lock while the holder runs).
    machine.wait_until_fails(
        f"flock -n {lock} -c true",
        timeout=10,
    )
    out = machine.fail(
        f"{NIX} debug-attach {drv} 2>&1"
    )
    assert "another `nix debug-attach`" in out, (
        f"expected concurrent-attach refusal, got:\n{out}"
    )

    # `--force` must succeed against the held lock, log the
    # "not holding the lock" warning, and (since the attached shell
    # exits immediately) also drive the wrapper to resume. After
    # this step the build is effectively over — we don't need a
    # separate wake.
    machine.succeed(
        "cat > /tmp/force-cmds.sh <<'CMDS'\n"
        "exit\n"
        "CMDS"
    )
    machine.succeed(
        "cat > /tmp/bd-force.sh <<'EOF'\n"
        "#!/bin/sh\n"
        "export PATH=/run/current-system/sw/bin:$PATH\n"
        "exec " + NIX + " debug-attach --force " + drv + "\n"
        "EOF"
    )
    force_log = machine.succeed(
        "chmod +x /tmp/bd-force.sh && "
        "script -qc /tmp/bd-force.sh /tmp/force.log < /tmp/force-cmds.sh && "
        "cat /tmp/force.log"
    )
    assert "proceeding without holding the lock" in force_log, (
        f"expected --force to log the not-holding-the-lock warning, "
        f"got:\n{force_log}"
    )

    # Release the lingering lock-holder helper. The wrapper itself
    # was already woken by `--force`'s attach session exiting.
    machine.succeed(
        "test -f /tmp/hold-lock.pid && "
        "kill -TERM $(cat /tmp/hold-lock.pid) 2>/dev/null; true"
    )
    machine.wait_until_succeeds("test -f /tmp/build.exit", timeout=60)
  '';
}
