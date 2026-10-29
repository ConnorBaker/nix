# shared, reverted with e1's job, is promoted in e2's, and its s is forced
# there after shared's own force ended: s's string lies above where that
# ended, and is kept with shared. e3's job allocates above what e2 kept,
# then reads s.
let
  shared = {
    s = builtins.concatStringsSep "" [
      "a"
      "b"
    ];
  };
in
{
  e1 = builtins.seq shared.s 1;
  e2 = builtins.seq shared.s 2;
  e3 = builtins.seq (builtins.genList (i: i * 2) 64) shared.s;
}
