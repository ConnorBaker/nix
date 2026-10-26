# Q.c reaches s through a cell of another owner, R.r.
let
  R = {
    r = builtins.seq s 7;
  };
  Q = {
    z = {
      f = x: x;
    };
    c = builtins.seq [ 1 ] (builtins.seq R.r 42);
  };
  s = Q.z;
in
[
  (builtins.seq Q (builtins.seq R (builtins.seq Q.c [ s ])))
  (s == Q.z)
  [
    s
    Q.z
  ]
  null
]
