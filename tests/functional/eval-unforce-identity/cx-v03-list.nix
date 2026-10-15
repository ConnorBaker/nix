let Q = { d = { f = x: x; }; }; L = { l = [ Q.d ]; }; R = builtins.head L.l;
in [ (builtins.seq L true) (builtins.seq R true) (R == Q.d) (builtins.elem R [ Q.d ]) ]
