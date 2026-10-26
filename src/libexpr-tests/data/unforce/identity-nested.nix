# A force nested in another: W forces U, so W's value is U's set, built
# while W was being forced. S0 keeps U.d; whatever is kept for W must keep
# U, whose force is inside W's.
let
  U = {
    d = {
      f = x: x;
    };
  };
  W = builtins.seq U U;
  S0 = {
    y = U.d;
  };
in
[
  (builtins.seq S0 true)
  (builtins.seq W (builtins.seq S0.y true))
  (S0.y == U.d)
]
