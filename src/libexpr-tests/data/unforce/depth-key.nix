# Two cells of one expression, first forced at very different call depths:
# a.r ~9000 calls deep (it needs 100 more), b.r at the top (it needs 9000).
# Re-forced in the third element, b.r runs at its own first depth: at
# a.r's, it would need ~18000, more than max-call-depth.
let
  deep = n: x: if n == 0 then x else deep (n - 1) x;
  deepS = n: x: if n == 0 then builtins.seq x x else deepS (n - 1) x;
  f = n: {
    r =
      let
        x = deep n n;
      in
      builtins.seq x [ x ];
  };
  a = f 100;
  b = f 9000;
in
builtins.seq a (
  builtins.seq b [
    (deepS 9000 a.r)
    b.r
    b.r
  ]
)
