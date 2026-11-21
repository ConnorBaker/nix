{ name }:

with import ./config.nix;

# A derivation with an input of its own, whose contents are its own.
mkDerivation {
  name = "substitutes-${name}";
  builder = builtins.toFile "substitutes-${name}.sh" "echo ${name} > $out";
  input = builtins.toFile "substitutes-${name}-f" name;
}
