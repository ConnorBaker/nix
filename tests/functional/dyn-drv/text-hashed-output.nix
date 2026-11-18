with import ./config.nix;

# A simple content-addressing derivation.
# The derivation can be arbitrarily modified by passing a different `seed`,
# but the output will always be the same
rec {
  hello = mkDerivation {
    name = "hello";
    buildCommand = ''
      set -x
      echo "Building a CA derivation"
      mkdir -p $out
      echo "Hello World" > $out/hello
    '';
  };

  producingDrv = mkDerivation {
    name = "hello.drv";
    buildCommand = ''
      echo "Copying the derivation"
      cp ${builtins.unsafeDiscardOutputDependency hello.drvPath} $out
    '';
    __contentAddressed = true;
    outputHashMode = "text";
    outputHashAlgo = "sha256";
  };

  # Input-addressed, so the paths of their outputs are known before they
  # are built. The output of `producingProducingDrvInputAddressed` is the
  # derivation of `producingDrvInputAddressed`, whose output is the
  # derivation of `hello`.
  producingDrvInputAddressed = mkDerivation {
    name = "hello.drv";
    buildCommand = ''
      cp ${builtins.unsafeDiscardOutputDependency hello.drvPath} $out
    '';
    outputHashMode = "text";
  };

  producingProducingDrvInputAddressed = mkDerivation {
    name = "hello.drv.drv";
    buildCommand = ''
      cp ${builtins.unsafeDiscardOutputDependency producingDrvInputAddressed.drvPath} $out
    '';
    outputHashMode = "text";
  };

  wrapper = mkDerivation {
    name = "use-dynamic-drv-in-non-dynamic-drv";
    buildCommand = ''
      echo "Copying the output of the dynamic derivation"
      cp -r ${builtins.outputOf producingDrv.outPath "out"} $out
    '';
  };

  failingWrapper = mkDerivation {
    name = "use-dynamic-drv-in-non-dynamic-drv-wrong";
    buildCommand = ''
      echo "Fail at copying the output of the dynamic derivation"
      fail ${builtins.outputOf producingDrv.outPath "out"} $out
    '';
  };
}
