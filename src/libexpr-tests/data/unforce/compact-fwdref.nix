# Memory a job freed, reused by the next: an Env slot is read before it is
# written when a binding refers to a later one of the same `let`, `rec`
# set or formals, and must read null. Each round: a pool list forced by
# two elements (kept at the second's end, which also builds garbage below
# it, so that what it keeps moves down), then elements whose first Env has
# such a forward reference.
let
  mk =
    n:
    builtins.genList (i: {
      v = i;
      s = "x${toString i}";
    }) n;
  garbage =
    n:
    builtins.length (
      builtins.genList (i: [
        i
        i
        i
      ]) n
    );
  fwdLet =
    k:
    let
      zfa = zfb;
      zfb = k;
    in
    zfa;
  fwdRec =
    k:
    (rec {
      zra = zrb;
      zrb = k;
    }).zra;
  fwdDef =
    k:
    (
      {
        zda ? zdb,
        zdb ? k,
      }:
      zda
    )
      { };
  fwdLet3 =
    k:
    let
      zga = zgb;
      zgb = zgc;
      zgc = k;
    in
    zga + zgb;
  round = p: k: [
    (builtins.length p)
    (builtins.length p + garbage (60 + k))
    (fwdLet k)
    (fwdRec k)
    (fwdDef k)
    (fwdLet3 k)
  ];
in
builtins.concatLists [
  (round (mk 3) 1)
  (round (mk 7) 2)
  (round (mk 11) 3)
  (round (mk 17) 4)
  (round (mk 23) 5)
  (round (mk 31) 6)
  (round (mk 41) 7)
  (round (mk 53) 8)
]
