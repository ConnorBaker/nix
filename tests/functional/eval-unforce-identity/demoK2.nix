# The demo1 schedule for promotion after 2 forces (review §1.3).
let
  P = { a = { f = x: x; }; };
  R = P.a;
in
[
  (builtins.seq R true)
  (P ? a)
  (builtins.seq R true)
  (R == P.a)
]
