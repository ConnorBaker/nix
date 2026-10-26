# What a job keeps is all it allocated but a small list below it: it moves
# down over that list, or stays, and the next job's first Env has a
# forward reference.
let
  xs = builtins.genList (i: { i = i; }) 100000;
  fwd =
    k:
    let
      za = zb;
      zb = k;
    in
    za;
in
[
  (builtins.length xs)
  (builtins.seq [ 1 2 3 ] (builtins.length xs))
  (fwd 3)
  (builtins.toJSON (builtins.elemAt xs 99999))
  (builtins.seq [ 4 5 6 ] (builtins.length xs))
  (fwd 4)
]
