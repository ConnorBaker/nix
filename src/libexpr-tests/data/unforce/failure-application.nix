# As failure-memoised.nix, for an application's cell (an element of
# `map`'s result), which is young.
let
  x = {
    a = 1 + 1;
  };
  l = builtins.map (
    y:
    assert y == x;
    y
  ) [ 5 ];
in
{
  p = builtins.seq (import ./lib/junk.nix 100) (builtins.tryEval (builtins.elemAt l 0)).success;
  q = x;
  r = builtins.elemAt l 0;
}
