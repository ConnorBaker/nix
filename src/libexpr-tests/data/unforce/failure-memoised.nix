# A failure stays as it first was: t fails in p's job, under `tryEval`,
# with a message that shows x while x.a is a thunk. q then forces x.a, and
# r raises t's failure again, with that message.
let
  x = {
    a = 1 + 1;
  };
  t =
    assert x == 5;
    0;
in
{
  p = builtins.seq (import ./lib/junk.nix 100) (builtins.tryEval t).success;
  q = x;
  r = t;
}
