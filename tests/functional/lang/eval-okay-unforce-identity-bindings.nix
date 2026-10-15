# Review notes/reviews/identity-comparison.md §2.2 (B1): Bindings identity, observed by the printer's
# «repeated». Job per element.
let
  Q = { d = { a = 1; }; };
  L = builtins.listToAttrs [ { name = "y"; value = Q.d; } ];
  R = L.y;
in [ (builtins.seq L true) (builtins.seq R true) { x = R; y = Q.d; } ]
