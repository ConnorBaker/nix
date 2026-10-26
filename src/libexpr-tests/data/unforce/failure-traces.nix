# A failure keeps the frames it first propagated through (`… in an operand
# of '+'`, under `tryEval`), which b shows when it raises it again.
let
  t = builtins.addErrorContext "first-path context" (throw "boom");
in
{
  a = builtins.seq (import ./lib/junk.nix 100) (builtins.tryEval (t + 1)).success;
  b = t;
}
