with import ./config.nix;

# Floating content-addressed derivations using the Git tree-hashing
# ingestion method (`outputHashMode = "git"`), as opposed to the
# `recursive` (NAR) method exercised by content-addressed.nix.
#
# "Floating" = the output hash is NOT declared up front; it is computed
# from whatever the build produces (requires `ca-derivations`). Combined
# with the `git` method (requires `git-hashing`), the output's store-path
# identity is the Git tree OID of the built contents.
let
  mkGitCADerivation =
    args:
    mkDerivation (
      {
        __contentAddressed = true;
        outputHashMode = "git";
        outputHashAlgo = "sha1";
      }
      // args
    );
in

{
  seed ? 0,
}:

rec {
  # A single-file (blob-rooted) output.
  rootFile = mkGitCADerivation {
    name = "floating-git-file";
    buildCommand = ''
      echo "seed: ${toString seed}" > /dev/null # only perturbs the drv, not the output
      printf hello-floating-git > $out
    '';
  };

  # A directory-tree output — the case that matters for source trees:
  # the store path's identity is the Git *tree* OID, recursively
  # composed from the per-file blob OIDs.
  rootTree = mkGitCADerivation {
    name = "floating-git-tree";
    buildCommand = ''
      echo "seed: ${toString seed}" > /dev/null
      mkdir -p $out/sub
      printf hello > $out/a
      printf world > $out/sub/b
    '';
  };

  # A second derivation whose *output content* is byte-identical to
  # rootTree's, reached by a different build. Under content addressing
  # by the Git tree OID, both must land on the SAME store path.
  rootTreeTwin = mkGitCADerivation {
    name = "floating-git-tree"; # same name + same contents => same CA path
    buildCommand = ''
      echo "twin ${toString seed}" > /dev/null
      mkdir -p $out/sub
      # Built in a different order / via different commands, same result.
      printf world > $out/sub/b
      printf hello > $out/a
    '';
  };
}
