# Cells forced in jobs to objects older than the jobs keep those objects:
# functions compare equal by identity, and a set printed in one element's
# job is «repeated» in a later one.
let
  o = {
    f = a: a;
    l = [
      1
      2
      3
    ];
    s = "x";
  };
  pick = n: if n < 100 then o else null;
  xs = builtins.genList pick 6;
in
builtins.seq o [
  (builtins.elemAt xs 0)
  ((builtins.elemAt xs 1).f == o.f)
  (builtins.elemAt xs 2)
  ((builtins.elemAt xs 3) == (builtins.elemAt xs 4))
  (builtins.elemAt xs 5).l
]
