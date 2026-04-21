# On a successful build, `--build-debugger` must be a no-op: no
# attach-info is left behind once `nix build` returns. Also verifies
# the `0700` permission the daemon enforces on the debugger directory
# is applied on first publish.

{ lib, ... }:

{
  imports = [ ./common.nix ];

  name = "build-debugger-attach-success-noop";
  enableOCR = false;

  nodes.machine =
    { pkgs, ... }:
    {
      environment.etc."test-drvs/success.nix".text = ''
        with import <nixpkgs> { };
        stdenv.mkDerivation {
          name = "bd-success";
          src = null;
          dontUnpack = true;
          buildPhase = "mkdir -p $out; echo built > $out/stamp";
          installPhase = "true";
        }
      '';
    };

  testScript = ''
    start_all()
    machine.wait_for_unit("multi-user.target")

    NIX = "nix --extra-experimental-features 'nix-command build-debugger ca-derivations'"

    drv = machine.succeed(
        "nix-instantiate --extra-experimental-features "
        "'nix-command build-debugger ca-derivations' /etc/test-drvs/success.nix"
    ).strip().splitlines()[-1]
    drv_hash = drv.split("/")[-1][:32]

    machine.succeed(
        f"{NIX} build --build-debugger {drv}^out --no-link"
    )
    # Attach-info was published at startBuild and unpublished on
    # cleanupBuild — by the time nix build returns it must be gone.
    machine.fail(
        f"test -f /nix/var/nix/debugger/{drv_hash}.attach"
    )

    # The directory the daemon created on first publish must be 0700.
    perms = machine.succeed(
        "stat -c '%a' /nix/var/nix/debugger"
    ).strip()
    assert perms == "700", f"expected 0700 on debugger dir, got {perms}"
  '';
}
