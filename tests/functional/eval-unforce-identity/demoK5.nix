# The demo1 schedule for promotion after 5 forces.
let
  P = { a = { f = x: x; }; };
  R = P.a;
in
[
  (builtins.seq R true)
  (builtins.seq R true)
  (builtins.seq R true)
  (builtins.seq R true)
  (P ? a)
  (builtins.seq R true)
  (R == P.a)
]
