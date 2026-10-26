# A producer forced before its consumer starts: in the third element P.a
# is forced, then R, which reads P.a.b, a cell that P.a's force made.
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
  (builtins.seq P.a (builtins.seq R true))
  (R == P.a.b)
]
