# Boolean frames (if, assert, &&, ||, ->, !): what a condition allocated
# goes back as it ends, unless something outside it points into it. Each
# element reads, after a condition, what it built: through a young cell
# made before it, or through `shared`, a target; what the element made just
# before it; and a failure caught in it, which the job's end reads.
let
  o.f = x: builtins.length x.l;
  big = n: { l = builtins.genList (i: { v = i; }) n; };
  shared = big 3000;
  young =
    b:
    assert o.f b == 3000;
    (builtins.elemAt b.l 2999).v;
  below = xs: if o.f (big 3000) > 0 then builtins.elemAt xs 50 else 0;
  target = s: if o.f s > 0 && !(o.f s < 0) then (builtins.elemAt s.l 1500).v else 0;
  nested =
    s:
    if (if o.f (big 3000) > 0 then s.l != [ ] else false) -> o.f s > 0 then
      (builtins.elemAt s.l 7).v
    else
      0;
in
{
  young = young (big 3000);
  below = below (builtins.genList (i: i * 2) 100);
  target = target shared;
  nested = nested shared;
  failed = if (builtins.tryEval (throw "x")).success || o.f (big 3000) == 0 then 1 else 2;
}
