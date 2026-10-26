# What a job keeps reaches its young cells only through a partly applied
# primop's operand (`builtins.add (i * 2)`), a curried function's Env, or
# a two-element list's second pointer; it moves down over garbage, and
# later elements apply what moved.
let
  garbage =
    k:
    builtins.length (
      builtins.genList (i: [
        i
        k
      ]) 2000
    );
  g = k: v: builtins.seq (garbage k) v;
  adders = builtins.genList (i: builtins.add (i * 2)) 50;
  concs = builtins.genList (i: builtins.concatStringsSep "-${toString i}-") 20;
  curried = builtins.genList (
    i:
    (
      a: b: c:
      a + b + c + i
    )
      (i * 3)
  ) 30;
  pairs = builtins.genList (i: [
    (i * 7)
    "p${toString i}"
  ]) 40;
  apply = l: map (f: f 1) l;
in
[
  (g 1 (builtins.length adders))
  (g 2 (apply adders))
  (g 3 (
    map (
      f:
      f [
        "a"
        "b"
      ]
    ) concs
  ))
  (g 4 (map (f: f 1 2) curried))
  (g 5 (apply adders))
  (g 6 pairs)
  (g 7 (map (p: builtins.elemAt p 1) pairs))
  (g 8 (
    map (
      f:
      f [
        "c"
        "d"
      ]
    ) concs
  ))
  (g 9 (map (f: f 4 5) curried))
]
