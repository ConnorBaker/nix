# Functions in a list are called with their defaults: x is found once
# however it is reached; two equal calls are two derivations.
let
  d = import ./lib/d.nix;
  junk = import ./lib/junk.nix;
  x = d "x";
in
[
  ({ ... }: builtins.seq (junk 1000) x)
  ({ ... }: x)
  x
  ({ ... }: d "fresh")
  ({ ... }: d "fresh")
]
