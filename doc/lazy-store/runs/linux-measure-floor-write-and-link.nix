# The floor of the copy's per-file kernel work: what creating N small files in
# 40 directories costs on the builder's own filesystem, what one `linkat` each
# into the blobs directory adds, and whether doing it from several threads
# costs less wall time (05-validation.md section 6, *The kernel time is the
# link*; 06-holder-index.md section 1).  The program is
# `linux-floor-write-and-link.c` beside this file.  Nothing but nixpkgs is
# taken from the repository.
#   nix build --impure --no-link --print-out-paths -L -f linux-measure-floor-write-and-link.nix \
#     --argstr repo <path to the checkout> --argstr rev <revision> [--arg N 40000]
{
  repo,
  rev,
  N ? 40000,
  tag ? "",
}:
let
  f = builtins.getFlake "git+file://${repo}?rev=${rev}";
  pkgs = f.inputs.nixpkgs.legacyPackages.aarch64-linux;
in
pkgs.runCommand "lazy-store-floor-write-and-link-${toString N}${tag}"
  {
    nativeBuildInputs = [
      pkgs.gcc
      pkgs.coreutils
    ];
    preferLocalBuild = false;
    allowSubstitutes = false;
  }
  ''
    mkdir -p $out
    cc -Wall -O2 -o floor ${./linux-floor-write-and-link.c} -lpthread
    export FLOOR_N=${toString N}
    {
      echo "cpus: $(nproc); tmpdir fs: $(stat -f -c %T $TMPDIR); load: $(cat /proc/loadavg)"
      echo "N=$FLOOR_N files in 40 directories"
      for trial in 1 2 3; do
        ./floor create                1 0 0 0
        ./floor create+stat           1 0 1 0
        ./floor create+link           1 1 1 0
        ./floor create+link-noStat    1 1 0 0
        ./floor create+link-sharded   1 1 1 1
        ./floor create-par4           4 0 0 0
        ./floor create+link-par4      4 1 1 0
        ./floor create+link-par8      8 1 1 0
        ./floor create+link-par16    16 1 1 0
        ./floor create+link-par8-shd  8 1 1 1
        echo "--- trial $trial done, load: $(cat /proc/loadavg)"
      done
    } 2>&1 | tee $out/floor.log
  ''
