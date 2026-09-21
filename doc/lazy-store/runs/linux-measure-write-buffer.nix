# The Linux peak-memory measurement of the evaluator's write buffer
# (04-derivation.md section 1.1 and section 2, *Memory*; the table in
# 05-validation.md section 6), run as an aarch64-linux derivation on a remote
# builder:
#   nix build --impure --no-link --print-out-paths -L -f linux-measure-write-buffer.nix \
#     --argstr repo <path to a git repository holding both revisions> \
#     --argstr branchRev <revision of the branch> --argstr masterRev <revision of master> \
#     [--argstr Ns "1000 4000 16000 32000 64000"]
# Both packages are pinned by the revisions given, so the working tree's state
# does not enter.  The driver is the branch's own
# tests/functional/differential/measure/measure-eval-linux.sh, run once with
# each binary; GNU `time -v` gives the peak resident set.
{
  repo,
  branchRev,
  masterRev,
  Ns ? "1000 4000 16000 32000 64000",
}:
let
  branch = builtins.getFlake "git+file://${repo}?rev=${branchRev}";
  master = builtins.getFlake "git+file://${repo}?rev=${masterRev}";
  pkgs = branch.inputs.nixpkgs.legacyPackages.aarch64-linux;
  branchNix = branch.packages.aarch64-linux.nix-cli;
  masterNix = master.packages.aarch64-linux.nix-cli;
  script = "${branch}/tests/functional/differential/measure/measure-eval-linux.sh";
in
pkgs.runCommand "lazy-store-write-buffer-measure"
  {
    nativeBuildInputs = [
      pkgs.bash
      pkgs.coreutils
      pkgs.gnugrep
      pkgs.gnused
      pkgs.gawk
      pkgs.time
    ];
    preferLocalBuild = false;
    allowSubstitutes = false;
    passthru = { inherit branchNix masterNix; };
  }
  ''
    mkdir -p $out
    export MEASURE_ROOT=$TMPDIR/m TIME_BIN=${pkgs.time}/bin/time HOME=$TMPDIR
    {
      echo "== reference (master ${masterRev})"
      ${pkgs.bash}/bin/bash ${script} ${masterNix}/bin "${Ns}"
      echo "== branch (${branchRev})"
      ${pkgs.bash}/bin/bash ${script} ${branchNix}/bin "${Ns}"
    } 2>&1 | tee $out/memory.log
  ''
