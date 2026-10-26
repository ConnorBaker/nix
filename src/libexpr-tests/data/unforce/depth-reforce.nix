# A re-force runs at the call depth of its first force: x (~9000 calls
# deep) is forced at the top by a, and reached by b under 5000 calls.
# Reverted with a's job, x's re-force in b must not need 5000 + 9000 calls.
# (x's value is a list its force builds, so that a's job reverts it.)
let
  deep = n: if n == 0 then 0 else 1 + deep (n - 1);
  x =
    let
      r = deep 9000;
    in
    builtins.seq r [ r ];
  wrap = n: v: if n == 0 then v else wrap (n - 1) v;
in
{
  a = builtins.seq x (import ./lib/junk.nix 100);
  b = wrap 5000 x;
}
