# xs, reverted with the first element's job, is forced again in the
# second's, which keeps it (a cell that an earlier job reverted is
# promoted): all that job allocated is xs's, so nothing moves.
let
  xs = builtins.genList (i: i) 100000;
in
[
  (builtins.length xs)
  (builtins.length xs)
  (builtins.length xs)
]
