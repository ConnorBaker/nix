# A consumer's force of a producer's cell: the first element forces L, the
# second R = L.y, a cell that L's force made and left a thunk, whose value
# is Q.d. The consumer-*.nix programs vary how L reaches Q.d.
let
  Q = {
    d = {
      f = x: x;
    };
  };
  L = builtins.listToAttrs [
    {
      name = "y";
      value = Q.d;
    }
  ];
  R = L.y;
in
[
  (builtins.seq L true)
  (builtins.seq R true)
  (R == Q.d)
  (builtins.elem R [ Q.d ])
]
