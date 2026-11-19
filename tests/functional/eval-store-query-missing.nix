# `depth` levels of `width` derivations, each depending on every
# derivation of the level below, and one derivation on top of them.
{
  depth ? 4,
  width ? 10,
}:

with import ./config.nix;

let
  mk =
    name: attrs:
    mkDerivation (
      {
        name = "eval-store-query-missing-${name}";
        buildCommand = "echo ${name} > $out";
      }
      // attrs
    );
  mkLevel =
    n: below:
    builtins.genList (
      i: mk "${toString n}-${toString i}" { buildCommand = "echo ${toString below} > $out"; }
    ) width;
  levels = builtins.foldl' (
    acc: n: acc ++ [ (mkLevel n (if n == 0 then [ ] else builtins.elemAt acc (n - 1))) ]
  ) [ ] (builtins.genList (n: n) depth);
in
{
  # Only the evaluation store has it; only a substituter has its output.
  lone = mk "lone" { };

  # Floating content-addressed: only a realisation gives its output's path.
  unbuilt = mk "unbuilt" {
    __contentAddressed = true;
    outputHashMode = "recursive";
    outputHashAlgo = "sha256";
  };

  top = mk "top" { buildCommand = "echo ${toString (builtins.elemAt levels (depth - 1))} > $out"; };
}
