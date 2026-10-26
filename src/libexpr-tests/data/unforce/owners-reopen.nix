# Q's segment reopened: H's call forces Q, which builds only its element's
# thunk; the printer then forces that element, of Q's, right after Q's
# segment. H, printed, holds the element.
let
  Q = [ (builtins.seq [ ] (x: x)) ];
  H = (x: x) Q;
  H2 = builtins.head [ Q ];
in
[
  [ H ]
  (H == Q)
  [
    H
    Q
  ]
  [ H2 ]
  (H2 == Q)
  null
]
