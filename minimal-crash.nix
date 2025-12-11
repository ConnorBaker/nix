let
  getTest = n:
    let
      result = builtins.unsafeDiscardStringContext "test-${builtins.toString n}";
    in
    builtins.deepSeq result result;
  
  tests = builtins.genList getTest 10;
in
tests
