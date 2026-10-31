let
  f = x: assert x.ok; 1;
  r = builtins.tryEval (builtins.seq (builtins.tryEval (f { ok = false; })).success (f { ok = false; }));
in
builtins.seq r.success (builtins.break r.success)
