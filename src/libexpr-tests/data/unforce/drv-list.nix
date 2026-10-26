# A list: an element repeated is found once (also through `// { }`, which
# returns it), a copy twice, and equal calls twice.
let
  d = import ./lib/d.nix;
  junk = import ./lib/junk.nix;
  x = d "list-x";
in
[
  (builtins.seq (junk 100) x)
  x
  (x // { })
  (x // { tag = 1; })
  (d "list-y")
  (d "list-y")
]
