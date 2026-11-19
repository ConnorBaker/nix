with import ./config.nix;

# Dynamic derivations whose .drv files are produced by text-hashed CA
# derivations from the bytes of an instantiated derivation, without a
# reference to it, so the produced .drv can be garbage collected while the
# realisation naming it stays.
rec {
  helloScript = builtins.toFile "gc-built-drv-hello.sh" ''
    mkdir $out
    echo hello > $out/hello
  '';
  catScript = builtins.toFile "gc-built-drv-cat.sh" ''
    cp $src $out
  '';

  hello = derivation {
    name = "gc-hello";
    inherit system;
    builder = shell;
    args = [
      "-e"
      helloScript
    ];
    PATH = path;
  };

  textDrv =
    name: target: refs:
    derivation {
      inherit name system refs;
      builder = shell;
      args = [
        "-e"
        catScript
      ];
      PATH = path;
      # A file, not an environment variable: placeholders in the latter are rewritten.
      src = builtins.toFile "${name}-text" (
        builtins.unsafeDiscardStringContext (builtins.readFile target.drvPath)
      );
      __contentAddressed = true;
      outputHashMode = "text";
      outputHashAlgo = "sha256";
    };

  # producer^out = hello.drv
  producer = textDrv "gc-hello.drv" hello [ helloScript ];
  # meta^out = producer.drv
  meta = textDrv "gc-hello.drv.drv" producer [
    catScript
    helloScript
    producer.src
  ];

  # A CA derivation with an input that is an output of a dynamic derivation
  consumer = derivation {
    name = "gc-consumer";
    inherit system;
    builder = shell;
    args = [
      "-c"
      "read x < ${builtins.outputOf producer.outPath "out"}/hello; echo $x > $out"
    ];
    PATH = path;
    __contentAddressed = true;
    outputHashMode = "recursive";
    outputHashAlgo = "sha256";
  };
}
