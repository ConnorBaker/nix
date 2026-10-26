# Q.c reaches h through a force that fails, caught by `tryEval`.
let
  bad = throw "boom";
  h = Q.z;
  s = builtins.seq h bad;
  Q = {
    z = {
      f = x: x;
    };
    c = builtins.seq [ 1 ] (builtins.seq (builtins.tryEval s) 42);
  };
in
[
  (builtins.seq Q (builtins.seq Q.c [ h ]))
  (h == Q.z)
  [
    h
    Q.z
  ]
  null
]
