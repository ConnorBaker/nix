# As owners-older.nix, reached from a cell of another owner, compared
# through a list and a set.
let
  R = {
    r = builtins.seq (builtins.genList (i: i) 4) Q.c;
  };
  Q = {
    z = [
      (x: x)
      (y: y)
      (w: w)
    ];
    c = builtins.length Q.z + builtins.length (builtins.genList (i: i) 2);
  };
  h = Q.z;
in
[
  (builtins.seq Q (
    builtins.seq R (builtins.seq (builtins.genList (i: i) 3) (builtins.seq R.r [ h ]))
  ))
  (h == Q.z)
  ([ h ] == [ Q.z ])
  ({ a = h; } == { a = Q.z; })
  [
    h
    Q.z
  ]
  null
]
