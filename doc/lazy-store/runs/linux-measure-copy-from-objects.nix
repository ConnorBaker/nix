# The copy of a tree the store holds, made from the store's own objects
# (01-specification.md section 9.10, *One route*, the second route; section 10,
# *The copy of a tree the store holds is made from the store's own objects*;
# the tables in 05-validation.md section 6, *The copy from the store's own
# objects on Linux*).  The driver is `linux-copy-from-objects.sh` beside this
# file: a fresh store root primed with a clean commit, then three one-file
# edits of the dirty checkout, each timed as `nix eval --raw` of
# `builtins.fetchGit`'s `.treeHash` (the naming alone), `.outPath` (naming,
# copy and registration) and `.outPath` again (the memo hit), and a fourth
# edit's copy under `strace -f -c -w` for the syscalls per unchanged file.
#   nix build --impure --no-link --print-out-paths -L -f linux-measure-copy-from-objects.nix \
#     --argstr repo <path to the checkout> \
#     --argstr beforeRev <revision before the route> \
#     --argstr afterRev <revision after it>
# Both packages are pinned by the revisions given, so the working tree's state
# does not enter.  `macos-copy-from-objects.sh` beside this file is the same
# measurement on a Mac, run outside the sandbox and direction only.
{
  repo,
  beforeRev,
  afterRev,
}:
let
  before = builtins.getFlake "git+file://${repo}?rev=${beforeRev}";
  after = builtins.getFlake "git+file://${repo}?rev=${afterRev}";
  pkgs = after.inputs.nixpkgs.legacyPackages.aarch64-linux;
  beforeNix = before.packages.aarch64-linux.nix-cli;
  afterNix = after.packages.aarch64-linux.nix-cli;
  script = builtins.toFile "copy-from-objects.sh" (builtins.readFile ./linux-copy-from-objects.sh);
in
pkgs.runCommand "lazy-store-copy-from-objects"
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
      diffutils
    ];
    preferLocalBuild = false;
    allowSubstitutes = false;
  }
  ''
    mkdir -p $out
    export MEASURE_ROOT=$TMPDIR/m OUT=$out
    ${pkgs.bash}/bin/bash ${script} ${beforeNix}/bin ${afterNix}/bin 2>&1 | tee $out/phases.log
  ''
