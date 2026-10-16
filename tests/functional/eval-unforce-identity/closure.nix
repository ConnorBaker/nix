# A lambda's environment slot is copied as a thunk.
let
  P =
    let
      s = { f = x: x; };
    in
    { get = _: s; };
  R = P.get null;
in
[
  (builtins.seq R true)
  (P ? get)
  (builtins.seq R true)
  (R == P.get null)
]
