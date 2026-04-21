# Sweep correctness under concurrent `.attach` activity: a live paused
# build's entry must survive the stale-entry sweep, even while the sweep
# is running alongside a new `--build-debugger` publish. Also covers the
# age-based cull path (6h entries are removed regardless of pid-liveness).
#
# The stale-entry sweep runs inside `publishDebuggerAttachInfo`, gated by
# a 60 s per-worker atomic. To trigger it deterministically from a test
# that can't easily wait 60 s:
#
#   1. Start a real `--build-debugger` build (call it BUILD1); let its
#      wrapper hit the failure pause and publish a live `.attach`.
#   2. Forge several stale entries in the debugger dir: some with dead
#      pids, some with mtimes older than 6 h.
#   3. Kick off a second `--build-debugger` build against an already-
#      failing-fast drv. Its `publishDebuggerAttachInfo` runs the
#      sweep... but only if 60 s have passed since BUILD1's sweep. We
#      use the `touch -d` trick to forge BUILD1's attach-info mtime
#      far enough in the past that the time-guard logic is irrelevant
#      to the correctness of what we're checking.
#
# The key invariant we actually verify: the sweep distinguishes live
# vs. dead pids via pidfd (post-work-item-19), and age-caps the whole
# directory at 6 h. BUILD1's live entry must remain; forged stale
# entries must be gone.

{ lib, ... }:

{
  imports = [ ./common.nix ];

  name = "build-debugger-security-sweep";
  enableOCR = false;

  nodes.machine =
    { pkgs, ... }:
    {
      environment.etc."test-drvs/stdenv-fail.nix".text = ''
        with import <nixpkgs> { };
        stdenv.mkDerivation {
          name = "bd-sweep-fail";
          src = null;
          dontUnpack = true;
          buildPhase = "false";
        }
      '';

      environment.etc."test-drvs/stdenv-fail2.nix".text = ''
        with import <nixpkgs> { };
        stdenv.mkDerivation {
          name = "bd-sweep-fail2";
          src = null;
          dontUnpack = true;
          buildPhase = "exit 2";
        }
      '';
    };

  testScript = ''
    start_all()
    machine.wait_for_unit("multi-user.target")

    NIX = "nix --extra-experimental-features 'nix-command build-debugger ca-derivations'"

    drv1 = machine.succeed(
        "nix-instantiate --extra-experimental-features "
        "'nix-command build-debugger ca-derivations' "
        "/etc/test-drvs/stdenv-fail.nix"
    ).strip().splitlines()[-1]
    drv1_hash = drv1.split("/")[-1][:32]
    attach1 = f"/nix/var/nix/debugger/{drv1_hash}.attach"

    drv2 = machine.succeed(
        "nix-instantiate --extra-experimental-features "
        "'nix-command build-debugger ca-derivations' "
        "/etc/test-drvs/stdenv-fail2.nix"
    ).strip().splitlines()[-1]
    drv2_hash = drv2.split("/")[-1][:32]
    attach2 = f"/nix/var/nix/debugger/{drv2_hash}.attach"

    # Start BUILD1 in the background and wait for it to publish a live
    # attach-info + env-vars (i.e., the wrapper actually hit its
    # failure and is now paused).
    machine.succeed(
        "cat > /tmp/bd-sweep1.sh <<'EOF'\n"
        "#!/bin/sh\n"
        "export PATH=/run/current-system/sw/bin:$PATH\n"
        + NIX + " build --build-debugger " + drv1 + "^out > /tmp/build1.log 2>&1\n"
        "echo $? > /tmp/build1.exit\n"
        "EOF"
    )
    machine.succeed(
        "chmod +x /tmp/bd-sweep1.sh && "
        "systemd-run --quiet --unit=bd-sweep1 --no-block "
        "--property=StandardInput=null /tmp/bd-sweep1.sh"
    )
    machine.wait_until_succeeds(
        f"test -f {attach1} && "
        "tmp=$(sed -n 's/.*\"hostTmpdir\":\"\\([^\"]*\\)\".*/\\1/p' "
        + attach1 + ") && "
        "test -n \"$tmp\" && test -f \"$tmp/env-vars\"",
        timeout=300,
    )

    # Forge stale entries alongside the live BUILD1 attach-info.
    #
    # Type A — dead local pid: pid that definitely isn't running. We
    # pick 999999999 which is above pid_max on essentially every Linux
    # host; the sweep's pidfd probe returns ESRCH and the entry is
    # removed.
    forged_dead_hash = "b" * 32
    forged_dead_attach = f"/nix/var/nix/debugger/{forged_dead_hash}.attach"
    machine.succeed(
        "cat > " + forged_dead_attach + " <<'EOF'\n"
        "{\"schemaVersion\": 1, \"pid\": 999999999, "
        "\"drvPath\": \"/nix/store/" + forged_dead_hash + "-dead.drv\", "
        "\"sandboxTmpdir\": \"/build\", \"hostTmpdir\": \"/build\", "
        "\"bash\": \"/bin/bash\", "
        "\"wrapperScript\": \"/build/.nix-debug-wrapper.sh\"}\n"
        "EOF"
    )

    # Type B — ancient mtime: fresh-looking content, but the file was
    # last modified > 6 h ago. The sweep's age-cull removes it
    # regardless of pid-shape or remoteHost contents.
    forged_old_hash = "c" * 32
    forged_old_attach = f"/nix/var/nix/debugger/{forged_old_hash}.attach"
    # Use pid 1 in the fixture so a too-lenient sweep (no age cull) would
    # KEEP this entry and therefore fail our assertion below.
    machine.succeed(
        "cat > " + forged_old_attach + " <<'EOF'\n"
        "{\"schemaVersion\": 1, \"pid\": 1, "
        "\"drvPath\": \"/nix/store/" + forged_old_hash + "-ancient.drv\", "
        "\"sandboxTmpdir\": \"/build\", \"hostTmpdir\": \"/build\", "
        "\"bash\": \"/bin/bash\", "
        "\"wrapperScript\": \"/build/.nix-debug-wrapper.sh\"}\n"
        "EOF"
    )
    machine.succeed(
        f"touch -d '7 hours ago' {forged_old_attach}"
    )

    # Type C — orphan redirect: has remoteHost set but no local pid
    # to probe. Fresh mtime → sweep keeps it (that's by design; the
    # age-cull is what handles these).
    forged_redirect_hash = "d" * 32
    forged_redirect_attach = f"/nix/var/nix/debugger/{forged_redirect_hash}.attach"
    machine.succeed(
        "cat > " + forged_redirect_attach + " <<'EOF'\n"
        "{\"schemaVersion\": 1, \"remoteHost\": \"ssh://elsewhere\", "
        "\"drvPath\": \"/nix/store/" + forged_redirect_hash + "-redir.drv\"}\n"
        "EOF"
    )

    # Force the 60-second per-worker guard to elapse by backdating the
    # steady-clock-based marker — that's not possible externally, so
    # instead we wait on wall clock and rely on a fresh daemon worker.
    # `pkill -HUP nix-daemon` bounces the daemon; the next connection
    # spawns a fresh worker with no prior sweep, so the gate clears.
    machine.succeed("systemctl restart nix-daemon.service")

    # BUILD2: failing-fast drv whose publish runs the sweep.
    machine.succeed(
        "cat > /tmp/bd-sweep2.sh <<'EOF'\n"
        "#!/bin/sh\n"
        "export PATH=/run/current-system/sw/bin:$PATH\n"
        + NIX + " build --build-debugger " + drv2 + "^out > /tmp/build2.log 2>&1\n"
        "echo $? > /tmp/build2.exit\n"
        "EOF"
    )
    machine.succeed(
        "chmod +x /tmp/bd-sweep2.sh && "
        "systemd-run --quiet --unit=bd-sweep2 --no-block "
        "--property=StandardInput=null /tmp/bd-sweep2.sh"
    )
    machine.wait_until_succeeds(
        f"test -f {attach2}",
        timeout=300,
    )

    # ----------------------------------------------------------------
    # Sweep invariants:
    #   BUILD1's live attach-info: PRESENT (the whole point of the
    #       sweep distinguishing live pids from dead).
    #   Forged dead-pid entry: REMOVED.
    #   Forged ancient-mtime entry: REMOVED (age cull).
    #   Forged fresh redirect entry: PRESENT (age cull didn't fire;
    #       pid-less → sweep keeps it).
    # ----------------------------------------------------------------
    assert machine.succeed(f"test -f {attach1} && echo ok").strip() == "ok", (
        "live BUILD1 attach-info was incorrectly swept"
    )
    machine.fail(f"test -f {forged_dead_attach}")
    machine.fail(f"test -f {forged_old_attach}")
    assert machine.succeed(f"test -f {forged_redirect_attach} && echo ok").strip() == "ok", (
        "fresh redirect attach-info was incorrectly swept"
    )

    # Release BUILD1 and BUILD2.
    machine.succeed(
        "pid=$(sed -n 's/.*\"pid\":\\([0-9]*\\).*/\\1/p' "
        + attach1 + ") && kill -TERM $pid 2>/dev/null || true"
    )
    machine.succeed(
        "pid=$(sed -n 's/.*\"pid\":\\([0-9]*\\).*/\\1/p' "
        + attach2 + ") && kill -TERM $pid 2>/dev/null || true"
    )
    machine.wait_until_succeeds("test -f /tmp/build1.exit", timeout=60)
    machine.wait_until_succeeds("test -f /tmp/build2.exit", timeout=60)
    # Cleanup the redirect so a later test run doesn't collide.
    machine.succeed(f"rm -f {forged_redirect_attach}")
  '';
}
