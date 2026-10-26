# Q forced in the first element too.
let
  Q = {
    d = {
      f = x: x;
    };
  };
  L = {
    y = Q.d;
  };
  R = L.y;
in
[
  (builtins.seq L (builtins.seq Q true))
  (builtins.seq R true)
  (R == Q.d)
  (builtins.elem R [ Q.d ])
]
