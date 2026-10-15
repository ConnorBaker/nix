let Q = { d = { f = x: x; }; }; L = { g = _: Q.d; }; R = L.g null;
in [ (builtins.seq L true) (builtins.seq R true) (R == Q.d) (builtins.elem R [ Q.d ]) ]
