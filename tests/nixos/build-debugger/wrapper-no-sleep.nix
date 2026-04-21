# The wrapper pauses by spawning `sleep 3600 &` and waiting on it.
# If the sandbox doesn't have `sleep` in PATH (bare-bash drv that
# forgot to wire coreutils), the wrapper emits a specific warning and
# returns without pausing — the build still fails, but no debug session
# is possible.
#
# This test exercises that branch by building a bare-bash drv whose
# PATH excludes coreutils. The expected failure mode: the wrapper's
# `[build-debugger] no \`sleep\` in PATH inside the sandbox; cannot
# pause` warning appears in the build log, and the build exits
# non-zero without publishing an env-vars file (because _nix_debug_attach
# returns before the pause path).

{ lib, ... }:

{
  imports = [ ./common.nix ];

  name = "build-debugger-wrapper-no-sleep";
  enableOCR = false;

  nodes.machine =
    { pkgs, ... }:
    {
      # Bare-bash drv with a script that deliberately doesn't put
      # coreutils on PATH. Just bash builtins + /bin paths. Bash's
      # `command -v sleep` inside the wrapper fails → warning fires.
      environment.etc."test-drvs/no-sleep.nix".text = ''
        let
          system = builtins.currentSystem;
          bash = "''${builtins.storePath "${pkgs.bash}"}/bin/bash";
          script = builtins.toFile "builder.sh" '''
            # Intentionally do not export any PATH entry that contains
            # sleep; bash builtins are all we have.
            export PATH=""
            exit 11
          ''';
        in derivation {
          name = "bd-no-sleep";
          inherit system;
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
        "/etc/test-drvs/no-sleep.nix"
    ).strip().splitlines()[-1]
    drv_hash = drv.split("/")[-1][:32]
    attach = f"/nix/var/nix/debugger/{drv_hash}.attach"

    # Run synchronously; no attach possible, so `nix build` exits on
    # its own once the wrapper returns ec 11.
    machine.fail(
        f"{NIX} build --build-debugger {drv}^out > /tmp/build.log 2>&1"
    )
    build_log = machine.succeed("cat /tmp/build.log")
    assert "no `sleep`" in build_log, (
        f"expected `no \\`sleep\\`` warning in build log, got:\n{build_log}"
    )

    # Attach-info was published at startBuild, but since the wrapper
    # returned without pausing, cleanupBuild has already removed it.
    machine.fail(f"test -f {attach}")
  '';
}
