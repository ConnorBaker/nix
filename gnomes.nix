let
  # The resulting drvPath should be divorced from anything -- just a string.
  getGnomeTestDrvPath =
    _:
    let
      drvPath =
        builtins.unsafeDiscardStringContext
          (import (
            (builtins.getFlake "github:nixos/nixpkgs/e2b4ce9a00fcb6458016c1fd626f336970017ea0").outPath
            + "/nixos/release.nix"
          ) { }).closures.gnome.aarch64-linux;
    in
    builtins.deepSeq drvPath drvPath;

  gnomes = builtins.genList (
    n: builtins.trace "processed gnome ${builtins.toString n}" (getGnomeTestDrvPath n)
  ) 10;
in
gnomes
