# Epoch counterexample candidate at K=2 (default K), m=0 (proposed shipping default), batch size 0.
# Job 1 forces L (count 1). Job 2 forces R = L.y: L reaches K=2 and is promoted, but L's young attribute y was
# forced after L's force ended, by R, and now holds Q's young result. Q (count 1) is reverted; job 3 re-forces Q.
let
  Q = { d = { f = x: x; }; };
  L = builtins.listToAttrs [ { name = "y"; value = Q.d; } ];
  R = L.y;
in [
  (builtins.seq L true)
  (builtins.seq R true)
  (R == Q.d)
  (builtins.elem R [ Q.d ])
]
