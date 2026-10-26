# Shared values one level down, in attributes forced in reverse order.
let
  s = {
    x = 1;
  };
  l = [
    1
    2
  ];
in
{
  zz = {
    inner = s;
    il = l;
  };
  yy = {
    other = s;
    ol = l;
  };
}
