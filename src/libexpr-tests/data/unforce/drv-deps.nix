# Derivations that depend on others found by the same search: b's and c's
# drvPaths compute a's, k's computes j's; some warn while their drvPath
# is computed (`__json`, `allowedReferences` with structured attributes,
# `builtins.warn`).
let
  dd =
    name: extra:
    derivation (
      {
        inherit name;
        system = "x86_64-linux";
        builder = "/bin/sh";
      }
      // extra
    );
in
rec {
  a = dd "a" {
    __structuredAttrs = true;
    allowedReferences = [ ];
  };
  b = dd "b" { dep = a; };
  c = dd "c" {
    dep = a;
    dep2 = b;
  };
  j = dd "j" { __json = "{}"; };
  k = dd "k" { dep = j.drvPath; };
  l = dd "l" { x = builtins.warn "W-l" 1; };
  m = import ./lib/d.nix (
    builtins.unsafeDiscardStringContext (builtins.substring 0 3 (baseNameOf a.drvPath))
  );
}
