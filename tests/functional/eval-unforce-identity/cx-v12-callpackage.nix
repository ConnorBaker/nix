# callPackage-shaped: packages are functions of a shared, imported stdenv; the traversal forces a package set
# element, then its drv attribute; a later element compares hostPlatform values (pointer identity on functions).
let
  stdenv = import ./cx-lib/stdenv.nix;
  callPackage = f: f { inherit stdenv; };
  pkgs = { hello = callPackage ({ stdenv }: { inherit stdenv; meta.hostPlatform = stdenv.hostPlatform; }); };
in [ (builtins.seq pkgs.hello true) (builtins.seq pkgs.hello.meta.hostPlatform true)
     (pkgs.hello.meta.hostPlatform == stdenv.hostPlatform) (pkgs.hello.meta.hostPlatform == (import ./cx-lib/stdenv.nix).hostPlatform) ]
