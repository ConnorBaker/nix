# Does concurrency pay for the copy's per-file work?  Two questions the first
# floor left open: whether the siblings of ONE directory may be created
# concurrently (what a pool over a directory's own files would do, which ext4
# serialises on the directory's `i_rwsem`), and whether the link may be
# deferred out of the write's path (05-validation.md section 6, *The kernel
# time is the link*).  The program is
# `linux-floor-directory-concurrency.c` beside this file.  Nothing but nixpkgs
# is taken from the repository.
#   nix build --impure --no-link --print-out-paths -L \
#     -f linux-measure-floor-directory-concurrency.nix \
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
pkgs.runCommand "lazy-store-floor-directory-concurrency-${toString N}${tag}"
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
    cc -Wall -O2 -o floor2 ${./linux-floor-directory-concurrency.c} -lpthread
    export FLOOR_N=${toString N}
    {
      echo "cpus: $(nproc); tmpdir fs: $(stat -f -c %T $TMPDIR); load: $(cat /proc/loadavg)"
      for trial in 1 2 3; do
        ./floor2 write-seq            spread 1 0
        ./floor2 write+link-seq       spread 1 1
        ./floor2 write-1dir-seq       shared 1 0
        ./floor2 write-1dir-par4      shared 4 0
        ./floor2 write-1dir-par8      shared 8 0
        ./floor2 write+link-1dir-par4 shared 4 1
        ./floor2 write-par4           spread 4 0
        ./floor2 write-par8           spread 8 0
        ./floor2 defer-par1           defer  1 1
        ./floor2 defer-par4           defer  4 1
        ./floor2 defer-par8           defer  8 1
        echo "--- trial $trial done, load: $(cat /proc/loadavg)"
      done
    } 2>&1 | tee $out/floor2.log
  ''
