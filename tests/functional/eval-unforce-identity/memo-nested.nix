# The closure's memo (unforce.cc, `Closure`) must not let a target nested in
# another use the outer one's memo too early. Job 2 forces W, which forces U
# inside it; W's content is U's Bindings, allocated inside U's interval, so
# W's walk prunes it as its own child's and leaves a memo that holds only
# from W's end on. U starts before W ends: its walk must still enter that
# Bindings and reach d's value, which S0 (promoted in job 2) keeps a copy of,
# so U is promoted too.
let
  U = { d = { f = x: x; }; };
  W = builtins.seq U U;
  S0 = { y = U.d; };
in [ (builtins.seq S0 true) (builtins.seq W (builtins.seq S0.y true)) (S0.y == U.d) ]
