# Plain lambdas whose call Env cannot outlive the call, and ones whose Env escapes in a thunk, a closure or an Env.
let
  f = x: x; g = x: { a = x; b = 1; c = "s"; d = [ ]; }; h = x: [ x ]; k = x: x.y or 2; m = x: if x then 1 else 2;
  n = x: "${x}-s"; c = x: y: x + y; p = x: x // { q = 1; }; q = x: x == x; r = x: !x; s = x: x ++ [ ];
  t = x: assert x; 7; u = x: __curPos; v = x: { inherit x; }; w = x: (y: y) x; z = x: f (f x);
  ap = x: builtins.add x 1; sel = x: x.a.b; has = x: x ? a.b; pth = x: ./. + "/${x}";
  e1 = x: { a = x + 1; }; e2 = x: [ (x * 2) ]; e3 = x: let y = x + 3; in { inherit y; }; e4 = x: with { o = 1; }; [ (x + o) ];
  e5 = x: f (x + 5); e6 = x: { ${toString x} = x; }; e7 = x: rec { a = x; b = a; }; e8 = with { o = 3; }; x: [ o x ];
  run = fn: map (i: fn i) [ 1 2 3 ];
in [ (f 1) (g 2) (h 3) (k { }) (k { y = 5; }) (m true) (n "a") (c 1 2) (p { r = 0; }) (q 3) (r false) (s [ 1 ]) (t true)
  (u 0).column (v 9) (w 4) (z 6) (ap 4) (sel { a.b = 8; }) (has { a.b = 1; }) (builtins.baseNameOf (pth "zz"))
  (builtins.foldl' (a: b: a + b) 0 [ 1 2 3 ]) ((x: x) (y: y) 5) (builtins.map f [ 1 2 ]) (builtins.genList (x: x * 2) 3)
  (builtins.tryEval (t false)) (map (fn: run fn) [ e1 e2 e3 e4 e5 e6 e7 e8 (x: c x) ]) (map (g: g 10) (run c)) ]
