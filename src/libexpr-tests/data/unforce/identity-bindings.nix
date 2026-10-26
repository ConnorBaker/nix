# The identity of attribute sets, which printers show as «repeated»: R
# and Q.d are one set, reached through `listToAttrs` in one element's job
# and directly in a later one.
let
  Q = {
    d = {
      a = 1;
    };
  };
  L = builtins.listToAttrs [
    {
      name = "y";
      value = Q.d;
    }
  ];
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
