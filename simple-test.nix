let
  f = n: builtins.trace "processing ${builtins.toString n}" n;
  result = builtins.genList f 3;
in
result
