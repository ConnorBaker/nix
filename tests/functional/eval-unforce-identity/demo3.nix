let
  P = { a = { f = x: x; }; };
  R = P.a;
in
[
  (builtins.seq R true)
  (P ? a)
  (builtins.seq R true)
  (
    assert R == P.a;
    "ok"
  )
]
