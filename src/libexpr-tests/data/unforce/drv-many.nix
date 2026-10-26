# Many small jobs: each drops a little garbage (more every seventh),
# forces a shared pool of derivations, and reaches members that earlier
# elements reached, directly or inside a `recurseForDerivations` set, next
# to copies of them (`// { }` is the member itself, `// { t = 1; }` a copy).
let
  d = import ./lib/d.nix;
  junk = import ./lib/junk.nix;
  pool = builtins.listToAttrs (
    builtins.genList (i: {
      name = "p${toString i}";
      value = d "p${toString (i / 3)}";
    }) 60
  );
in
builtins.listToAttrs (
  builtins.genList (i: {
    name = "e${toString i}";
    value = builtins.seq (junk (if i / 7 * 7 == i then 3000 else 10)) (
      if i / 2 * 2 == i then
        pool."p${toString (i / 2)}"
      else
        {
          recurseForDerivations = true;
          a = pool."p${toString (i / 3)}";
          b = pool."p${toString i}" // { };
          c = pool."p${toString (i / 4)}" // {
            t = 1;
          };
        }
    );
  }) 60
)
