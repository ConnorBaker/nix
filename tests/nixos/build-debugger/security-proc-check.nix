# Defence-in-depth against pid reuse / stale attach-info: `nix
# debug-attach` checks `/proc/<pid>/cmdline` and refuses if the target
# isn't actually our wrapper script.

{ lib, ... }:

{
  imports = [ ./common.nix ];

  name = "build-debugger-security-proc-check";
  enableOCR = false;

  testScript = ''
    start_all()
    machine.wait_for_unit("multi-user.target")

    NIX = "nix --extra-experimental-features 'nix-command build-debugger ca-derivations'"

    machine.succeed(
        "mkdir -p /nix/var/nix/debugger && chmod 0700 /nix/var/nix/debugger"
    )

    # Forge an attach-info pointing at init (pid 1), which is a
    # persistently-alive process whose `/proc/<pid>/cmdline` clearly
    # doesn't contain `.nix-debug-wrapper.sh`. `debug-attach` must
    # refuse BEFORE entering any namespaces. Using init sidesteps the
    # fragility of "spawn a shell, use $$" — those shells get reaped
    # between `machine.succeed` invocations.
    forged_hash = "c" * 32
    forged_attach = f"/nix/var/nix/debugger/{forged_hash}.attach"
    forged_drv = f"/nix/store/{forged_hash}-forged.drv"
    machine.succeed(
        "mkdir -p /build && "
        "touch /build/env-vars && "
        "cat > " + forged_attach + " <<'EOF'\n"
        + "{\"schemaVersion\": 1, \"drvPath\": \"" + forged_drv + "\", "
        + "\"pid\": 1, \"bash\": \"/bin/bash\", "
        + "\"sandboxTmpdir\": \"/build\", \"hostTmpdir\": \"/build\", "
        + "\"wrapperScript\": \"/build/.nix-debug-wrapper.sh\"}\n"
        + "EOF"
    )
    out = machine.fail(
        f"{NIX} debug-attach {forged_drv} 2>&1"
    )
    assert "no longer runs the expected wrapper script" in out, (
        f"expected /proc verification to refuse attach, got:\n{out}"
    )
  '';
}
