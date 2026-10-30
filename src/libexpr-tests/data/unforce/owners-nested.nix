# Owner segments (unforce.cc, `setOwner`): in the first element, Q.c's
# force, nested in it, forces s and with it Q.z, whose set is built
# under Q next to Q.c's own segment. A printed value holds Q.z's set, so Q
# keeps its value with it: `s == Q.z` and «repeated» print as without
# unforcing.
let
  Q = {
    z = {
      f = x: x;
    };
    c = builtins.seq [ 1 ] (builtins.seq s 42);
  };
  s = Q.z;
in
[
  (builtins.seq Q (builtins.seq Q.c [ s ]))
  (s == Q.z)
  [
    s
    Q.z
  ]
  null
]
