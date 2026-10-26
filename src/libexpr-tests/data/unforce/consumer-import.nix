# The producer an imported file (as Nixpkgs' lib and platform files are).
let
  L = {
    name = "hello";
    meta = {
      platform = import ./lib/platform.nix;
    };
  };
in
[
  (builtins.seq L true)
  (builtins.seq L.meta.platform true)
  (L.meta.platform == import ./lib/platform.nix)
  (builtins.elem (import ./lib/platform.nix) [ L.meta.platform ])
]
