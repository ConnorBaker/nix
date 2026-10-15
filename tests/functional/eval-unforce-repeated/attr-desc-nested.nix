# The same with the shared values nested one level down.
let s = { x = 1; }; l = [ 1 2 ]; in { zz = { inner = s; il = l; }; yy = { other = s; ol = l; }; }
