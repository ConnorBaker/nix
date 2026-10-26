# A young cell that fails within its owner's force: q's force evaluates r.b,
# whose `tryEval` catches r.a's failure, with a message that shows x while x.a
# is a thunk. q keeps its value, failed cell and all: re-forced, it would fail
# again after the second element forced x.a, and the third would raise a
# failure that shows x.a's value.
let
  x = {
    a = 1 + 1;
  };
  q =
    let
      r = {
        a =
          assert x == 5;
          0;
        b = (builtins.tryEval r.a).success;
      };
    in
    builtins.seq r.b r;
in
[
  (builtins.seq (import ./lib/junk.nix 100) q.b)
  x
  q.a
]
