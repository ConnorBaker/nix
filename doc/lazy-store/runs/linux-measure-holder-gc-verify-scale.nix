# `nix-store --gc` and `nix-store --verify --check-contents` on a store
# inflated to hold M objects, with the real binaries, under the holder
# representation and under the route it replaces (06-holder-index.md section
# 4, *the collector and the verifier at scale*).  The driver is
# `linux-holder-gc-verify-scale.sh` beside this file: one first entry of an
# N-file tree into a fresh store, then M further blobs — rows attached to that
# path for the holder, files under `.objects/blobs/<xx>/` for the tip — then
# the two commands timed.
#   nix build --impure --no-link --print-out-paths -L -f linux-measure-holder-gc-verify-scale.nix \
#     --argstr repo <path to the checkout> \
#     --argstr tipRev <revision of the branch tip> \
#     --argstr holderRev <revision of the holder prototype> \
#     [--arg N 40000] [--arg M 1000000]
# Both packages are pinned by the revisions given.  The tip's synthetic blob
# files are empty, so its `--verify --check-contents` also reports M corrupt
# objects and exits 1; only the error reporting is spurious, and the document
# says so where it quotes the number.
{
  repo,
  tipRev,
  holderRev,
  N ? 40000,
  M ? 1000000,
}:
let
  tipFlake = builtins.getFlake "git+file://${repo}?rev=${tipRev}";
  holderFlake = builtins.getFlake "git+file://${repo}?rev=${holderRev}";
  pkgs = tipFlake.inputs.nixpkgs.legacyPackages.aarch64-linux;
  tip = tipFlake.packages.aarch64-linux.nix-cli;
  holder = holderFlake.packages.aarch64-linux.nix-cli;
  dirs = "holder=${holder}/bin tip=${tip}/bin";
in
pkgs.runCommand "lazy-store-holder-gc-verify-scale-${toString M}"
  {
    nativeBuildInputs = with pkgs; [
      bash
      coreutils
      gnugrep
      gnused
      gawk
      gitMinimal
      findutils
      sqlite
      python3
    ];
    preferLocalBuild = false;
    allowSubstitutes = false;
  }
  ''
    mkdir -p $out
    export MEASURE_ROOT=$TMPDIR/m MEASURE_N=${toString N} MEASURE_M=${toString M}
    export MEASURE_BINS="holder tip"
    export MEASURE_BINDIRS=${builtins.toJSON dirs}
    bash ${./linux-holder-gc-verify-scale.sh} 2>&1 | tee $out/scale2.log
  ''
