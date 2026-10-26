# A reverted cell's position (`Value::determinePos`): at, N levels down a
# chain, is forced before any job; at.inner, older than the jobs, is first
# forced by the first element and reverted with its job. The second element
# forces the chain deeply, past max-call-depth about 10000 levels down, at
# a level whose position the error reports: for at.inner, the position of
# the set it holds without unforcing. (depth-pos-N.nix, N around the edge,
# where each front end reaches it. `deepSeq`, so that no printer prints the
# chain.)
N:
let
  f = n: { inner = f (n + 1); };
  x = f 0;
  at = builtins.foldl' (v: _: v.inner) x (builtins.genList (i: i) N);
in
builtins.seq at [
  (builtins.seq at.inner (import ./junk.nix 100))
  (builtins.deepSeq x null)
]
