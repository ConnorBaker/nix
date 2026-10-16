# Collapsed replacement for the 30 generated e-X*/ae-X* repeated-printer
# fixtures, attribute-set form: the same shared-value / «repeated» property in
# a top-level attribute set, whose Bindings (sorted) order — in which
# `nix-instantiate --strict` forces it — is the reverse of the printed order
# (z9 first). Shared values still print «repeated» identically with and
# without unforcing, across the same spread of value shapes.
let
  s = {
    f = x: x;
    v = 1;
  };
  deep = { a = { b = s; }; };
  m = builtins.mapAttrs (_: v: v) { p = s; };
  ld = builtins.listToAttrs [ { name = "k"; value = s; } ];
in
{
  z9 = s;
  z8 = s; # plain attrs
  z7 = deep;
  z6 = deep.a.b; # nested (deep.a.b == s)
  z5 = m;
  z4 = m.p; # mapAttrs
  z3 = ld;
  z2 = ld.k; # listToAttrs
  z1 = [ s s ]; # list
}
