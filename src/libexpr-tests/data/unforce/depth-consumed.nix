# A cell forced by a consumer, after the force that built it ended, keeps its
# value: s's force builds s.y, and element 0 forces it, ~9000 calls deep.
# Reverting s would make its re-force build s.y again unforced, and element
# 1 would force it under 2000 calls, past max-call-depth, where Nix without
# unforcing reads its value.
let
  g = n: if n == 0 then 0 else 1 + g (n - 1);
  s = {
    y = g 9000;
  };
  deep = n: if n == 0 then builtins.seq s.y [ ] else deep (n - 1);
in
[
  (builtins.seq s.y [ ])
  (deep 2000)
]
