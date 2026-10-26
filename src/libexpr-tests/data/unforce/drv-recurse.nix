# A `recurseForDerivations` set reached again through an alias, through
# another such set, and its members through selections: each found once.
# w.p is another set with s.p's drvPath: found again.
let
  d = import ./lib/d.nix;
  junk = import ./lib/junk.nix;
in
rec {
  s = builtins.seq (junk 100) {
    recurseForDerivations = true;
    p = d "rec-p";
    q = d "rec-q";
  };
  t = s;
  u = s.p;
  v = {
    recurseForDerivations = true;
    inner = s;
    q = s.q;
  };
  w = {
    recurseForDerivations = true;
    p = s.p // {
      tag = 1;
    };
  };
}
