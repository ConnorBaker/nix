# A dependency's derivation fails while a dependent's drvPath is computed:
# b's drvPath fails in c's builder, with the frames of computing it.
let
  dd =
    name: extra:
    derivation (
      {
        inherit name;
        system = "x86_64-linux";
        builder = "/bin/sh";
      }
      // extra
    );
in
rec {
  a = dd "a" { x = builtins.warn "W-a" 1; };
  b = dd "b" {
    dep = a;
    dep2 = c;
  };
  c = dd "c" { builder = throw "c-builder fails"; };
}
