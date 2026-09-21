# Can the `linkat` that enters a written file as its blob be taken off the
# walk's thread?  The walk writes the files in its own order and hands each
# name to one other thread that links it, so one thread still touches the
# blobs directory and there is no contention to lose (01-specification.md
# section 10, *A written file is entered as its blob on the walk's own
# thread*; 05-validation.md section 6).  The isolated floor says it would
# save; the real walk says it does not, and that difference is the entry's
# reason.  The program is `linux-floor-link-on-another-thread.c` beside this
# file.  Nothing but nixpkgs is taken from the repository.
#   nix build --impure --no-link --print-out-paths -L \
#     -f linux-measure-floor-link-on-another-thread.nix \
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
pkgs.runCommand "lazy-store-floor-link-on-another-thread-${toString N}${tag}"
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
    cc -Wall -O2 -o floor3 ${./linux-floor-link-on-another-thread.c} -lpthread
    export FLOOR_N=${toString N}
    {
      echo "cpus: $(nproc); tmpdir fs: $(stat -f -c %T $TMPDIR); load: $(cat /proc/loadavg)"
      for trial in 1 2 3; do
        ./floor3 write-only        0
        ./floor3 write+link-here   1
        ./floor3 write+link-thread 2
        echo "--- trial $trial, load: $(cat /proc/loadavg)"
      done
    } 2>&1 | tee $out/floor3.log
  ''
