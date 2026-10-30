# nix-env -qa --json: members by attribute path, null for a failed assertion, deferred printing errors.
with import ../config.nix;
let
  drv =
    name: system: meta:
    mkDerivation { inherit name system meta; };
in
{
  x = drv "b-1.0" (throw "x") { };
  y = drv "a-1.0" (throw "y") { };
  utf8 = drv "c-1.0" system { bad = builtins.substring 0 1 "é"; };
  aborts = drv "d-1.0" system { bad = abort "d"; };
}
