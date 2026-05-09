let
  count = 96;
  indices = builtins.genList (i: i) count;
  baseList = builtins.map
    (i: {
      idx = i;
      label = "item-${toString i}";
      pair = [ i (i + 1) ];
    })
    indices;
  baseAttrs = {
    left = baseList;
    right = baseList;
    nested = { inherit baseList; };
    copied = builtins.filter (x: x.idx >= 0) baseList;
    mapped = builtins.map (x: { inherit (x) idx label pair; }) baseList;
  };
  checks = {
    leftRight = baseAttrs.left == baseAttrs.right;
    leftNested = baseAttrs.left == baseAttrs.nested.baseList;
    leftCopied = baseAttrs.left == baseAttrs.copied;
    leftMapped = baseAttrs.left == baseAttrs.mapped;
  };
  sample = builtins.map (i: (builtins.elemAt baseAttrs.left i).idx) (builtins.genList (i: i) 8);
in {
  inherit checks sample;
  copiedLength = builtins.length baseAttrs.copied;
}
