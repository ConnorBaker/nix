# As owners-back.nix, Q.z a list.
let
  Q = {
    z = [
      (x: x)
      (y: y)
      (w: w)
    ];
    c = builtins.seq s 42;
  };
  s =
    let
      a = [
        1
        2
        3
      ];
    in
    builtins.seq a Q.z;
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
