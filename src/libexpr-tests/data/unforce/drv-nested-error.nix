# An error in a nested `recurseForDerivations` set, after derivations were
# found at both levels.
let
  d = import ./lib/d.nix;
in
{
  a = d "a";
  s = {
    recurseForDerivations = true;
    x = d "x";
    y = throw "nested error";
  };
}
