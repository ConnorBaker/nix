# Derivations found in jobs that allocate alike: each job drops garbage,
# its derivation's set moves down, and the next job's set is allocated
# where the one before it was. Each must still be found as itself.
let
  d = import ./lib/d.nix;
  junk = import ./lib/junk.nix;
  mk = name: builtins.seq (junk 100) (d name);
in
{
  a1 = mk "a1";
  a2 = mk "a2";
  a3 = mk "a3";
  a4 = mk "a4";
  a5 = mk "a5";
  a6 = mk "a6";
}
