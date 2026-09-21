# The object store's cost property with the copy sequential and parallel
# beside each other (01-specification.md section 10, *the copy walks a
# directory's subdirectories on a bounded pool*; the table in
# 05-validation.md section 6, *Nothing the object store exists for
# regressed*).  It is `linux-measure-object-store.nix` with the branch run
# twice, once with one copy worker and once with the pool, through
# `linux-timing-object-store-parallel.sh`.
#   nix build --impure --no-link --print-out-paths -L -f linux-measure-object-store-parallel.nix \
#     --argstr repo <path to the checkout> \
#     --argstr seqRev <revision whose copy is sequential> \
#     --argstr parRev <revision whose copy is parallel> \
#     --argstr masterRev <revision of master> [--arg N 40000]
# Every package is pinned by the revisions given, so the working tree's state
# does not enter.
{
  repo,
  seqRev,
  parRev,
  masterRev,
  N ? 40000,
}:
let
  seqFlake = builtins.getFlake "git+file://${repo}?rev=${seqRev}";
  parFlake = builtins.getFlake "git+file://${repo}?rev=${parRev}";
  masterFlake = builtins.getFlake "git+file://${repo}?rev=${masterRev}";
  pkgs = parFlake.inputs.nixpkgs.legacyPackages.aarch64-linux;
  seq = seqFlake.packages.aarch64-linux.nix-cli;
  par = parFlake.packages.aarch64-linux.nix-cli;
  master = masterFlake.packages.aarch64-linux.nix-cli;
in
pkgs.runCommand "lazy-store-object-store-parallel-measure-${toString N}"
  {
    nativeBuildInputs = with pkgs; [
      bash
      coreutils
      gnugrep
      gnused
      gawk
      gitMinimal
      gnutar
      findutils
    ];
    preferLocalBuild = false;
    allowSubstitutes = false;
  }
  ''
    mkdir -p $out
    export MEASURE_ROOT=$TMPDIR/m MEASURE_N=${toString N}
    bash ${./linux-timing-object-store-parallel.sh} ${seq}/bin ${par}/bin ${master}/bin 2>&1 \
      | tee $out/timing.log
  ''
