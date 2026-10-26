# Derivations reached again in later jobs: directly, inside a
# `recurseForDerivations` set, through a selection, and from a file
# imported twice (one set).
let
  d = import ./lib/d.nix;
  junk = import ./lib/junk.nix;
  x = d "x";
in
rec {
  a = builtins.seq (junk 1000) x;
  b = builtins.seq (junk 1000) {
    recurseForDerivations = true;
    p = a;
    q = d "q";
    r = x // {
      tag = 1;
    };
  };
  c = builtins.seq (junk 1000) b.q;
  e = builtins.seq (junk 1000) (import ./lib/drvfile.nix);
  f = import ./lib/drvfile.nix;
  g = a;
  h = b;
}
