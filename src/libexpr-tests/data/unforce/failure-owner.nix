# A young failure (p.a) in a job whose owner segments are many (forces of
# young cells that allocate): p.a's owner must be found among them. e2
# raises the failure again, whose message shows x while x.a was a thunk.
let
  olds = builtins.genList (i: { v = i; }) 1000;
  x = {
    a = 1 + 1;
  };
  p = {
    a =
      assert x == 5;
      0;
  };
in
{
  e0 = builtins.seq (builtins.foldl' (acc: o: acc + o.v) 0 olds) (builtins.tryEval p.a).success;
  e1 = x;
  e2 = p.a;
}
