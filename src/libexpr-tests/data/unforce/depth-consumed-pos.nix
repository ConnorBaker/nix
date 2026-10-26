# As depth-consumed.nix, for a chain: element 0 forces x.inner, older than
# the jobs, and then, after that force ended, the 10100 levels below it that
# it built. Element 1 forces the chain deeply, past max-call-depth about
# 10000 levels down: at a forced level, whose position the error reports.
# Rebuilt unforced, the chain would be exceeded a frame sooner, at a call.
let
  f = n: { inner = f (n + 1); };
  x = f 0;
in
builtins.seq x [
  (builtins.seq (builtins.foldl' (v: _: v.inner) x (builtins.genList (i: i) 10100)) (
    import ./lib/junk.nix 100
  ))
  (builtins.deepSeq x null)
]
