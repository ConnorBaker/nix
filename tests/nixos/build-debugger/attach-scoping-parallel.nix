# Scoping invariant under parallelism: `--build-debugger <installable>`
# with `--max-jobs >= 2` instruments only the target drv even when
# multiple builds are running concurrently. Non-targeted drvs in the
# closure must not pause and must not publish attach-info, regardless
# of scheduling order.

{ lib, ... }:

{
  imports = [ ./common.nix ];

  name = "build-debugger-attach-scoping-parallel";
  enableOCR = false;

  nodes.machine =
    { pkgs, ... }:
    {
      # Target drv has two parallel siblings in its buildInputs. Each
      # sibling succeeds (exit 0) after a brief pause so they're
      # in-flight at the same time as the target. The target itself
      # fails, triggering its (and only its) attach-info publish.
      # Three independent top-level drvs so `nix build` can schedule
      # them concurrently under `--max-jobs >= 2`. The target FAILS
      # and pauses; the siblings SLEEP long enough to still be in-
      # flight when the test inspects the debugger directory. That
      # is the window during which any (incorrectly) instrumented
      # sibling WOULD have attach-info published — the whole
      # invariant we want to verify.
      #
      # `sib1` and `sib2` are not `buildInputs` of the target; that
      # would force them to finish before the target runs. They're
      # listed as separate installables on the `nix build` command
      # line below.
      environment.etc."test-drvs/parallel-target.nix".text = ''
        with import <nixpkgs> { };
        stdenv.mkDerivation {
          name = "bd-parallel-target";
          src = null;
          dontUnpack = true;
          buildPhase = "exit 3";
        }
      '';
      environment.etc."test-drvs/parallel-sib1.nix".text = ''
        with import <nixpkgs> { };
        stdenv.mkDerivation {
          name = "bd-parallel-sib1";
          src = null;
          dontUnpack = true;
          buildPhase = '''
            mkdir -p $out
            echo started > $out/stamp
            sleep 90
          ''';
          installPhase = "true";
        }
      '';
      environment.etc."test-drvs/parallel-sib2.nix".text = ''
        with import <nixpkgs> { };
        stdenv.mkDerivation {
          name = "bd-parallel-sib2";
          src = null;
          dontUnpack = true;
          buildPhase = '''
            mkdir -p $out
            echo started > $out/stamp
            sleep 90
          ''';
          installPhase = "true";
        }
      '';
    };

  testScript = ''
    start_all()
    machine.wait_for_unit("multi-user.target")

    NIX = "nix --extra-experimental-features 'nix-command build-debugger ca-derivations'"

    drv_target = machine.succeed(
        "nix-instantiate --extra-experimental-features "
        "'nix-command build-debugger ca-derivations' /etc/test-drvs/parallel-target.nix"
    ).strip().splitlines()[-1]
    drv_sib1 = machine.succeed(
        "nix-instantiate --extra-experimental-features "
        "'nix-command build-debugger ca-derivations' /etc/test-drvs/parallel-sib1.nix"
    ).strip().splitlines()[-1]
    drv_sib2 = machine.succeed(
        "nix-instantiate --extra-experimental-features "
        "'nix-command build-debugger ca-derivations' /etc/test-drvs/parallel-sib2.nix"
    ).strip().splitlines()[-1]

    target_hash = drv_target.split("/")[-1][:32]
    sib1_hash = drv_sib1.split("/")[-1][:32]
    sib2_hash = drv_sib2.split("/")[-1][:32]
    target_attach = f"/nix/var/nix/debugger/{target_hash}.attach"
    sib1_attach = f"/nix/var/nix/debugger/{sib1_hash}.attach"
    sib2_attach = f"/nix/var/nix/debugger/{sib2_hash}.attach"

    # Kick off the siblings first (no `--build-debugger`) so they're
    # running by the time we start the target with debugger. If we
    # scheduled all three with the flag in one `nix build`, the
    # `--build-debugger` CLI would reject multi-installable invocation.
    machine.succeed(
        "cat > /tmp/bd-sibs.sh <<'EOF'\n"
        "#!/bin/sh\n"
        "export PATH=/run/current-system/sw/bin:$PATH\n"
        + NIX + " build --max-jobs 4 "
        + drv_sib1 + "^out " + drv_sib2 + "^out "
        "> /tmp/sibs.log 2>&1\n"
        "echo $? > /tmp/sibs.exit\n"
        "EOF"
    )
    machine.succeed(
        "chmod +x /tmp/bd-sibs.sh && "
        "systemd-run --quiet --unit=bd-sibs --no-block "
        "--property=StandardInput=null /tmp/bd-sibs.sh"
    )

    # Wait for at least one sibling to have written its stamp,
    # meaning its builder is actively running `sleep 90`.
    machine.wait_until_succeeds(
        "nix-store -q --outputs " + drv_sib1 + " | head -1 | "
        "xargs -I{} test -f {}/stamp || "
        "nix-store -q --outputs " + drv_sib2 + " | head -1 | "
        "xargs -I{} test -f {}/stamp",
        timeout=300,
    )

    # Now kick off the target with --build-debugger. Siblings are
    # still mid-`sleep 90`; their builders are live.
    machine.succeed(
        "cat > /tmp/bd-run.sh <<'EOF'\n"
        "#!/bin/sh\n"
        "export PATH=/run/current-system/sw/bin:$PATH\n"
        + NIX + " build --max-jobs 4 --build-debugger "
        + drv_target + "^out > /tmp/build.log 2>&1\n"
        "echo $? > /tmp/build.exit\n"
        "EOF"
    )
    machine.succeed(
        "chmod +x /tmp/bd-run.sh && "
        "systemd-run --quiet --unit=bd-par --no-block "
        "--property=StandardInput=null /tmp/bd-run.sh"
    )

    machine.wait_until_succeeds(
        f"test -f {target_attach} && "
        "tmp=$(sed -n 's/.*\"hostTmpdir\":\"\\([^\"]*\\)\".*/\\1/p' "
        + target_attach + ") && "
        "test -n \"$tmp\" && test -f \"$tmp/env-vars\"",
        timeout=300,
    )

    # Siblings are STILL running at this point. If any of them
    # (incorrectly) got instrumented, its attach-info would be
    # published right now. Verify it isn't.
    machine.fail(f"test -f {sib1_attach}")
    machine.fail(f"test -f {sib2_attach}")

    # Kill the lingering sleepy siblings first so the target build
    # can't get stuck waiting on daemon locks they hold. Match by
    # process name `sleep` (not `-f 'sleep 90'`, which would also
    # match pkill's own argv and suicide it).
    machine.succeed("pkill -TERM -x sleep || true")
    # Wake the target's wrapper and let all builds finish.
    machine.succeed(
        "pid=$(sed -n 's/.*\"pid\":\\([0-9]*\\).*/\\1/p' "
        + target_attach + ") && kill -TERM $pid"
    )
    machine.wait_until_succeeds("test -f /tmp/build.exit", timeout=120)
    machine.wait_until_succeeds("test -f /tmp/sibs.exit", timeout=120)
  '';
}
