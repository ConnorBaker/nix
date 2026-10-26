# Re-forces nested 40 deep, each 9000 calls: v's elements are forced in
# order by the first element, each finding the one before forced. All
# reverted, the second element's force of the last re-forces the one
# before at the bottom of its 9000 calls, and so on, each at its own call
# depth, but all on the native stack at once: re-forces that start deeper
# than their first force run on fresh stacks.
let
  k = 40;
  d = 9000;
  deepThen = n: x: if n == 0 then x else deepThen (n - 1) x;
  v = builtins.genList (
    i:
    if i == 0 then
      [ 0 ]
    else
      let
        r = 1 + deepThen d (builtins.head (builtins.elemAt v (i - 1)));
      in
      builtins.seq r [ r ]
  ) (k + 1);
in
builtins.seq v [
  (builtins.seq (import ./lib/junk.nix 100) (builtins.foldl' (a: b: builtins.seq b a) 0 v))
  (builtins.elemAt v k)
]
