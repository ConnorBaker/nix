# A lib.fix-style self-reference (review §1.3 mapattrs.nix), without Nixpkgs.
let
  fix = f: let x = f x; in x;
  unique = builtins.foldl' (acc: e: if builtins.elem e acc then acc else acc ++ [ e ]) [ ];
  P = fix (self: { a = { f = x: x; }; b = self.a; });
  R = P.b;
in
[
  (builtins.seq R true)
  (P ? a)
  (builtins.seq R true)
  (R == P.a)
  (unique [ R P.a ] == [ R ])
]
