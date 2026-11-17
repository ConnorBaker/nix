{ random }:
with import ./config.nix;
mkDerivation {
  name = "check-submitted-${if random then "random" else "fixed"}";
  requiredSystemFeatures = [ "builder-rpc-v0" ];
  buildCommand = ''
    set -eu
    PATH=${builtins.getEnv "NIX_BIN_DIR"}:$PATH
    export NIX_CONFIG='extra-experimental-features = nix-command ca-derivations dynamic-derivations'
    mkdir r
    ${if random then "cat /proc/sys/kernel/random/uuid" else "echo fixed"} > r/x
    o="$(nix store add -n "$name" ./r)"
    nix store submit-output "$o" out
  '';
  __contentAddressed = true;
  outputHashMode = "nar";
  outputHashAlgo = "sha256";
}
