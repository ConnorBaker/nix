# Scoping invariant: `--build-debugger <installable>` instruments only
# the drv the user named. If a dependency in its closure fails first,
# the whole closure fails without any attach-info being published for
# the (uninstrumented) parent.

{ lib, ... }:

{
  imports = [ ./common.nix ];

  name = "build-debugger-attach-scoping";
  enableOCR = false;

  nodes.machine =
    { pkgs, ... }:
    {
      environment.etc."test-drvs/scoping.nix".text = ''
        with import <nixpkgs> { };
        let
          dep = stdenv.mkDerivation {
            name = "bd-dep";
            src = null;
            dontUnpack = true;
            buildPhase = "exit 5";
          };
        in stdenv.mkDerivation {
          name = "bd-parent";
          src = null;
          dontUnpack = true;
          buildInputs = [ dep ];
          buildPhase = "exit 2";
        }
      '';
    };

  testScript = ''
    start_all()
    machine.wait_for_unit("multi-user.target")

    NIX = "nix --extra-experimental-features 'nix-command build-debugger ca-derivations'"

    drv_parent = machine.succeed(
        "nix-instantiate --extra-experimental-features "
        "'nix-command build-debugger ca-derivations' /etc/test-drvs/scoping.nix"
    ).strip().splitlines()[-1]
    parent_hash = drv_parent.split("/")[-1][:32]

    machine.succeed(
        "cat > /tmp/bd-run.sh <<'EOF'\n"
        + "#!/bin/sh\n"
        + NIX + " build --build-debugger "
        + drv_parent + "^out > /tmp/build.log 2>&1\n"
        + "echo $? > /tmp/build.exit\n"
        + "EOF"
    )
    machine.succeed(
        "chmod +x /tmp/bd-run.sh && "
        "systemd-run --quiet --unit=bd-scope --no-block "
        "--property=StandardInput=null /tmp/bd-run.sh"
    )
    machine.wait_until_succeeds("test -f /tmp/build.exit", timeout=300)

    # The parent drv was instrumented but never reached (dep failed
    # first); no attach-info for the parent should persist.
    machine.fail(
        f"test -f /nix/var/nix/debugger/{parent_hash}.attach"
    )
  '';
}
