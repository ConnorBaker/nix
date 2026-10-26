# Sharing within an attribute and across attributes, out of order.
let
  s = {
    x = 1;
  };
  t = {
    y = s;
    z = s;
  };
in
{
  b = t;
  a = {
    q = s;
  };
  c = s;
}
