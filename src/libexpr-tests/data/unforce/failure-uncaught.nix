# As failure-memoised.nix, without `tryEval`: p fails, and so the
# traversal.
let
  x = {
    a = 1 + 1;
  };
  t = builtins.length x;
in
{
  p = builtins.seq (import ./lib/junk.nix 100) t;
  q = x;
  r = t;
}
