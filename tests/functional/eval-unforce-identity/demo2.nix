# genericClosure keys containing functions are ordered only through cell
# identity; a split throws "cannot compare a set with a set".
let
  P = { a = { f = x: x; }; };
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
