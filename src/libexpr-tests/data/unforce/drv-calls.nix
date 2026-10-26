# Two calls with equal arguments: two sets with one drvPath, found twice;
# an alias of the second once.
let
  d = import ./lib/d.nix;
  junk = import ./lib/junk.nix;
in
rec {
  x = builtins.seq (junk 100) (d "same");
  y = d "same";
  z = y;
}
