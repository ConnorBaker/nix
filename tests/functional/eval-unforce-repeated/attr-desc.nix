# A set shared by two top-level attributes whose Bindings order (zz first) is the reverse of the printed order.
let s = { x = 1; }; in { zz = s; yy = s; }
