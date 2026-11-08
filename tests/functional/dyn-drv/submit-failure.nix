with import ./config.nix;

let
  buildSubmitting =
    name: command:
    mkDerivation {
      inherit name;

      requiredSystemFeatures = [ "builder-rpc-v0" ];

      buildCommand = ''
        set -e
        set -u

        PATH=${builtins.getEnv "NIX_BIN_DIR"}:$PATH
        export NIX_CONFIG='extra-experimental-features = nix-command ca-derivations dynamic-derivations'

        ${command}
      '';

      __contentAddressed = true;
      outputHashMode = "nar";
      outputHashAlgo = "sha256";

    };
  # Submits `path` as the output of a flat fixed-output derivation of "hello\n".
  submitFixed =
    name: path:
    derivation (
      (buildSubmitting name "nix store submit-output ${path} out").drvAttrs
      // {
        __contentAddressed = false;
        outputHashMode = "flat";
        outputHash = builtins.hashString "sha256" "hello\n";
      }
    );
  inputAddressed = mkDerivation {
    name = "input-addressed";
    buildCommand = "echo hello > $out";
    __contentAddressed = false;
  };
in
{
  duplicate = buildSubmitting "duplicate" ''
    mkdir a
    echo "miao" > a/gatto
    a="$(nix store add -n duplicate ./a)"
    nix store submit-output "$a" out

    mkdir b
    echo "miau" > b/katze
    b="$(nix store add -n duplicate ./b)"
    nix store submit-output "$b" out
  '';
  notAdded = buildSubmitting "not-added" ''
    echo kot > a
    nix store submit-output "$(nix store add --dry-run -n not-added ./a)" out
  '';
  fixedInputAddressed = submitFixed "input-addressed" inputAddressed;
  fixedText = submitFixed "fixed-text" (builtins.toFile "fixed-text" "hello\n");
  noSubmit = buildSubmitting "no-submit" ''
    mkdir a
    echo "nyaa" > a/neko
    a="$(nix store add -n no-submit ./a)"
    # Don't run the required `nix store submit-output`
  '';
}
