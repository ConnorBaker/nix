# Unforcing (eval-unforce) must not change pointer identity, which `==`
# short-circuits on: a cell promoted after being forced in K jobs must not
# keep a copy of a value whose producer is reverted and rebuilt later.
# Each top-level element is one job (eval-unforce-batch-size = 0).
let
  P =
    let
      s = { f = x: x; };
    in
    { get = _: s; };
  R = P.get null;
in
[
  (builtins.seq R true)
  (P ? get)
  (builtins.seq R true)
  (R == P.get null)
]
