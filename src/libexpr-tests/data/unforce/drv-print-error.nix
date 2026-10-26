# The search succeeds, and b's drvPath fails after it, after a's.
let
  d = import ./lib/d.nix;
in
{
  a = d "a";
  b = d "b" // {
    drvPath = throw "b-drvPath fails";
  };
  c = d "c";
}
