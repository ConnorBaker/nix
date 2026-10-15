# Q is an imported file (a fileEvalCache cell, as for Nixpkgs' lib and platform files); L is a package-like set
# whose meta reads it. Job 1 forces L; job 2 forces L.meta.platform; later jobs compare with a fresh import.
let L = { name = "hello"; meta = { platform = import ./cx-lib/platform.nix; }; };
in [ (builtins.seq L true) (builtins.seq L.meta.platform true)
     (L.meta.platform == import ./cx-lib/platform.nix) (builtins.elem (import ./cx-lib/platform.nix) [ L.meta.platform ]) ]
