# Unforcing (eval-unforce) must not change pointer identity, which `==`
# short-circuits on: a cell promoted after being forced in K jobs must not
# keep a copy of a value whose producer is reverted and rebuilt later.
# Each top-level element is one job (eval-unforce-batch-size = 0).
let
  P = { a = { b = { f = x: x; }; }; };
  # R shares a sub-object of P.a, not P.a itself.
  R = P.a.b;
in
[
  (builtins.seq R true)
  (P ? a)
  (builtins.seq R true)
  (R == P.a.b)
  (R == P.a.b)
]
