{
  salt,
  go ? null,
}:

with import ./config.nix;

let
  # Given the fifo `go`, each of these builds says "started", then reads a line from it.
  gate = if go == null then "" else "echo started; read -r _ < ${go}; ";
  mk =
    name: attrs:
    mkDerivation (
      {
        inherit name salt;
        buildCommand = "${gate}echo ${name} > $out";
      }
      // attrs
    );
in

{
  a = mk "a" { };
  b = mk "b" { };
  c = mk "c" { };
}
