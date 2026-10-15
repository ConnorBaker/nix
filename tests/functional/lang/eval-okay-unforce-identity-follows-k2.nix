# The comparison's counterexample to count-only closures (notes/identity/COMPARISON.md §1.4):
# R = L.y is forced after L's force ended and pulls Q's young result into L, which
# reaches the default K = 2 while Q was forced only once. Job per element.
let
  Q = { d = { f = x: x; }; };
  L = builtins.listToAttrs [ { name = "y"; value = Q.d; } ];
  R = L.y;
in [
  (builtins.seq L true)
  (builtins.seq R true)
  (R == Q.d)
  (builtins.elem R [ Q.d ])
]
