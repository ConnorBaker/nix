# Q.c forces Q.z, an older cell of its own owner, after the element has
# allocated in between.
let
  Q = {
    z = {
      f = x: x;
    };
    c = builtins.seq Q.z 42;
  };
  h = Q.z;
in
[
  (builtins.seq Q (builtins.seq (builtins.genList (i: i) 3) (builtins.seq Q.c [ h ])))
  (h == Q.z)
  [
    h
    Q.z
  ]
  null
]
