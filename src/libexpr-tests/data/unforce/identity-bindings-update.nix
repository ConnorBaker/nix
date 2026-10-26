# As identity-bindings.nix, through `//`.
let
  Q = {
    d = {
      a = 1;
    };
  };
  L = Q // {
    y = Q.d;
  };
  R = L.y;
in
[
  (builtins.seq L true)
  (builtins.seq R true)
  {
    x = R;
    y = Q.d;
  }
]
