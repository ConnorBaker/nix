# The review's demo1 (notes/research/identity-reliance.md §3.1). With a job
# per element and promotion after 2 forces, R was promoted holding a copy of
# the result of P.a's thunk, which was reverted and later rebuilt.
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
