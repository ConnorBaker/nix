# As compact-deep.nix, every level a set of 8 `//` layers with the link in
# the bottom one: the walks defer the base layers too.
let
  garbage =
    k:
    builtins.length (
      builtins.genList (i: [
        i
        k
      ]) 3000
    );
  g = k: v: builtins.seq (garbage k) v;
  level =
    acc: k:
    builtins.foldl' (s: n: s // { ${n} = k; }) { a = acc; } [
      "c"
      "d"
      "e"
      "f"
      "g"
      "h"
      "q"
    ];
  chain = builtins.foldl' level null (builtins.genList (i: i) 5000);
  depth =
    c:
    builtins.length (
      builtins.genericClosure {
        startSet = [
          {
            key = 0;
            inherit c;
          }
        ];
        operator =
          x:
          if x.c.a == null then
            [ ]
          else
            [
              {
                key = x.key + 1;
                c = x.c.a;
              }
            ];
      }
    );
in
[
  (g 1 chain.q)
  (g 2 chain.q)
  (g 3 [
    chain.a.c
    chain.a.a.h
  ])
  (g 4 (depth chain))
  (g 5 (builtins.attrNames chain.a.a.a))
]
