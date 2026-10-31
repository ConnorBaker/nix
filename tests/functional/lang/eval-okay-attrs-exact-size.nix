# Sets with dynamic attributes (null names among them) and removeAttrs results, built to their exact size.
let k = "c"; z = null; l = { a = 1; b = 2; } // { c = 3; } // { b = 4; }; in {
  a = { ${z} = 1; };
  b = { ${"a"} = 1; b = 2; ${z} = 3; ${k} = 4; inherit k; };
  c = builtins.attrNames { ${"q"} = 1; ${"p"} = 2; m = 3; ${"zz"} = 4; };
  d = let f = x: { ${x} = x; }; in [ (f "a") (f "b") ];
  e = builtins.length (builtins.attrNames { ${z} = 1; });
  g = { ${z} = 1; } == { };
  h = rec { ${k} = x; x = 2; };
  i = let inherit ({ s = "zz"; }) s; in { ${s} = 1; t = 2; };
  r = map (builtins.removeAttrs l) [ [ ] [ "a" ] [ "b" "b" "x" ] [ "a" "b" "c" ] [ "c" "a" "a" "q" "b" ] ];
}
