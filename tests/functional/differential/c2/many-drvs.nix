# Enough derivations that any serialisation of the list exceeds the stdio
# buffer of a pipe, so the first bytes leave the process before it ends.
builtins.genList (
  i:
  derivation {
    name = "c2-probe-${toString i}";
    system = "x86_64-linux";
    builder = "/bin/sh";
    args = [
      "-c"
      "echo ${toString i} > $out"
    ];
    padding = builtins.concatStringsSep "-" (builtins.genList toString 40);
  }
) 300
