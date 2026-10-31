# builtins.mapAttrs with every shape of function and argument; failed attributes caught and read again.
let
  inherit (builtins) mapAttrs tryEval foldl' genList attrValues attrNames seq add;
  s = { a = 1; b = 2; c = 3; };
  layered = foldl' (acc: i: acc // { "k${toString i}" = i; }) s (genList (x: x) 40);
  sum = foldl' (a: b: a + b) 0;
  m = mapAttrs (n: v: assert v > 0; v) { a = 0; b = 1; };
  mf = mapAttrs (n: assert n != "a"; v: v) { a = 0; b = 1; };
in
{
  m1 = mapAttrs (n: v: "${n}=${toString v}") s;
  m2 = mapAttrs builtins.seq s;
  m3 = mapAttrs { __functor = self: n: v: v * 10; } s;
  m4 = mapAttrs (n: add 100) s;
  m5 = mapAttrs (n: v: v + 1) layered;
  m6 = mapAttrs (n: v: mapAttrs (n2: v2: "${n}.${n2}") { x = v; }) s;
  m7 = mapAttrs (n: v: v) { };
  m8 = attrNames (mapAttrs (n: throw "never forced") s);
  t1 = [ (tryEval m.a).success (tryEval m.a).success m.b (tryEval mf.a).success (tryEval mf.a).success mf.b ];
  steps = sum (attrValues (foldl' (acc: i: acc // mapAttrs (n: v: v + i) { "x${toString i}" = i; y = i; }) { } (genList (x: x) 3000)));
  steps2 = foldl' (acc: i: seq (mapAttrs (n: v: v * i) { z = i; }).z (acc + i)) 0 (genList (x: x) 3000);
  eq = [ (mapAttrs (n: v: v) s == s) ((mapAttrs (n: v: v) { f = x: x; }) == (mapAttrs (n: v: v) { f = x: x; })) ];
}
