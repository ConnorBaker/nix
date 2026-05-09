let
  count = 180;
  indices = builtins.genList (i: i) count;
  names = builtins.map (i: "n${toString i}") indices;
  sampleIndices = builtins.genList (i: count - 10 + i) 10;
  self = builtins.listToAttrs (
    builtins.map
      (i:
        let
          name = builtins.elemAt names i;
        in
        {
          inherit name;
          value =
            if i == 0 then {
              score = 1;
              pair = [ 1 1 ];
            } else
              let
                prev = self.${builtins.elemAt names (i - 1)};
                prev2 =
                  if i == 1
                  then prev
                  else self.${builtins.elemAt names (i - 2)};
              in {
                score = prev.score + i + builtins.length prev.pair + builtins.length prev2.pair;
                pair = [ prev.score prev2.score ];
              };
        })
      indices);
  forcedTotal = builtins.foldl' (acc: name: acc + self.${name}.score) 0 names;
in {
  inherit forcedTotal;
  tailScores = builtins.map (i: self.${builtins.elemAt names i}.score) sampleIndices;
}
