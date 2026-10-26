# A producer forced after its consumer: in the third element r is forced
# (r.a stays a thunk), then P.q, then r.a, which reads P.q's value.
let
  P = {
    q = {
      x = {
        f = x: x;
      };
    };
  };
  r = {
    a = P.q.x;
  };
in
[
  (P ? q)
  (builtins.seq r (P ? q))
  (builtins.seq r (builtins.seq P.q (builtins.seq r.a true)))
  (r.a == P.q.x)
]
