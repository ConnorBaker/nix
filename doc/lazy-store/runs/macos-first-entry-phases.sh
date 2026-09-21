#!/usr/bin/env bash
# macOS: the first entry of a new tree in phases, a branch binary against a
# master binary (doc/lazy-store/05-validation.md section 6, *macOS, once,
# direction only*; 06-holder-index.md section 4, *macOS first entry*).  There
# is no derivation: the measurement is of this machine's filesystem and its
# endpoint-security agent, so it runs outside the sandbox, against binaries
# built here.
#
#   MAC_BRANCH=<bindir> MAC_MASTER=<bindir> [MAC_N=4000] [MAC_ROOT=<dir>] \
#     bash macos-first-entry-phases.sh
#
# `real` on such a machine is dominated by the agent's first-touch cost and
# moves by an order of magnitude between identical trials; `sys` is the column
# to read and `user` is the control.  The repository is generated once per N
# and kept, so a second run does not pay for making it.
set -uo pipefail
BRANCH=${MAC_BRANCH:?MAC_BRANCH must name a directory holding the branch nix}
MASTER=${MAC_MASTER:?MAC_MASTER must name a directory holding the master nix}
N=${MAC_N:-4000}
W=${MAC_ROOT:-${TMPDIR:-/tmp}/lazy-store-macos-first-entry}
R=$W/repo-$N
mkdir -p "$W"; export HOME=$W/home; mkdir -p "$HOME"
echo "== N=$N  load: $(uptime)"
echo "branch: $BRANCH"
echo "master: $MASTER"
now() { python3 -c 'import time;print(time.time())'; }
t() { local s e rc; s=$(now); "$@" > /dev/null 2> "$W/err"; rc=$?; e=$(now)
  python3 -c "print('%.2f' % ($e - $s))" ; [ $rc -ne 0 ] && echo "   (exit $rc: $(head -c 300 "$W/err"))"; }
if [ ! -d "$R/.git" ]; then
  rm -rf "$R"; mkdir -p "$R"; for d in $(seq 0 39); do mkdir -p "$R/dir$d"; done
  for i in $(seq 1 "$N"); do echo "content $i" > "$R/dir$((i % 40))/f$i"; done
  ( cd "$R" && git init -q . && git add . && git -c user.name=n -c user.email=n@x commit -q -m init )
fi
echo "files: $(find "$R" -type f -not -path '*/.git/*' | wc -l)"
echo "loose objects: $(find "$R/.git/objects" -type f | wc -l)"
cfgOf() { case $1 in master*) printf 'extra-experimental-features = nix-command flakes\nauto-optimise-store = false\n';;
                     *) printf 'extra-experimental-features = nix-command flakes\n';; esac; }
binOf() { case $1 in master*) echo "$MASTER";; *) echo "$BRANCH";; esac; }
attrOf() { case $1 in master*) echo narHash;; *) echo treeHash;; esac; }
fresh() { local root=$W/root-$1; [ -e "$root" ] && chmod -R u+w "$root" 2>/dev/null; rm -rf "$root" "$W/cache-$1"; }
# shellcheck disable=SC2329 # ev is invoked indirectly: `t ev` below passes it to the timer
ev() { NIX_CONFIG="$(cfgOf "$1")" XDG_CACHE_HOME="$W/cache-$1" "$(binOf "$1")/nix" --store "$W/root-$1" \
        eval --raw --impure --expr "(builtins.fetchGit { url = \"$R\"; }).$2"; }
for label in branch master; do
  a=$(attrOf $label)
  for trial in 1 2 3; do
    fresh "$label"
    n1=$(t ev "$label" "$a"); n2=$(t ev "$label" "$a"); c=$(t ev "$label" outPath); c2=$(t ev "$label" outPath)
    printf '%-7s trial %d  naming(%s) cold %s  warm %s  copy %s  re-entry %s   load %s\n' \
      "$label" "$trial" "$a" "$n1" "$n2" "$c" "$c2" "$(uptime | sed 's/.*averages: //')"
  done
  fresh "$label-e"
  printf '%-7s whole first entry (outPath, fresh) %s\n' "$label" "$(t ev "$label-e" outPath)"
done
echo "== control: reading every git object once"
printf 'git cat-file --batch-all-objects --batch: %s\n' "$( { s=$(now); git -C "$R" cat-file --batch-all-objects --unordered --batch > /dev/null 2>&1; e=$(now); python3 -c "print('%.2f' % ($e - $s))"; } )"
printf 'git cat-file --batch-all-objects --batch-check: %s\n' "$( { s=$(now); git -C "$R" cat-file --batch-all-objects --unordered --batch-check > /dev/null 2>&1; e=$(now); python3 -c "print('%.2f' % ($e - $s))"; } )"
echo "== done, load: $(uptime)"
