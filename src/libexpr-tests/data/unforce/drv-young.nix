# A `recurseForDerivations` set built in a job that drops garbage: its
# derivations' cells are in the job's memory, and only the set of
# derivations found holds them. Reached again in later jobs through a
# selection, an alias and another such set: found once each.
let
  d = import ./lib/d.nix;
  junk = import ./lib/junk.nix;
in
rec {
  s = builtins.seq (junk 1000) {
    recurseForDerivations = true;
    p = d "p";
    q = d "q";
  };
  t = s.p;
  u = s;
  v = {
    recurseForDerivations = true;
    w = s.q;
  };
}
