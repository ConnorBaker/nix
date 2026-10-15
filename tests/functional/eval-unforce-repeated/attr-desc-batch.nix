# As attr-desc.nix, with zz allocating more than the default batch size (64 MiB), so that the default
# configuration also prints zz and yy in different batches.
let s = { x = 1; }; big = n: builtins.genList (i: i) n; in { zz = builtins.seq (builtins.length (big 4000000)) s; yy = s; }
