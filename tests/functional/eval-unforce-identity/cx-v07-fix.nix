let Q = { d = { f = x: x; }; }; F = (self: { y = Q.d; z = self.y; }) F; R = F.z;
in [ (builtins.seq F true) (builtins.seq R true) (R == Q.d) (builtins.elem R [ Q.d ]) ]
