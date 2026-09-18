# The Linux object-store cost measurement (01-specification.md sections 9.9/9.10; the tables in
# 05-validation.md section 6), run as an aarch64-linux derivation on a remote builder through the daemon:
#   nix build --impure --no-link --print-out-paths -L -f linux-measure-object-store.nix \
#     --argstr repo <path to the checkout> --argstr branchRev <revision of the branch> \
#     --argstr masterRev <revision of master it is based on> [--arg N 40000]
# Both packages are pinned by the revisions given, so the working tree's state does not enter.
{
  N ? 4000,
  repo,
  branchRev,
  masterRev,
}:
let
  branch = builtins.getFlake "git+file://${repo}?rev=${branchRev}";
  master = builtins.getFlake "git+file://${repo}?rev=${masterRev}";
  pkgs = branch.inputs.nixpkgs.legacyPackages.aarch64-linux;
  branchNix = branch.packages.aarch64-linux.nix-cli;
  masterNix = master.packages.aarch64-linux.nix-cli;
  script = builtins.toFile "timing-object-store.sh" (builtins.readFile ./linux-timing-object-store.sh);
in
pkgs.runCommand "lazy-store-object-store-measure-${toString N}"
  {
    nativeBuildInputs = [
      pkgs.bash
      pkgs.coreutils
      pkgs.gnugrep
      pkgs.gnused
      pkgs.gawk
      pkgs.gitMinimal
      pkgs.gnutar
      pkgs.findutils
    ];
    preferLocalBuild = false;
    allowSubstitutes = false;
    passthru = { inherit branchNix masterNix; };
  }
  ''
    mkdir -p $out
    export MEASURE_ROOT=$TMPDIR/m MEASURE_N=${toString N}
    ${pkgs.bash}/bin/bash ${script} ${branchNix}/bin ${masterNix}/bin 2>&1 | tee $out/timing.log
  ''
