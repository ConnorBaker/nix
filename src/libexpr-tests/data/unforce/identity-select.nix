# Pointer identity across jobs: R is P.a's value, forced in one element's
# job and compared with P.a in a later one (`==` short-cuts on identity:
# a function compares equal only to itself).
let
  P = {
    a = {
      f = x: x;
    };
  };
  R = P.a;
in
[
  (builtins.seq R true)
  (P ? a)
  (builtins.seq R true)
  (R == P.a)
]
