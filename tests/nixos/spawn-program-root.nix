# spawnProgram's tests that need root (RootSpawnProgram, which the unit
# tests leave out), run as root, both on the vfork() path and, with ASan,
# on the fork() path.
{ nixComponents, ... }:
let
  sanitized = nixComponents.overrideScope (
    self: super: {
      withASan = true;
      withUBSan = true;
    }
  );
in
{
  name = "spawn-program-root";

  nodes.machine = { };

  testScript = ''
    machine.wait_for_unit("multi-user.target")
    for tests in ["${nixComponents.nix-util-tests}", "${sanitized.nix-util-tests}"]:
        machine.succeed(f"cd /tmp && GTEST_FILTER='RootSpawnProgram.*:spawnProgram.*:killUser.*' ASAN_OPTIONS=detect_leaks=0 {tests}/bin/nix-util-tests >&2")
  '';
}
