# Without the check, the context would name output `a` of output `b`.
builtins.addDrvOutputDependencies (
  builtins.appendContext "" {
    ${
      builtins.unsafeDiscardStringContext
        (derivation {
          name = "x";
          builder = "/bin/sh";
          system = "x86_64-linux";
        }).drvPath
    } =
      {
        outputs = [ "a!b" ];
      };
  }
)
