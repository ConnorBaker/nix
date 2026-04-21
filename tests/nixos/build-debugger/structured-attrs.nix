# `__structuredAttrs = true` derivation: verifies that env capture picks
# up shell-only (non-exported) variables via `declare -p` (the ones
# stdenv sources from `$NIX_ATTRS_SH_FILE`).
#
# Subtest (inherited from the original file): 12.

{ lib, ... }:

{
  imports = [ ./common.nix ];

  name = "build-debugger-structured-attrs";
  enableOCR = false;

  nodes.machine =
    { pkgs, ... }:
    {
      environment.etc."test-drvs/structured.nix".text = ''
        with import <nixpkgs> { };
        stdenv.mkDerivation {
          name = "bd-structured";
          __structuredAttrs = true;
          src = null;
          dontUnpack = true;
          mySentinel = "seen-via-structured-attrs";
          buildPhase = '''
            echo "mySentinel=$mySentinel"
            exit 9
          ''';
        }
      '';
    };

  testScript = ''
    start_all()
    machine.wait_for_unit("multi-user.target")

    NIX = "nix --extra-experimental-features 'nix-command build-debugger ca-derivations'"


    def instantiate(path):
        return machine.succeed(
            "nix-instantiate --extra-experimental-features "
            f"'nix-command build-debugger ca-derivations' /etc/test-drvs/{path}"
        ).strip().splitlines()[-1]


    drv_struct = instantiate("structured.nix")
    struct_hash = drv_struct.split("/")[-1][:32]
    struct_attach = f"/nix/var/nix/debugger/{struct_hash}.attach"

    machine.succeed("rm -f /tmp/build.log /tmp/build.exit")
    machine.succeed(
        "cat > /tmp/bd-run.sh <<'EOF'\n"
        + "#!/bin/sh\n"
        + "export PATH=/run/current-system/sw/bin:$PATH\n"
        + NIX + " build --build-debugger " + drv_struct + "^out"
        + " > /tmp/build.log 2>&1\n"
        + "echo $? > /tmp/build.exit\n"
        + "EOF"
    )
    machine.succeed(
        "chmod +x /tmp/bd-run.sh && "
        "systemd-run --quiet --unit=bd-struct --no-block "
        "--property=StandardInput=null /tmp/bd-run.sh"
    )

    machine.wait_until_succeeds(
        f"test -f {struct_attach} && "
        "tmp=$(sed -n 's/.*\"hostTmpdir\":\"\\([^\"]*\\)\".*/\\1/p' "
        + struct_attach + ") && "
        "test -n \"$tmp\" && "
        "grep -q 'mySentinel=.*seen-via-structured-attrs' \"$tmp/env-vars\"",
        timeout=300,
    )

    machine.succeed(
        "pid=$(sed -n 's/.*\"pid\":\\([0-9]*\\).*/\\1/p' "
        + struct_attach + ") && kill -TERM $pid"
    )
    machine.wait_until_succeeds("test -f /tmp/build.exit", timeout=60)
  '';
}
