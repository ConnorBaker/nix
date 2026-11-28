with import ./config.nix;

# A fixed-output derivation whose builder adds its own output to the store
# through the build's store socket, `par` times at once. `add` picks the client
# operation.
{
  feature,
  add,
  files,
  par,
  hash,
}:

mkDerivation {
  name = "own-output";
  requiredSystemFeatures = [ feature ];
  buildCommand = ''
    PATH=${builtins.getEnv "NIX_BIN_DIR"}:$PATH
    export NIX_CONFIG='extra-experimental-features = nix-command ca-derivations dynamic-derivations recursive-nix'
    mkdir own-output
    printf %s "${feature} ${add}" > own-output/salt
    for ((i = 0; i < ${toString files}; i++)); do echo $i > own-output/$i; done
    pids=()
    for ((i = 0; i < ${toString par}; i++)); do
      ${
        {
          nar = "nix store add";
          scan = "nix store add --scan";
          dump = "nix-store --add";
        }
        .${add}
      } own-output > p$i &
      pids+=($!)
    done
    for pid in "''${pids[@]}"; do wait $pid; done
    ${
      if feature == "builder-rpc-v0" then
        ''nix store submit-output "$(< p0)" out''
      else
        ''[[ $(< p0) == "$out" ]]''
    }
  '';
  outputHashMode = "nar";
  outputHashAlgo = "sha256";
  outputHash = hash;
}
