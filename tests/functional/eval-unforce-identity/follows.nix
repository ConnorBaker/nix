# A producer forced after the consumer completed. In the third element r is
# forced first (its attribute a stays a thunk), then P.q (an unforced copy),
# then r.a, which reads P.q's result. r's data is rebuilt when P.q is
# re-forced, although P.q's force neither contains nor precedes r's.
let
  P = { q = { x = { f = x: x; }; }; };
  r = { a = P.q.x; };
in
[
  (P ? q)
  (builtins.seq r (P ? q))
  (builtins.seq r (builtins.seq P.q (builtins.seq r.a true)))
  (r.a == P.q.x)
]
