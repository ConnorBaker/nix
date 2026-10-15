# Q forced in job 1 too: Q count 2 in job 2 as well
let Q = { d = { f = x: x; }; }; L = { y = Q.d; }; R = L.y;
in [ (builtins.seq L (builtins.seq Q true)) (builtins.seq R true) (R == Q.d) (builtins.elem R [ Q.d ]) ]
