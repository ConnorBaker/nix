# Bindings identity (B1): a promoted cell kept a copy of a reverted
# producer's Bindings; base's printer shows the second occurrence as «repeated».
let
  Q = { d = { a = 1; }; };
  L = builtins.listToAttrs [ { name = "y"; value = Q.d; } ];
  R = L.y;
  S = { x = R; y = Q.d; };
in [ (builtins.seq L true) (builtins.seq R true) (builtins.deepSeq S (builtins.trace S true)) ]
