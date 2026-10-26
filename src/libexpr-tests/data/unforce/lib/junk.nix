# Garbage that an element's job builds and drops.
n:
builtins.foldl' (acc: e: acc + e.i + builtins.stringLength e.s) 0 (
  builtins.genList (i: {
    inherit i;
    s = toString i;
  }) n
)
