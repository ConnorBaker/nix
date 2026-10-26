# A young cell's failure: p.a, built by p's force in the first element's
# job, fails there, under `tryEval`; the second element raises it again,
# with the frames it first had (`while evaluating the attribute 'a'`).
let
  p = {
    a = throw "boom";
    b = 1;
  };
in
[
  (builtins.seq (import ./lib/junk.nix 100) (builtins.tryEval p.a).success)
  p.a
]
