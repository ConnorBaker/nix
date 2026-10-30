# nix-env -qa --json: the walk queries a before b, which comes first by name.
builtins.mapAttrs (_: name: import ./no-outpath.nix // { inherit name; }) {
  a = "y-1.0";
  b = builtins.trace "walk b" "x-1.0";
}
