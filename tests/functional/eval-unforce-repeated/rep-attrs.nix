# The same as rep.nix, in an attribute set.
let x = { a = 1; }; in { p = x; q = x; }
