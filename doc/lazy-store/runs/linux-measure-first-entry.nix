# The first entry of a new tree, taken apart (01-specification.md section 10,
# *the object store's three kinds are sharded* and *the copy walks a
# directory's subdirectories on a bounded pool*; the tables in
# 05-validation.md section 6), run as an aarch64-linux derivation on a remote
# builder.  The driver is `linux-first-entry.sh` beside this file; `sections`
# selects what it does (`entry`, `phases`, `strace`, `profile`, `split`,
# `perf`), `order` fixes the order the labels run in so that a round can be
# reversed, and `N` is the number of files in the generated repository.
#   nix build --impure --no-link --print-out-paths -L -f linux-measure-first-entry.nix \
#     --argstr repo <path to the checkout> \
#     --argstr baseRev <revision before the change> \
#     --argstr branchRev <revision after it> \
#     --argstr masterRev <revision of master it is based on> \
#     [--arg N 40000] [--argstr sections "entry strace"] \
#     [--argstr order "master-off branch-base branch-shard master-on"]
# Every package is pinned by the revisions given, so the working tree's state
# does not enter.  `master-off` and `master-on` are the same binary run with
# `auto-optimise-store` false and true; the branch has no such setting.
{
  repo,
  baseRev,
  branchRev,
  masterRev,
  N ? 40000,
  sections ? "entry",
  order ? null,
  tag ? "",
  script ? ./linux-first-entry.sh,
}:
let
  branch = builtins.getFlake "git+file://${repo}?rev=${branchRev}";
  base = builtins.getFlake "git+file://${repo}?rev=${baseRev}";
  master = builtins.getFlake "git+file://${repo}?rev=${masterRev}";
  pkgs = branch.inputs.nixpkgs.legacyPackages.aarch64-linux;
  bins = {
    branch-base = base.packages.aarch64-linux.nix-cli;
    branch-shard = branch.packages.aarch64-linux.nix-cli;
    master-off = master.packages.aarch64-linux.nix-cli;
    master-on = master.packages.aarch64-linux.nix-cli;
  };
  labels =
    if order == null then
      builtins.attrNames bins
    else
      builtins.filter (x: builtins.isString x && x != "") (builtins.split " +" order);
  dirs = builtins.concatStringsSep " " (map (l: "${l}=${bins.${l}}/bin") (builtins.attrNames bins));
in
pkgs.runCommand "lazy-store-first-entry-${toString N}${tag}"
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
    ];
    preferLocalBuild = false;
    allowSubstitutes = false;
  }
  ''
    mkdir -p $out
    export MEASURE_ROOT=$TMPDIR/m MEASURE_N=${toString N}
    export MEASURE_SECTIONS=${builtins.toJSON sections}
    export MEASURE_BINS=${builtins.toJSON (builtins.concatStringsSep " " labels)}
    export MEASURE_BINDIRS=${builtins.toJSON dirs}
    bash ${script} 2>&1 | tee $out/entry.log
  ''
