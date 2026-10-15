# Unforcing (eval-unforce) must not change pointer identity, which `==`
# short-circuits on: a cell promoted after being forced in K jobs must not
# keep a copy of a value whose producer is reverted and rebuilt later.
# Each top-level element is one job (eval-unforce-batch-size = 0).
let
  P = import ./unforce-identity-set.nix;
  R = P.a;
in
[
  (builtins.seq R true)
  (P ? a)
  (builtins.seq R true)
  (R == (import ./unforce-identity-set.nix).a)
]
