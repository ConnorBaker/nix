let
  count = 256;
  indices = builtins.genList (i: i) count;
  base = builtins.listToAttrs (
    builtins.map
      (i: {
        name = "k${toString i}";
        value = {
          idx = i;
          even = i < 128;
          bucket =
            if i < 64 then 0
            else if i < 128 then 1
            else if i < 192 then 2
            else if i < 224 then 3
            else 4;
          payload = builtins.genList (j: i + j) 8;
        };
      })
      indices);
  mapped = builtins.mapAttrs
    (name: value: {
      inherit name;
      idx = value.idx;
      even = value.even;
      bucket = value.bucket;
      total = builtins.foldl' (acc: x: acc + x) 0 value.payload;
    })
    base;
  trimmed = builtins.removeAttrs mapped [ "k1" "k3" "k5" "k7" "k9" ];
  intersected = builtins.intersectAttrs trimmed mapped;
  values = builtins.attrValues intersected;
  filtered = builtins.filter (x: x.even || x.bucket == 3) values;
  partitioned = builtins.partition (x: x.bucket < 2) filtered;
  sorted = builtins.sort (a: b: a.total < b.total) (partitioned.right ++ partitioned.wrong);
  regrouped = builtins.listToAttrs (
    builtins.map
      (x: {
        name = x.name;
        value = {
          inherit (x) idx total bucket even;
        };
      })
      sorted);
  firstFive = builtins.map (i: (builtins.elemAt sorted i).total) (builtins.genList (i: i) 5);
in {
  count = builtins.length sorted;
  bucketChecksum = builtins.foldl' (acc: name: acc + regrouped.${name}.bucket) 0 (builtins.attrNames regrouped);
  inherit firstFive;
}
