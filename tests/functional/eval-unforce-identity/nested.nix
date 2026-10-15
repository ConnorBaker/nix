# demo1 inside an attribute set: with the mimalloc backend every nested
# scope is a job (review §1.3).
let
  P = { a = { f = x: x; }; };
  R = P.a;
in
{ x = [
  (builtins.seq R true)
  (P ? a)
  (builtins.seq R true)
  (R == P.a)
]; }
