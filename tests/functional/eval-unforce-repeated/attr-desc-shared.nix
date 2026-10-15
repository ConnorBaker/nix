# Sharing within and across top-level attributes, out of order.
let s = { x = 1; }; t = { y = s; z = s; }; in { b = t; a = { q = s; }; c = s; }
