# No error: an alias found once, a copy with an equal drvPath again.
let
  d = import ./lib/d.nix;
in
rec {
  a = d "x";
  b = a;
  c = a // {
    tag = 1;
  };
  e = d "e";
  l = {
    recurseForDerivations = true;
    m = d "m";
    n = c;
  };
}
