# Phase split of the write-buffer wall-clock regression (the table in
# 05-validation.md section 6, *Which phase*), run as an aarch64-linux
# derivation on a remote builder:
#   nix build --impure --no-link --print-out-paths -L -f linux-measure-eval-phases.nix \
#     --argstr repo <path to a git repository holding both revisions> \
#     --argstr branchRev <revision of the branch> --argstr masterRev <revision of master> \
#     [--argstr Ns "16000 64000"]
# Both packages are pinned by the revisions given, so the working tree's state
# does not enter.  For each binary and each N:
#   ro    -- `nix eval --read-only --json`: evaluation, the derivation named
#            but never written (primops.cc computeStorePath on both sides).
#   cold  -- `nix eval --json` into a fresh store: everything.
#   warm  -- the same command again in the same store: every path is valid,
#            so no object is written; the buffer's bookkeeping, the hashes
#            and the validity query remain.
# cold-warm is the store write; warm-ro is the naming and bookkeeping;
# ro is evaluation proper (doors, Value accessors, JSON rendering).
{
  repo,
  branchRev,
  masterRev,
  Ns ? "16000 64000",
}:
let
  branch = builtins.getFlake "git+file://${repo}?rev=${branchRev}";
  master = builtins.getFlake "git+file://${repo}?rev=${masterRev}";
  pkgs = branch.inputs.nixpkgs.legacyPackages.aarch64-linux;
  branchNix = branch.packages.aarch64-linux.nix-cli;
  masterNix = master.packages.aarch64-linux.nix-cli;
in
pkgs.runCommand "lazy-store-evalwall-phase"
  {
    nativeBuildInputs = [
      pkgs.bash
      pkgs.coreutils
      pkgs.gnugrep
      pkgs.gnused
      pkgs.gawk
      pkgs.time
      pkgs.strace
    ];
    preferLocalBuild = false;
    allowSubstitutes = false;
  }
  ''
    set +e
    set +o pipefail
    mkdir -p $out
    export HOME=$TMPDIR
    root=$TMPDIR/m
    TIME=${pkgs.time}/bin/time

    setup() { # <dir>
      local d=$1
      rm -rf "$d"; mkdir -p "$d/store" "$d/var/nix" "$d/etc"
      printf 'experimental-features = nix-command\nsandbox = false\nsubstituters =\nbuild-users-group =\n' > "$d/etc/nix.conf"
    }
    useenv() { # <dir>
      local d=$1
      export NIX_STORE_DIR=$d/store NIX_STATE_DIR=$d/var/nix NIX_LOG_DIR=$d/var/log/nix
      export NIX_CONF_DIR=$d/etc NIX_DATA_DIR=$d/share NIX_LOCALSTATE_DIR=$d/var NIX_REMOTE=
      unset NIX_CONFIG || true
    }
    report() { # <label> <N> <timelog>
      local label=$1 n=$2 tl=$3 rss wall flushes objects
      rss=$(grep -o 'Maximum resident set size (kbytes): [0-9]*' "$tl" | grep -o '[0-9]*$' || echo NA)
      wall=$(grep 'Elapsed (wall clock) time' "$tl" | sed 's/.*: //' || echo NA)
      flushes=$(grep -c 'writing [0-9]* store objects' "$tl" || true)
      objects=$( (grep -o 'writing [0-9]* store objects' "$tl" || true) | (grep -o '[0-9]*' || true) | awk '{s+=$1} END{print s+0}')
      printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$label" "$n" "$rss" "$wall" "$flushes" "$objects"
    }

    drv='derivation { name = "m-''${toString i}"; system = "x86_64-linux"; builder = "/bin/sh"; args = [ "-c" "echo ''${toString i} > ''$out" ]; }'

    phases() { # <who> <bindir>
      local who=$1 bin=$2 n
      for n in ${Ns}; do
        local list="builtins.genList (i: ($drv).drvPath) $n"
        # ro
        local d=$root/$who-ro-$n; setup "$d"; useenv "$d"
        $TIME -v "$bin/nix" eval -vv --read-only --json --expr "$list" > "$d/out" 2> "$d/tl" || true
        report "$who-eval-ro" "$n" "$d/tl"
        # cold then warm, same store
        d=$root/$who-cw-$n; setup "$d"; useenv "$d"
        $TIME -v "$bin/nix" eval -vv --json --expr "$list" > "$d/out1" 2> "$d/tl1" || true
        report "$who-eval-cold" "$n" "$d/tl1"
        $TIME -v "$bin/nix" eval -vv --json --expr "$list" > "$d/out2" 2> "$d/tl2" || true
        report "$who-eval-warm" "$n" "$d/tl2"
        cmp -s "$d/out1" "$d/out2" && echo "# $who $n cold==warm output OK" || echo "# $who $n OUTPUT DIFFERS"
      done
      # nix-instantiate at the largest N
      local nmax; nmax=$(echo "${Ns}" | awk '{print $NF}')
      local wf=$root/fan-$who.nix
      printf 'builtins.genList (i: %s) %s\n' "$drv" "$nmax" > "$wf"
      local d=$root/$who-inst-ro; setup "$d"; useenv "$d"
      $TIME -v "$bin/nix-instantiate" -vv --readonly-mode "$wf" > "$d/out" 2> "$d/tl" || true
      report "$who-inst-ro" "$nmax" "$d/tl"
      d=$root/$who-inst; setup "$d"; useenv "$d"
      $TIME -v "$bin/nix-instantiate" -vv "$wf" > "$d/out1" 2> "$d/tl1" || true
      report "$who-inst-cold" "$nmax" "$d/tl1"
      $TIME -v "$bin/nix-instantiate" -vv "$wf" > "$d/out2" 2> "$d/tl2" || true
      report "$who-inst-warm" "$nmax" "$d/tl2"
    }

    {
      printf 'label\tN\trssKB\twall\tflushes\tobjects\n'
      phases master ${masterNix}/bin
      phases branch ${branchNix}/bin
    } 2>&1 | tee $out/phase.tsv

    # Syscall accounting on the cold eval at N=16000, both binaries.
    for who in master branch; do
      case $who in master) bin=${masterNix}/bin;; branch) bin=${branchNix}/bin;; esac
      d=$root/$who-strace; setup "$d"; useenv "$d"
      list="builtins.genList (i: ($drv).drvPath) 16000"
      ${pkgs.strace}/bin/strace -f -c -w -o $out/strace-$who.txt "$bin/nix" eval --json --expr "$list" > "$d/out" 2>"$d/err" || true
      echo "== $who strace rc, stderr tail" >> $out/strace-notes.txt
      tail -5 "$d/err" >> $out/strace-notes.txt || true
    done
    cat $out/strace-master.txt $out/strace-branch.txt || true
  ''
