{
  busybox,
  inputSalt,
  salt,
}:

with import ./config.nix;

let
  input = builtins.toFile "build-remote-upload-lock-input-${inputSalt}" inputSalt;
in

derivation {
  inherit system salt;
  name = "build-remote-upload-lock";
  builder = busybox;
  args = [
    "sh"
    "-c"
    "read x < ${input}; echo $x $salt > $out"
  ];
}
// {
  inherit input;
}
