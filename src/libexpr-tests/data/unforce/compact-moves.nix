# Every kind of object moves down over the garbage a job built first:
# pool is kept by the first job that forces it (it holds a failure, which
# is never recomputed), pool2 by the second. Strings with contexts, paths,
# sets and their layers, lists, closures and a failure are among them.
let
  garbage =
    k:
    builtins.length (
      builtins.genList (i: [
        i
        k
      ]) 5000
    );
  drv = derivation {
    name = "moves";
    system = "x";
    builder = "/b";
  };
  mkPool = n: {
    sets = builtins.genList (i: {
      v = i;
      s = "x${toString (i + n)}";
      l = [
        i
        (i + 1)
        (i + 2)
      ];
    }) 300;
    strings = builtins.genList (i: "${drv}-${toString (i + n)}") 50;
    paths = builtins.genList (i: ./. + "/p${toString (i + n)}") 20;
    closures = builtins.genList (i: { f = x: x + i + n; }) 50;
    layered = {
      a = n;
    }
    // {
      b = [
        2
        3
        4
      ];
    }
    // builtins.listToAttrs [
      {
        name = "c";
        value = "d";
      }
    ];
  };
  pool = mkPool 0 // {
    failing = {
      bad = throw "no";
      ok = 1;
    };
  };
  pool2 = mkPool 1;
  show =
    v:
    builtins.toJSON {
      inherit (v) sets strings layered;
      paths = map toString v.paths;
      closures = map (c: c.f 1) v.closures;
      failing = if v ? failing then (builtins.tryEval v.failing.bad).success else null;
    };
in
[
  (builtins.seq (garbage 1) (show pool))
  (show pool2)
  (builtins.seq (garbage 2) (show pool2))
  (show pool)
  (show pool2)
]
