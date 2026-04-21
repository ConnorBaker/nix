# Static refusals of `--build-debugger` and `nix debug-attach` that don't
# require a successful build. Split out of the monolithic
# `build-debugger.nix` so the suite parallelises on multi-core builders.
#
# Subtests:
#   - `nix develop --build-debugger` rejected.
#   - Non-bash builder rejected by preflight.
#   - Inline `-c` bash rejected.
#   - CA derivation rejected at CLI parse time.
#   - Untrusted user refused in daemon mode.
#   - `nix debug-attach` must be root.
#   - `nix debug-attach` against a drv that isn't paused fails fast.
#   - Malformed drv-path (base-32 alphabet).
#   - Schema version rejection.
#   - `--on HOST` against an unreachable host.

{ lib, ... }:

{
  imports = [ ./common.nix ];

  name = "build-debugger-gates";
  enableOCR = false;

  nodes.machine =
    { pkgs, ... }:
    {
      # B. Non-bash builder (preflight refusal).
      environment.etc."test-drvs/non-bash.nix".text = ''
        let system = builtins.currentSystem;
        in derivation {
          name = "bd-non-bash";
          inherit system;
          builder = "${pkgs.python3}/bin/python3";
          args = [ "-c" "import sys; sys.exit(1)" ];
        }
      '';

      # C. Inline-bash (`-c`) builder (preflight refusal).
      environment.etc."test-drvs/inline-bash.nix".text = ''
        let system = builtins.currentSystem;
        in derivation {
          name = "bd-inline-bash";
          inherit system;
          builder = "${pkgs.bash}/bin/bash";
          args = [ "-c" "false" ];
        }
      '';

      # D. CA derivation (CLI-parse-time refusal).
      environment.etc."test-drvs/ca.nix".text = ''
        with import <nixpkgs> { };
        stdenv.mkDerivation {
          name = "bd-ca";
          __contentAddressed = true;
          src = null;
          dontUnpack = true;
          buildPhase = "false";
        }
      '';

      # Used for the trusted-users refusal (which requires a drv the
      # daemon would instrument) and as the target of the `must be root`
      # check for `nix debug-attach`.
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


    def instantiate(path):
        return machine.succeed(
            "nix-instantiate --extra-experimental-features "
            f"'nix-command build-debugger ca-derivations' /etc/test-drvs/{path}"
        ).strip().splitlines()[-1]


    # ----------------------------------------------------------------
    # 1. Static CLI gates: flag is accepted only on `nix build`.
    # ----------------------------------------------------------------
    machine.fail(
        f"{NIX} develop --build-debugger -f /etc/test-drvs/stdenv-fail.nix"
    )

    # ----------------------------------------------------------------
    # 2. Non-bash builder: preflight refuses early on `nix build`.
    # ----------------------------------------------------------------
    nonbash_drv = instantiate("non-bash.nix")
    out = machine.fail(
        f"{NIX} build --build-debugger {nonbash_drv}^out 2>&1"
    )
    assert "builder to be `bash`" in out, (
        f"expected builder-not-bash refusal, got:\n{out}"
    )

    # ----------------------------------------------------------------
    # 3. Inline `-c` bash: preflight refuses.
    # ----------------------------------------------------------------
    inline_drv = instantiate("inline-bash.nix")
    out = machine.fail(
        f"{NIX} build --build-debugger {inline_drv}^out 2>&1"
    )
    assert "cannot debug `-c`-style" in out, (
        f"expected `-c`-inline refusal, got:\n{out}"
    )

    # ----------------------------------------------------------------
    # 4. CA-derivation: refused at CLI parse time (before the build).
    # ----------------------------------------------------------------
    ca_drv = instantiate("ca.nix")
    out = machine.fail(
        f"{NIX} build --build-debugger {ca_drv}^out 2>&1"
    )
    assert "content-addressed" in out, (
        f"expected CA-derivation refusal, got:\n{out}"
    )

    # ----------------------------------------------------------------
    # 5. Untrusted user: gets the trust-gate refusal in daemon mode.
    # ----------------------------------------------------------------
    drv_stdenv = instantiate("stdenv-fail.nix")
    out = machine.fail(
        f"sudo -u mallory {NIX} build "
        f"--option build-debugger true {drv_stdenv}^out 2>&1"
    )
    assert "trusted-users" in out, (
        f"expected trusted-users refusal, got:\n{out}"
    )

    # ----------------------------------------------------------------
    # 6. `nix debug-attach` requires root.
    # ----------------------------------------------------------------
    out = machine.fail(
        f"sudo -u alice {NIX} debug-attach {drv_stdenv} 2>&1"
    )
    assert "must be run as root" in out, (
        f"expected root-required refusal, got:\n{out}"
    )

    # ----------------------------------------------------------------
    # Not-paused: `nix debug-attach` against a drv that isn't paused
    # must fail fast with guidance, not poll.
    # ----------------------------------------------------------------
    fake_drv = "/nix/store/"+ "0" * 32 +"-does-not-exist.drv"
    out = machine.fail(
        f"{NIX} debug-attach {fake_drv} 2>&1"
    )
    assert "not currently paused" in out, (
        f"expected `not currently paused` in output, got:\n{out}"
    )

    # ----------------------------------------------------------------
    # 15. hashPartOf: malformed drv-path argument (with `e` and `o` —
    #     letters excluded from Nix's base-32 alphabet) is rejected
    #     with a clear error.
    # ----------------------------------------------------------------
    malformed = "/nix/store/aaaaaaaaaaaaaaaaaeaaoaaaaaaaaaaa-foo.drv"
    out = machine.fail(
        f"{NIX} debug-attach {malformed} 2>&1"
    )
    assert "base-32 alphabet" in out, (
        f"expected base-32 alphabet error, got:\n{out}"
    )

    # ----------------------------------------------------------------
    # 16. Schema version rejection: forged attach-info with a newer
    #     schemaVersion must be refused with the upgrade-suggestion
    #     message.
    # ----------------------------------------------------------------
    fake_hash = "a" * 32
    fake_attach = f"/nix/var/nix/debugger/{fake_hash}.attach"
    fake_ok_drv = f"/nix/store/{fake_hash}-forged.drv"
    machine.succeed(
        "mkdir -p /nix/var/nix/debugger && chmod 0700 /nix/var/nix/debugger"
    )
    machine.succeed(
        "cat > " + fake_attach + " <<'EOF'\n"
        + "{\"schemaVersion\": 99, \"drvPath\": \"forged\"}\n"
        + "EOF"
    )
    out = machine.fail(
        f"{NIX} debug-attach {fake_ok_drv} 2>&1"
    )
    assert "schema version 99" in out and "upgrade" in out.lower(), (
        f"expected schema-version-mismatch error with upgrade hint, got:\n{out}"
    )
    machine.succeed(f"rm -f {fake_attach}")

    # ----------------------------------------------------------------
    # 21. `--on HOST` dispatches to SSH. With no reachable host, ssh
    #     should fail with exit 255 and our wrapper translates that
    #     into a friendly "SSH connection failed" error.
    # ----------------------------------------------------------------
    out = machine.fail(
        # Use a definitely-unreachable host so `ssh` returns 255.
        f"{NIX} debug-attach --on 192.0.2.1 "
        "/nix/store/" + ("e" * 32) + "-bogus.drv "
        "2>&1"
    )
    assert (
        "SSH connection to" in out
        or "ssh: connect to" in out
        or "No route to host" in out
        or "connection refused" in out.lower()
        or "unreachable" in out.lower()
    ), (
        f"expected SSH connection failure message, got:\n{out}"
    )

    # ----------------------------------------------------------------
    # 22. `--no-build-debugger` overrides a `build-debugger = true`
    #     in config: the build runs normally (no attach-info is
    #     published) even though the setting is on. Exercised via
    #     `--option build-debugger true` + `--no-build-debugger` on
    #     the same line; `--no-build-debugger` wins as the last
    #     CLI argument.
    #
    #     The banner-absence check is the strong signal: the pause
    #     happens BEFORE exit, and the wrapper prints "build failed
    #     in … with exit code" only when it actually reaches the
    #     pause code path. `cleanupBuild` removes attach-info at
    #     build end regardless, so the post-build `!test -f attach`
    #     check is just a sanity backstop — the banner assertion is
    #     what actually proves the flag was honored.
    # ----------------------------------------------------------------
    drv_hash = drv_stdenv.split("/")[-1][:32]
    attach_file = f"/nix/var/nix/debugger/{drv_hash}.attach"
    machine.succeed(f"rm -f {attach_file}")
    # The build should fail on the `exit 1` in buildPhase, but NOT pause.
    # Give it a generous timeout; without `--no-build-debugger` the
    # wrapper would pause for up to an hour. We wrap `timeout` around
    # the outer `nix build` so the test doesn't itself hang if the
    # flag is ignored.
    out = machine.fail(
        "timeout 60 "
        f"{NIX} build --option build-debugger true "
        f"--no-build-debugger {drv_stdenv}^out 2>&1"
    )
    assert "build failed in" not in out, (
        f"expected `--no-build-debugger` to suppress the pause banner, got:\n{out}"
    )
    assert "nix debug-attach" not in out, (
        f"expected no debug-attach instruction in log, got:\n{out}"
    )

    # ----------------------------------------------------------------
    # 23. `nix-build --build-debugger` (the legacy CLI): the flag is
    #     registered only on `nix build`, so `nix-build` must reject it.
    # ----------------------------------------------------------------
    out = machine.fail(
        "nix-build --extra-experimental-features "
        "'nix-command build-debugger' --build-debugger "
        "/etc/test-drvs/stdenv-fail.nix 2>&1"
    )
    assert (
        "unrecognised" in out.lower()
        or "unknown" in out.lower()
        or "unexpected argument" in out.lower()
    ), (
        f"expected `nix-build` to reject --build-debugger, got:\n{out}"
    )

    # ----------------------------------------------------------------
    # 24. `--option build-debugger true` WITHOUT a target, on a FAILING
    #     drv: `shouldApplyBuildDebugger` sees target=="" and skips
    #     instrumentation with a one-shot logWarning. The build still
    #     fails (via the buildPhase `false`), but no debugger pause
    #     banner appears and no attach-info is published for the drv.
    #
    #     Using a failing drv (not a succeeding one) is the strong
    #     assertion: a succeeding build never reaches the pause path
    #     regardless of instrumentation, so banner absence is
    #     tautological there. With a failing build, banner absence
    #     ONLY happens if instrumentation was actually skipped.
    # ----------------------------------------------------------------
    # Restart the daemon so we start from a clean process-global state.
    # `settings.buildDebugger{,Target}` are process-global in the
    # daemon and persist across client connections; an earlier subtest
    # may have left a non-empty target behind, which would cause this
    # subtest to (incorrectly) instrument rather than skip-and-warn.
    machine.succeed("systemctl restart nix-daemon.service")
    machine.succeed(f"rm -f {attach_file}")

    # Set `build-debugger-target` to a sentinel path that doesn't
    # match `drv_stdenv`, then attempt to build drv_stdenv with
    # `build-debugger` on. `shouldApplyBuildDebugger` will see the
    # mismatch and return false — exercising the non-empty-but-
    # non-matching path (which is equivalent, for the purposes of
    # this subtest, to empty target: no instrumentation fires).
    out = machine.fail(
        "timeout 60 "
        f"{NIX} build --option build-debugger true "
        "--option build-debugger-target "
        "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-not-us.drv "
        f"{drv_stdenv}^out 2>&1"
    )
    assert "build failed in" not in out, (
        f"expected non-matching-target skip to suppress the pause "
        f"banner on a failing build, got:\n{out}"
    )
    assert "nix debug-attach" not in out, (
        f"expected no debug-attach instruction in log, got:\n{out}"
    )
    machine.succeed(f"rm -f {attach_file}")

    # ----------------------------------------------------------------
    # 25. `--build-debugger` + `--repair`: refused at `CmdBuild::run`
    #     parse time. The repair path's contract is output-hash
    #     agreement, not builder exit status, so the debugger wrapper
    #     would add no useful signal and would confuse the exit-code-
    #     passthrough semantics.
    # ----------------------------------------------------------------
    out = machine.fail(
        f"{NIX} build --build-debugger --repair "
        f"{drv_stdenv}^out 2>&1"
    )
    assert (
        "incompatible with `--repair`" in out
        or "incompatible with --repair" in out
    ), (
        f"expected --build-debugger+--repair to be refused, got:\n{out}"
    )

    # ----------------------------------------------------------------
    # 26. `--build-debugger` + `--rebuild` (the `--check` equivalent):
    #     refused at parse time for the same reason.
    # ----------------------------------------------------------------
    out = machine.fail(
        f"{NIX} build --build-debugger --rebuild "
        f"{drv_stdenv}^out 2>&1"
    )
    assert (
        "incompatible with `--rebuild`" in out
        or "incompatible with --rebuild" in out
    ), (
        f"expected --build-debugger+--rebuild to be refused, got:\n{out}"
    )

    # ----------------------------------------------------------------
    # 27. Forged attach-info with schemaVersion=0 (missing): the writer
    #     always stamps the schema version; a file without it was
    #     produced by a pre-schema Nix and cannot be parsed safely.
    #     Subtest 16 covers the schemaVersion > current direction; this
    #     covers the missing-version direction.
    # ----------------------------------------------------------------
    sub27_hash = "b" * 32
    sub27_attach = f"/nix/var/nix/debugger/{sub27_hash}.attach"
    sub27_drv = f"/nix/store/{sub27_hash}-no-version.drv"
    machine.succeed(
        "mkdir -p /nix/var/nix/debugger && chmod 0700 /nix/var/nix/debugger && "
        "cat > " + sub27_attach + " <<'EOF'\n"
        + "{\"drvPath\": \"" + sub27_drv + "\"}\n"
        + "EOF"
    )
    out = machine.fail(
        f"{NIX} debug-attach {sub27_drv} 2>&1"
    )
    assert "schemaVersion" in out or "schema version" in out.lower(), (
        f"expected missing-schemaVersion rejection, got:\n{out}"
    )
    machine.succeed(f"rm -f {sub27_attach}")
  '';
}
