# What the object store's directories cost, made eagerly against made lazily
# (01-specification.md section 10, *The object store's three kinds are sharded
# 256 ways*, *The shards are made lazily*; 05-validation.md section 6).  The
# program is `linux-floor-object-store-directories.c` beside this file: the
# `mkdir` that making every shard at store creation would cost once, the
# `lstat` that `createDirectories` would then cost at every writable store's
# open, the same for the four kind directories alone, and the lazy
# alternative's whole-lifetime price.  Nothing but nixpkgs is taken from the
# repository.
#   nix build --impure --no-link --print-out-paths -L -f linux-measure-floor-object-store-directories.nix \
#     --argstr repo <path to the checkout> --argstr rev <revision>
{
  repo,
  rev,
}:
let
  f = builtins.getFlake "git+file://${repo}?rev=${rev}";
  pkgs = f.inputs.nixpkgs.legacyPackages.aarch64-linux;
in
pkgs.runCommand "lazy-store-floor-object-store-directories"
  {
    nativeBuildInputs = [
      pkgs.gcc
      pkgs.coreutils
    ];
    preferLocalBuild = false;
    allowSubstitutes = false;
    src = ./linux-floor-object-store-directories.c;
  }
  ''
    mkdir -p $out
    cp $src dirs.c
    gcc -Wall -O2 -o dirs dirs.c
    echo "cpus: $(nproc); fs: $(stat -f -c %T $TMPDIR)" | tee $out/dirs.log
    ./dirs "$TMPDIR" | tee -a $out/dirs.log
  ''
