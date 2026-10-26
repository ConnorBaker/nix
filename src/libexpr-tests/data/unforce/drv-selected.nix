# b and e select one set, built inside a (which is not searched): found
# once.
let
  d = import ./lib/d.nix;
  junk = import ./lib/junk.nix;
in
rec {
  a = builtins.seq (junk 100) {
    c = d "shared" // {
      f = x: x;
    };
  };
  b = a.c;
  e = a.c;
}
