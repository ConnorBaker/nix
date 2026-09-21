# Does the copy walk's parallelism survive the source accessor's lock?
# `GitSourceAccessor` takes one lock for every read, so under any parallel copy
# the read and the hash under it stay on one thread at a time and only the
# write side overlaps (01-specification.md section 10, *The copy walks a
# directory's subdirectories on a bounded pool*; 05-validation.md section 6,
# *Does the copy's parallelism survive the source accessor's lock?*).  The
# program is `linux-floor-parallel-copy-under-the-accessor-lock.c` beside this
# file: the per-file body in the walk's proportions, sequential against threads
# over the directories, with the lock and without it.  Nothing but nixpkgs is
# taken from the repository.
#   nix build --impure --no-link --print-out-paths -L -f linux-measure-floor-parallel-copy.nix \
#     --argstr repo <path to the checkout> --argstr rev <revision>
{
  repo,
  rev,
}:
let
  f = builtins.getFlake "git+file://${repo}?rev=${rev}";
  pkgs = f.inputs.nixpkgs.legacyPackages.aarch64-linux;
in
pkgs.runCommand "lazy-store-floor-parallel-copy"
  {
    nativeBuildInputs = [
      pkgs.gcc
      pkgs.coreutils
      pkgs.openssl.dev
    ];
    buildInputs = [ pkgs.openssl ];
    preferLocalBuild = false;
    allowSubstitutes = false;
    src = ./linux-floor-parallel-copy-under-the-accessor-lock.c;
  }
  ''
    mkdir -p $out
    cp $src floor4.c
    gcc -Wall -O2 -o floor4 floor4.c -lcrypto -lpthread
    {
      echo "cpus: $(nproc); fs: $(stat -f -c %T $TMPDIR); load: $(cat /proc/loadavg)"
      ./floor4 "$TMPDIR"
      echo "load at end: $(cat /proc/loadavg)"
    } | tee $out/floor4.log
  ''
