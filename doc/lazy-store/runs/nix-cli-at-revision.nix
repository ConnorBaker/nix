# One Nix CLI built from one revision of one repository, for the measurement
# drivers beside this file that take binaries rather than revisions
# (`linux-measure-first-entry.nix`, `linux-measure-first-entry-holder.nix`,
# `linux-measure-object-store-parallel.nix`,
# `linux-measure-holder-gc-verify-scale.nix`):
#   nix build --impure --no-link --print-out-paths -L -f nix-cli-at-revision.nix \
#     --argstr repo <path to the checkout> --argstr rev <revision>
# The revision pins the package, so the working tree's state does not enter.
{
  repo,
  rev,
  system ? "aarch64-linux",
}:
let
  f = builtins.getFlake "git+file://${repo}?rev=${rev}";
in
f.packages.${system}.nix-cli
