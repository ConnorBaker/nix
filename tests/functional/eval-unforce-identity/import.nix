# The fileEvalCache cell of an imported file (review §2.2(b)).
let
  P = import ./set.nix;
  R = P.a;
in
[
  (builtins.seq R true)
  (P ? a)
  (builtins.seq R true)
  (R == (import ./set.nix).a)
]
