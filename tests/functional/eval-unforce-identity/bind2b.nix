# Review notes/reviews/identity-comparison.md §2.2 (B1): a promoted cell kept a copy of a reverted
# producer's Bindings; base's printer shows the second occurrence as «repeated».
let
  Q = { d = { a = 1; }; };
  L = Q // { y = Q.d; };
  R = L.y;
in [ (builtins.seq L true) (builtins.seq R true) { x = R; y = Q.d; } ]
