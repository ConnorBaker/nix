# As identity-select.nix, through `elem` and `genericClosure`, whose keys
# with functions in them compare only by identity.
let
  P = {
    a = {
      f = x: x;
    };
  };
  R = P.a;
in
[
  (builtins.seq R true)
  (P ? a)
  (builtins.seq R true)
  (builtins.elem R [ P.a ])
  (builtins.length (
    builtins.genericClosure {
      startSet = [
        { key = [ R ]; }
        { key = [ P.a ]; }
      ];
      operator = _: [ ];
    }
  ))
]
