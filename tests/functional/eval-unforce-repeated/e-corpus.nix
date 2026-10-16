# Collapsed replacement for the 30 generated e-X*/ae-X* repeated-printer
# fixtures. A container reached a second time at top level prints as
# «repeated» — identically with and without unforcing and at every batch
# size, because the printers keep their `seen` sets exact across jobs. This
# one list reaches the same shared values twice through a spread of value
# shapes: plain attrs, a list, a function closure, a deeply nested set, an
# imported file, and the sets that mapAttrs / listToAttrs / foldl' /
# genericClosure / partition build.
let
  s = {
    f = x: x;
    v = 1;
  };
  l = [ s s ];
  deep = { a = { b = { c = s; }; }; };
  fn = _: s;
  m = builtins.mapAttrs (_: v: v) {
    p = s;
    q = s;
  };
  ld = builtins.listToAttrs [
    { name = "x"; value = s; }
    { name = "y"; value = s; }
  ];
  fo = builtins.foldl' (acc: x: acc // x) { } [
    { a = s; }
    { d = deep; }
  ];
  gc = builtins.genericClosure {
    startSet = [ { key = 0; v = s; } ];
    operator = x: if x.key < 1 then [ { key = 1; v = x.v; } ] else [ ];
  };
  pt = builtins.partition (x: x ? f) [ s deep ];
  I = import ./set.nix;
in
[
  s
  s # plain attrs
  l
  l # list
  deep
  deep.a.b.c # nested / deep (deep.a.b.c == s)
  (fn 1)
  (fn 2) # function-returned shared value
  m
  m.p
  m.q # mapAttrs
  ld
  ld.x
  ld.y # listToAttrs
  fo
  fo.a
  fo.d # foldl'
  (builtins.elemAt gc 0).v
  (builtins.elemAt gc 1).v # genericClosure
  (builtins.head pt.right) # partition
  I
  I.b.c
  (import ./set.nix).a # imported file (memoised)
]
