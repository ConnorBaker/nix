# The first entry of a new tree under the holder representation
# (06-holder-index.md section 4), run as an aarch64-linux derivation on a
# remote builder.  The driver is `linux-first-entry-holder.sh` beside this
# file; it is `linux-first-entry.sh` with the holder's labels added, each
# label selecting a probe through `NIX_HOLDER_PROBE`:
#   tip-{seq,par}     the route the holder replaces
#   holder-{seq,par}  the holder, one point query a file
#   htxn-{seq,par}    the same queries inside one read transaction
#   hpre-{seq,par}    every key prefetched into memory before the copy
#   hnr-{seq,par}     the holder probing but writing no rows (the ablation)
#   master-{off,on}   master with `auto-optimise-store` false and true
#   nix build --impure --no-link --print-out-paths -L -f linux-measure-first-entry-holder.nix \
#     --argstr repo <path to the checkout> \
#     --argstr tipRev <revision of the branch tip> \
#     --argstr holderRev <revision of the holder prototype> \
#     --argstr masterRev <revision of master> \
#     [--arg N 40000] [--argstr sections "entry strace gcverify store"] \
#     [--argstr order "holder-par hnr-par tip-par"] [--argstr trials "10"]
# Every package is pinned by the revisions given.  A round is read against
# itself: run it twice with `order` reversed and take the median of the two
# runs' trials together (06-holder-index.md section 4, *the measurement
# problem*).
{
  repo,
  tipRev,
  holderRev,
  masterRev,
  N ? 40000,
  sections ? "entry",
  order ? null,
  tag ? "",
  trials ? "3",
  script ? ./linux-first-entry-holder.sh,
}:
let
  tipFlake = builtins.getFlake "git+file://${repo}?rev=${tipRev}";
  holderFlake = builtins.getFlake "git+file://${repo}?rev=${holderRev}";
  masterFlake = builtins.getFlake "git+file://${repo}?rev=${masterRev}";
  pkgs = tipFlake.inputs.nixpkgs.legacyPackages.aarch64-linux;
  tip = tipFlake.packages.aarch64-linux.nix-cli;
  holder = holderFlake.packages.aarch64-linux.nix-cli;
  master = masterFlake.packages.aarch64-linux.nix-cli;
  bins = {
    tip-seq = tip;
    tip-par = tip;
    holder-seq = holder;
    holder-par = holder;
    htxn-seq = holder;
    htxn-par = holder;
    hpre-seq = holder;
    hpre-par = holder;
    hnr-seq = holder;
    hnr-par = holder;
    master-off = master;
    master-on = master;
  };
  labels =
    if order == null then
      builtins.attrNames bins
    else
      builtins.filter (x: builtins.isString x && x != "") (builtins.split " +" order);
  dirs = builtins.concatStringsSep " " (map (l: "${l}=${bins.${l}}/bin") (builtins.attrNames bins));
in
pkgs.runCommand "lazy-store-first-entry-holder-${toString N}${tag}"
  {
    nativeBuildInputs = with pkgs; [
      bash
      coreutils
      gnugrep
      gnused
      gawk
      gitMinimal
      findutils
      strace
      sqlite
    ];
    preferLocalBuild = false;
    allowSubstitutes = false;
  }
  ''
    mkdir -p $out
    export MEASURE_ROOT=$TMPDIR/m MEASURE_N=${toString N} MEASURE_TRIALS=${trials}
    export MEASURE_SECTIONS=${builtins.toJSON sections}
    export MEASURE_BINS=${builtins.toJSON (builtins.concatStringsSep " " labels)}
    export MEASURE_BINDIRS=${builtins.toJSON dirs}
    bash ${script} 2>&1 | tee $out/entry.log
  ''
