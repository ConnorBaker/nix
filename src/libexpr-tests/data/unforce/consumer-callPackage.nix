# Shaped as `callPackage`: a package is a function of a shared, imported
# stdenv; later elements compare its platform, which has a function in it.
let
  stdenv = import ./lib/stdenv.nix;
  callPackage = f: f { inherit stdenv; };
  pkgs = {
    hello = callPackage (
      { stdenv }: {
        inherit stdenv;
        meta.hostPlatform = stdenv.hostPlatform;
      }
    );
  };
in
[
  (builtins.seq pkgs.hello true)
  (builtins.seq pkgs.hello.meta.hostPlatform true)
  (pkgs.hello.meta.hostPlatform == stdenv.hostPlatform)
  (pkgs.hello.meta.hostPlatform == (import ./lib/stdenv.nix).hostPlatform)
]
