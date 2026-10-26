# As repeated-list.nix, in a set whose attributes are forced in another
# order (z9 first) than they are printed in.
let
  s = {
    f = x: x;
    v = 1;
  };
  deep = {
    a = {
      b = s;
    };
  };
  m = builtins.mapAttrs (_: v: v) { p = s; };
  ld = builtins.listToAttrs [
    {
      name = "k";
      value = s;
    }
  ];
in
{
  z9 = s;
  z8 = s;
  z7 = deep;
  z6 = deep.a.b;
  z5 = m;
  z4 = m.p;
  z3 = ld;
  z2 = ld.k;
  z1 = [
    s
    s
  ];
}
