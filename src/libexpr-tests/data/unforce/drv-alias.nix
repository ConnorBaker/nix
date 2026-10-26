# Derivations found are kept across jobs (`DerivationsSeen`): an alias
# reaches the same set again and is dropped. x's job drops garbage, so
# what it keeps moves, and y and z, in later jobs, must find the set where
# it moved.
let
  d = import ./lib/d.nix;
  junk = import ./lib/junk.nix;
in
rec {
  x = builtins.seq (junk 100) (d "alias");
  y = x;
  z = y;
}
