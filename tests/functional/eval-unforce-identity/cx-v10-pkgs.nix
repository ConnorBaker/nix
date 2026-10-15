# Nixpkgs-shaped: a fixpoint package set; job 1 forces the set, job 2 a package whose attribute reads another
# package's platform record; later jobs compare platforms (the pattern of stdenv.hostPlatform == ...).
let
  pkgs = (self: {
    platform = { system = "x"; canExecute = p: true; };
    stdenv = { hostPlatform = self.platform; };
    hello = { meta.platforms = [ self.stdenv.hostPlatform ]; };
  }) pkgs;
in [ (builtins.seq pkgs true) (builtins.seq (builtins.head pkgs.hello.meta.platforms) true)
     (builtins.head pkgs.hello.meta.platforms == pkgs.stdenv.hostPlatform)
     (builtins.elem pkgs.stdenv.hostPlatform pkgs.hello.meta.platforms) ]
