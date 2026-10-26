# An imported file's value, which the evaluator caches: a later import of
# it is the same value.
let
  P = import ./lib/set.nix;
  R = P.a;
in
[
  (builtins.seq R true)
  (P ? a)
  (builtins.seq R true)
  (R == (import ./lib/set.nix).a)
]
