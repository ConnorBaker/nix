# R is part of P.a's value, not all of it.
let
  P = {
    a = {
      b = {
        f = x: x;
      };
    };
  };
  R = P.a.b;
in
[
  (builtins.seq R true)
  (P ? a)
  (builtins.seq R true)
  (R == P.a.b)
  (R == P.a.b)
]
