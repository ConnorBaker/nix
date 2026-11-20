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

  # Its output is `producingDrv`'s derivation, so it is two dynamic
  # layers above `hello`.
  producingProducingDrv = mkDerivation {
    name = "hello.drv.drv";
    buildCommand = ''
      echo "Copying the derivation-producing derivation"
      cp ${builtins.unsafeDiscardOutputDependency producingDrv.drvPath} $out
    '';
    __contentAddressed = true;
    outputHashMode = "text";
    outputHashAlgo = "sha256";
  };

  # Input-addressed, so its output is the derivation of `producingDrv`
  # at a path other than that derivation's own.
  producingDrvAtForeignPath = mkDerivation {
    name = "hello.drv.drv";
    buildCommand = ''
      cp ${builtins.unsafeDiscardOutputDependency producingDrv.drvPath} $out
    '';
    outputHashMode = "text";
  };

  # Input-addressed, with an input derivation.
  helloCopy = mkDerivation {
    name = "hello-copy";
    buildCommand = "cp -r ${hello} $out";
  };

  # Outputs the derivation of the input-addressed `helloCopy`, at a path
  # other than that derivation's own.
  producingIaDrvAtForeignPath = mkDerivation {
    name = "hello-copy.drv";
    buildCommand = ''
      cp ${builtins.unsafeDiscardOutputDependency helloCopy.drvPath} $out
    '';
    outputHashMode = "text";
  };

  helloFromSource = mkDerivation {
    name = "hello-from-source";
    src = builtins.toFile "hello-source" "Hello World";
    buildCommand = "cp $src $out";
    __contentAddressed = true;
    outputHashMode = "recursive";
    outputHashAlgo = "sha256";
  };

  # Outputs the derivation of `helloFromSource` at a foreign path,
  # without depending on it, so the output does not reference that
  # derivation's inputs.
  producingDrvWithUnreferencedInputs = mkDerivation {
    name = "hello-from-source.drv";
    buildCommand = ''
      cp ${builtins.toFile "hello-from-source-text" (builtins.unsafeDiscardStringContext (builtins.readFile (builtins.unsafeDiscardOutputDependency helloFromSource.drvPath)))} $out
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
