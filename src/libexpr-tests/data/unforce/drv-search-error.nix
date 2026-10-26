# An error during the search, after a derivation was found; c is never
# reached.
let
  d = import ./lib/d.nix;
in
{
  a = d "a";
  b = throw "boom in b";
  c = builtins.trace "c-trace" (d "c");
}
