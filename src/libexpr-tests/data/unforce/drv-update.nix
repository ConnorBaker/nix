# `x // { tag = 1; }` is another set with x's drvPath: found again.
# `x // { }` is x itself: not.
let
  d = import ./lib/d.nix;
  junk = import ./lib/junk.nix;
in
rec {
  x = builtins.seq (junk 100) (d "update");
  y = x // { };
  z = x // {
    tag = 1;
  };
}
