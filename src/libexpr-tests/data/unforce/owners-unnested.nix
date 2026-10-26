# As owners-back.nix, without Q.c's force around s's.
let
  Q = {
    z = {
      f = x: x;
    };
    c = 42;
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
  (builtins.seq Q (builtins.seq s [ s ]))
  (s == Q.z)
  [
    s
    Q.z
  ]
  null
]
