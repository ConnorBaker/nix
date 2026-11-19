with import ./config.nix;

{
  # Only `out` is in the binary cache.
  partial = mkDerivation {
    name = "query-missing-partial";
    outputs = [
      "out"
      "dev"
    ];
    buildCommand = "echo out > $out; echo dev > $dev";
  };

  valid = mkDerivation {
    name = "query-missing-valid";
    buildCommand = "echo valid > $out";
  };
}
