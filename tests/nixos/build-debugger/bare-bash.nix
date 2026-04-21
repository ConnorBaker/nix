# Non-stdenv bare-bash build: verifies the wrapper's EXIT-trap path
# captures env via `declare -p` even when the target isn't a stdenv
# derivation (where `failureHook` is the preferred path).
#
# Subtest (inherited from the original file): 11.

{ lib, ... }:

{
  imports = [ ./common.nix ];

  name = "build-debugger-bare-bash";
  enableOCR = false;

  nodes.machine =
    { pkgs, ... }:
    {
      # G. Bare-bash (non-stdenv) failure — exercises the EXIT-trap
      #    path. `builtins.storePath` wraps the bash path so Nix tracks
      #    its closure as a runtime dependency (it needs its own
      #    coreutils etc. to run inside the sandbox).
      environment.etc."test-drvs/bare-bash.nix".text = ''
        let
          system = builtins.currentSystem;
          bash = "''${builtins.storePath "${pkgs.bash}"}/bin/bash";
          coreutils = builtins.storePath "${pkgs.coreutils}";
          script = builtins.toFile "builder.sh" '''
            export PATH="''${coreutils}/bin:$PATH"
            export BARE_SENTINEL="visible-from-bare-bash"
            echo "bare-bash pre-failure"
            exit 7
          ''';
        in derivation {
          name = "bd-bare-bash";
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


    def instantiate(path):
        return machine.succeed(
            "nix-instantiate --extra-experimental-features "
            f"'nix-command build-debugger ca-derivations' --impure /etc/test-drvs/{path}"
        ).strip().splitlines()[-1]


    drv_bare = instantiate("bare-bash.nix")
    bare_hash = drv_bare.split("/")[-1][:32]
    bare_attach = f"/nix/var/nix/debugger/{bare_hash}.attach"

    machine.succeed("rm -f /tmp/build.log /tmp/build.exit")
    machine.succeed(
        "cat > /tmp/bd-run.sh <<'EOF'\n"
        + "#!/bin/sh\n"
        + "export PATH=/run/current-system/sw/bin:$PATH\n"
        + NIX + " build --build-debugger " + drv_bare + "^out"
        + " > /tmp/build.log 2>&1\n"
        + "echo $? > /tmp/build.exit\n"
        + "EOF"
    )
    machine.succeed(
        "chmod +x /tmp/bd-run.sh && "
        "systemd-run --quiet --unit=bd-bare --no-block "
        "--property=StandardInput=null /tmp/bd-run.sh"
    )

    try:
        machine.wait_until_succeeds(
            f"test -f {bare_attach} && "
            "tmp=$(sed -n 's/.*\"hostTmpdir\":\"\\([^\"]*\\)\".*/\\1/p' "
            + bare_attach + ") && "
            "test -n \"$tmp\" && test -f \"$tmp/env-vars\" && "
            "grep -q BARE_SENTINEL \"$tmp/env-vars\"",
            timeout=120,
        )
    except Exception:
        print("=== bare build.log ===")
        print(machine.execute("cat /tmp/build.log 2>&1")[1])
        print("=== bare attach file ===")
        print(machine.execute(f"cat {bare_attach} 2>&1")[1])
        raise

    machine.succeed(
        "pid=$(sed -n 's/.*\"pid\":\\([0-9]*\\).*/\\1/p' "
        + bare_attach + ") && kill -TERM $pid"
    )
    machine.wait_until_succeeds("test -f /tmp/build.exit", timeout=60)
  '';
}
