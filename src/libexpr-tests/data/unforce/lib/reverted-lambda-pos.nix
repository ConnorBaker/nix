# As reverted-pos.nix, at.inner a function, whose position is the
# lambda's.
N:
let
  f = n: if n == N + 1 then (y: y) else { inner = f (n + 1); };
  x = f 0;
  at = builtins.foldl' (v: _: v.inner) x (builtins.genList (i: i) N);
in
builtins.seq at [
  (builtins.seq at.inner (import ./junk.nix 100))
  (builtins.deepSeq x null)
]
