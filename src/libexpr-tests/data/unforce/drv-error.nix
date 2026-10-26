# a's derivation fails once its drvPath is forced, after the search: the
# error has the frames of computing the drvPath, and none of the search.
{
  a = derivation {
    name = "a";
    system = "x86_64-linux";
    builder = throw "a-builder fails";
  };
  b = import ./lib/d.nix "b";
}
