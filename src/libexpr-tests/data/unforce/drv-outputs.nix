# Each output is a set of its own: one derivation per output reached, and
# an alias of an output once.
let
  junk = import ./lib/junk.nix;
  m = derivation {
    name = "multi";
    system = "x86_64-linux";
    builder = "/bin/sh";
    outputs = [
      "out"
      "dev"
    ];
  };
in
rec {
  x = builtins.seq (junk 100) m;
  y = m.dev;
  z = y;
  zz = m.out;
}
