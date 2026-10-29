# p, reverted with e0's job, is promoted in e1's, where d's force builds
# y's set before p's force ends; the printer holds that set, so e2 prints
# it as «repeated».
let
  p = builtins.genList (i: i) 3;
  shared = {
    d =
      let
        y = {
          a = 1;
        };
      in
      builtins.seq y (builtins.seq p y);
  };
in
{
  e0 = builtins.length p;
  e1 = shared;
  e2 = shared.d;
}
