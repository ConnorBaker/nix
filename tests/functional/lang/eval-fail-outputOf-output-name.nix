# Without the check, the context would name output `a` of output `b`.
builtins.addDrvOutputDependencies (
  builtins.outputOf
    (derivation {
      name = "x";
      builder = "/bin/sh";
      system = "x86_64-linux";
    }).outPath
    "a!b"
)
