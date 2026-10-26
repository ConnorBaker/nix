# Shaped as Nixpkgs: a fixed-point package set; a package's attribute reads
# another's platform, which later elements compare (as `stdenv.hostPlatform
# == ...` does).
let
  pkgs =
    (self: {
      platform = {
        system = "x";
        canExecute = p: true;
      };
      stdenv = {
        hostPlatform = self.platform;
      };
      hello = {
        meta.platforms = [ self.stdenv.hostPlatform ];
      };
    })
      pkgs;
in
[
  (builtins.seq pkgs true)
  (builtins.seq (builtins.head pkgs.hello.meta.platforms) true)
  (builtins.head pkgs.hello.meta.platforms == pkgs.stdenv.hostPlatform)
  (builtins.elem pkgs.stdenv.hostPlatform pkgs.hello.meta.platforms)
]
