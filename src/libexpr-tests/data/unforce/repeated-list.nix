# A container a printer prints in one element's job prints as «repeated»
# when a later element reaches it again: the printers' sets of what they
# printed stay exact across jobs (`UnforceSeen`). One shared value reached
# twice through many shapes of values: sets, a list, a closure, a deep set,
# an imported file, and the sets that mapAttrs, listToAttrs, foldl',
# genericClosure and partition build.
let
  s = {
    f = x: x;
    v = 1;
  };
  l = [
    s
    s
  ];
  deep = {
    a = {
      b = {
        c = s;
      };
    };
  };
  fn = _: s;
  m = builtins.mapAttrs (_: v: v) {
    p = s;
    q = s;
  };
  ld = builtins.listToAttrs [
    {
      name = "x";
      value = s;
    }
    {
      name = "y";
      value = s;
    }
  ];
  fo = builtins.foldl' (acc: x: acc // x) { } [
    { a = s; }
    { d = deep; }
  ];
  gc = builtins.genericClosure {
    startSet = [
      {
        key = 0;
        v = s;
      }
    ];
    operator =
      x:
      if x.key < 1 then
        [
          {
            key = 1;
            v = x.v;
          }
        ]
      else
        [ ];
  };
  pt = builtins.partition (x: x ? f) [
    s
    deep
  ];
  I = import ./lib/shapes.nix;
in
[
  s
  s
  l
  l
  deep
  deep.a.b.c
  (fn 1)
  (fn 2)
  m
  m.p
  m.q
  ld
  ld.x
  ld.y
  fo
  fo.a
  fo.d
  (builtins.elemAt gc 0).v
  (builtins.elemAt gc 1).v
  (builtins.head pt.right)
  I
  I.b.c
  (import ./lib/shapes.nix).a
]
