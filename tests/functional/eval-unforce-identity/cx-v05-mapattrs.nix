let Q = { d = { f = x: x; }; }; L = builtins.mapAttrs (n: v: v) { y = Q.d; }; R = L.y;
in [ (builtins.seq L true) (builtins.seq R true) (R == Q.d) (builtins.elem R [ Q.d ]) ]
