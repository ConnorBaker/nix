# Shared configuration for every `build-debugger-*.nix` VM test.
#
# Each split file imports this module and adds its own `testScript` +
# any test-specific `environment.etc."test-drvs/*.nix"` fixtures. The
# goal of keeping this module minimal is to let each split file run a
# short VM (≲ 3 min) so the whole suite parallelises well on a
# multi-core builder.
{ lib, ... }:

{
  # We rarely change the scripts in a way that type-checking would
  # catch; skipping saves a non-trivial chunk of per-drv eval time.
  # Matches the pattern set by `tests/nixos/functional/common.nix`.
  skipTypeCheck = true;

  nodes.machine =
    { pkgs, ... }:
    {
      virtualisation.writableStore = true;
      virtualisation.memorySize = 2048;
      virtualisation.cores = 2;
      virtualisation.additionalPaths = [
        pkgs.bash
        pkgs.python3
        pkgs.coreutils
        pkgs.stdenv
      ];

      nix.settings.substituters = lib.mkForce [ ];
      nix.settings.experimental-features = [
        "nix-command"
        "build-debugger"
        "ca-derivations"
      ];
      nix.settings.trusted-users = [
        "root"
        "alice"
      ];
      nix.settings.allowed-users = [
        "root"
        "alice"
        "mallory"
      ];

      users.users.alice = { isNormalUser = true; };
      users.users.mallory = { isNormalUser = true; };

      environment.systemPackages = [
        # `script` lives in `util-linux` — every split file uses the
        # `script -qc` + piped-stdin idiom to drive interactive
        # `nix debug-attach` sessions over a PTY.
        pkgs.util-linux
        # Used by the `nix-instantiate` fixtures that read `<nixpkgs>`.
        pkgs.python3
      ];
      nix.nixPath = [ "nixpkgs=${pkgs.path}" ];
    };
}
