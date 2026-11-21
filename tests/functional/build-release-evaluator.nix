# The file's value, which the evaluator caches, keeps `retained` reachable
# for as long as the evaluator lives. `drv` waits for
# build-release-evaluator.sh, so `nix build` sits in its build wait until the
# test has measured it.
with import ./config.nix;
let
  size = builtins.fromJSON (builtins.getEnv "RELEASE_SIZE");
  dir = builtins.getEnv "RELEASE_DIR";
  retained = builtins.genList (i: "retained string ${toString i}") size;
in
{
  inherit retained;
  drv = mkDerivation {
    name = "build-release-evaluator-${toString size}";
    # Force every string. A strict fold, unlike `deepSeq`, keeps no set of
    # visited values in C++ memory, so the measurement sees only the
    # evaluator's heap.
    retainedLength = builtins.foldl' (n: s: n + builtins.stringLength s) 0 retained;
    buildCommand = ''
      touch ${dir}/started
      while ! test -e ${dir}/go; do sleep 0.1; done
      echo done > $out
    '';
  };
}
