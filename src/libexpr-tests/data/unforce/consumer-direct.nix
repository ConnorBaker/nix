# With no cell of the consumer's own: the second element forces L.y.
let
  Q = {
    d = {
      f = x: x;
    };
  };
  L = {
    y = Q.d;
  };
in
[
  (builtins.seq L true)
  (builtins.seq L.y true)
  (L.y == Q.d)
  (builtins.elem L.y [ Q.d ])
]
