# A chain of 20 000 nested sets, the link first in each, kept and moved:
# walking it to its end defers what its bounded worklists cannot hold.
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
  chain = builtins.foldl' (acc: i: {
    a = acc;
    z = i;
  }) null (builtins.genList (i: i) 20000);
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
  (g 1 chain.z)
  (g 2 chain.z)
  (g 3 chain.a.a.z)
  (g 4 (depth chain))
]
