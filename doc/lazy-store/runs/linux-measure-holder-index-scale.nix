# The holder index at a real store's size, against the directory it would
# replace (06-holder-index.md section 4, *the matched probe costs*).  The
# program is `linux-holder-index-scale.c` beside this file; it fills a
# `Blobs` table of `dbN` rows and a sharded `blobs/<xx>/<id>` directory of
# `fileN` files on the same filesystem in the same run, then times the probe
# on each side for a miss and for a hit.  Run as an aarch64-linux derivation
# on a remote builder; nothing but nixpkgs is taken from the repository, so
# `repo` names any revision whose flake pins the nixpkgs to build against.
#   nix build --impure --no-link --print-out-paths -L -f linux-measure-holder-index-scale.nix \
#     --argstr repo <path to the checkout> --argstr rev <revision> \
#     [--argstr dbN 10000000] [--argstr fileN 1000000] [--argstr trim "trim"]
# `trim` runs the table alone, with the identifier as text and no files, which
# is what a 10^7 row fill needs to finish inside one build.
{
  repo,
  rev,
  dbN ? "1000000",
  fileN ? "1000000",
  trim ? "",
}:
let
  f = builtins.getFlake "git+file://${repo}?rev=${rev}";
  pkgs = f.inputs.nixpkgs.legacyPackages.aarch64-linux;
in
pkgs.runCommand "lazy-store-holder-index-scale"
  {
    nativeBuildInputs = [
      pkgs.gcc
      pkgs.coreutils
      pkgs.sqlite.dev
      pkgs.pkg-config
      pkgs.sqlite
    ];
    buildInputs = [ pkgs.sqlite ];
    preferLocalBuild = false;
    allowSubstitutes = false;
    src = ./linux-holder-index-scale.c;
  }
  ''
    mkdir -p $out
    cp $src scale.c
    gcc -O2 -o scale scale.c -lsqlite3
    {
      echo "cpus: $(nproc); fs: $(stat -f -c %T $TMPDIR); load: $(cat /proc/loadavg)"
      df -h "$TMPDIR" | tail -1
      df -i "$TMPDIR" | tail -1
      echo "sqlite: $(pkg-config --modversion sqlite3)"
      ./scale "$TMPDIR" ${dbN} ${fileN} ${trim}
      echo "load at end: $(cat /proc/loadavg)"
    } | tee $out/scale.log
  ''
