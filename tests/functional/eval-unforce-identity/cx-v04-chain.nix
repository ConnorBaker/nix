let Q = { d = { f = x: x; }; }; M = { e = Q.d; }; L = { y = M.e; }; R = L.y;
in [ (builtins.seq L true) (builtins.seq M true) (builtins.seq R true) (R == Q.d) (builtins.elem R [ Q.d ]) ]
