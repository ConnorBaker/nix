# A value every other element needs, forced in the first element's job.
let
  shared = builtins.foldl' (a: b: a + b) 0 (builtins.genList (i: i * 3) 20000);
in
builtins.genList (e: if e / 2 * 2 == e then shared + e else e) 8
