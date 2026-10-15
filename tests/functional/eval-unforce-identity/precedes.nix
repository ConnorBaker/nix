# A producer that is not nested in the consumer's force. In the third
# element P.a (an unforced copy made when P was promoted) is forced first;
# R is forced afterwards and forces P.a.b, a young thunk P.a's force
# created. R's result was allocated during R's force, yet it is rebuilt
# when P.a is re-forced.
let
  P = { a = { b = { f = x: x; }; }; };
  R = P.a.b;
in
[
  (builtins.seq R true)
  (P ? a)
  (builtins.seq P.a (builtins.seq R true))
  (R == P.a.b)
]
