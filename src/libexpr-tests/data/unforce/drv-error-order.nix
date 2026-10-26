# a's drvPath fails, but is forced only after the search; b's name fails
# during it: b's error is reported.
let
  d = import ./lib/d.nix;
in
{
  a = d "a" // {
    drvPath = throw "a-drvPath fails";
  };
  b = {
    type = "derivation";
    name = throw "b-name fails";
  };
}
