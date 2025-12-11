# Test for cached thunk problem
# This imports the same file 10 times via genList, which triggers caching
let
  # Each iteration imports the same file - the first import caches it
  getImport = n:
    let
      result = import ./simple-test.nix;
    in
    builtins.deepSeq result result;

  # Generate 10 imports of the same file
  imports = builtins.genList (n:
    builtins.trace "Processing import ${builtins.toString n}" (getImport n)
  ) 10;
in
# Force evaluation of all imports
builtins.deepSeq imports imports
