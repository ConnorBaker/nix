# no separate consumer cell: job 2 forces L then L.y directly
let Q = { d = { f = x: x; }; }; L = { y = Q.d; };
in [ (builtins.seq L true) (builtins.seq L.y true) (L.y == Q.d) (builtins.elem L.y [ Q.d ]) ]
