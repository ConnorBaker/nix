{
  name,
  inputs ? [ ],
}:

with import ./config.nix;

mkDerivation {
  name = "upload-lock-${name}";
  # Inputs of its own, each with contents of its own.
  builder = builtins.toFile "upload-lock-${name}.sh" "echo ${name} > $out";
  inputs = map (input: builtins.toFile "upload-lock-${input}" input) inputs;
}
