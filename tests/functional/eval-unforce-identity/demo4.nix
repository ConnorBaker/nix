# R keeps a sub-object of the reverted thunk's result; once both sides are
# promoted the split would be permanent.
let
  P = { a = { b = { f = x: x; }; }; };
  R = P.a.b;
in
[
  (builtins.seq R true)
  (P ? a)
  (builtins.seq R true)
  (R == P.a.b)
  (R == P.a.b)
]
