# As compact-fwdref.nix, the next job's first forward reference ~80 KiB
# past what the job before kept, in whole pages given back to the system.
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
  far = f: k: builtins.seq (garbage 2000) (f k);
  round = p: k: [
    (builtins.length p)
    (builtins.length p + garbage (4000 + k))
    (far fwdLet k)
    (far fwdRec k)
    (far fwdDef k)
  ];
in
builtins.concatLists [
  (round (mk 3) 1)
  (round (mk 7) 2)
  (round (mk 11) 3)
  (round (mk 17) 4)
]
