#!/usr/bin/env bash
# 05-validation.md section 6, "Where a first entry's bytes come from": the macOS first entry of a 40 000-file
# checkout with the object-database read and the carry each on and off (the carry was measured and not kept).
# Parameters: LAZY_STORE_SCRATCH, LAZY_STORE_TREE, the CLI paths named at the top of the script.
# The first entry with the carry on and off (the same binary), master beside.
set -uo pipefail
BINDIRS=${MEASURE_BINDIRS:?}
W=${MEASURE_ROOT:-$TMPDIR/m}; mkdir -p "$W"; R=$W/repo; N=${MEASURE_N:-40000}
SECTIONS=${MEASURE_SECTIONS:-"entry strace"}
BINS=${MEASURE_BINS:-"carry-on carry-off master-off"}
export HOME=$W/home; mkdir -p "$HOME"

echo "== environment"
uname -a; echo "cpus: $(nproc)"; echo "tmpdir fs: $(stat -f -c %T "$TMPDIR") ($TMPDIR)"
echo "load at start: $(cat /proc/loadavg)"
echo "bindirs: $BINDIRS"; echo "N=$N sections=$SECTIONS bins=$BINS"

now() { date +%s.%N; }
t() { local s e rc; s=$(now); "$@" > /dev/null 2> "$W/err"; rc=$?; e=$(now)
  awk -v s="$s" -v e="$e" 'BEGIN { printf "%.2f", e - s }'
  [ $rc -ne 0 ] && printf '(exit %s: %s)' $rc "$(grep -m1 -i error "$W/err" | cut -c1-160)"; echo; }

echo "== repository: $N files in 40 directories"
rm -rf "$R"; mkdir -p "$R"; for d in $(seq 0 39); do mkdir -p "$R/dir$d"; done
for i in $(seq 1 "$N"); do echo "content $i" > "$R/dir$((i % 40))/f$i"; done
( cd "$R" && git init -q . && git add . && git -c user.name=n -c user.email=n@x commit -q -m init )
echo "files: $(find "$R" -type f -not -path '*/.git/*' | wc -l)"
echo "loose objects: $(find "$R/.git/objects" -type f | wc -l)"
echo "bytes in files: $(find "$R" -type f -not -path '*/.git/*' -printf '%s\n' | awk '{s+=$1} END {print s}')"

binOf() { local e; for e in $BINDIRS; do [ "${e%%=*}" = "$1" ] && { echo "${e#*=}"; return; }; done
  echo "no bindir for $1" >&2; exit 1; }
cfgOf() { case $1 in
  master-on) printf 'extra-experimental-features = nix-command flakes\nauto-optimise-store = true\n';;
  master*)   printf 'extra-experimental-features = nix-command flakes\nauto-optimise-store = false\n';;
  *)         printf 'extra-experimental-features = nix-command flakes\n';; esac; }
envOf() { e=""
  case $1 in *nocarry*) e="$e _NIX_TEST_SOURCE_CARRY_BUDGET=0";; esac
  case $1 in *noodb*) e="$e _NIX_TEST_NO_ODB_READS=1";; esac
  echo "$e"; }
fresh() { local root=$W/root-$1; [ -e "$root" ] && chmod -R u+w "$root" 2>/dev/null; rm -rf "$root" "$W/cache-$1"; }
ev() { # <tag>
  # shellcheck disable=SC2046 # the environment list is meant to split
  env $(envOf "$1") NIX_CONFIG="$(cfgOf "$1")" XDG_CACHE_HOME="$W/cache-$1" "$(binOf "$1")/nix" \
    --store "$W/root-$1" eval --raw --impure --expr "(builtins.fetchGit { url = \"$R\"; }).outPath"; }

case " $SECTIONS " in *" entry "*)
echo "== first entry of the whole tree, fresh store and fetcher cache each trial"
for trial in 1 2 3; do
  for label in $BINS; do
    fresh "$label"
    printf '%-11s trial %s  first entry (outPath) %s   load %s\n' "$label" "$trial" "$(t ev "$label")" "$(cut -d' ' -f1 /proc/loadavg)"
  done
done;; esac

case " $SECTIONS " in *" split "*)
echo "== real/user/sys of one first entry (three trials)"
export TIMEFORMAT='   real %3R user %3U sys %3S'
for trial in 1 2 3; do
  for label in $BINS; do
    fresh "$label-sp"
    echo -n "$label trial $trial"
    # shellcheck disable=SC2046 # the environment list is meant to split
    env $(envOf "$label") NIX_CONFIG="$(cfgOf "$label")" XDG_CACHE_HOME="$W/cache-$label-sp" \
      bash -c "time \"$(binOf "$label")/nix\" --store \"$W/root-$label-sp\" eval --raw --impure --expr '(builtins.fetchGit { url = \"$R\"; }).outPath' > /dev/null 2>/dev/null"
  done
done;; esac

case " $SECTIONS " in *" strace "*)
echo "== syscalls of one first entry (strace -f -c -w), per file = count/$N"
for label in $BINS; do
  fresh "$label-st"
  # shellcheck disable=SC2046 # the environment list is meant to split
  env $(envOf "$label") NIX_CONFIG="$(cfgOf "$label")" XDG_CACHE_HOME="$W/cache-$label-st" \
    strace -f -c -w -o "$W/strace-$label.txt" "$(binOf "$label")/nix" --store "$W/root-$label-st" eval --raw --impure \
    --expr "(builtins.fetchGit { url = \"$R\"; }).outPath" > /dev/null 2>"$W/err-st-$label"
  echo "---- $label"; cat "$W/strace-$label.txt"
  echo "---- $label per file (N=$N), calls >= N/2:"
  awk -v n="$N" '/^ *[0-9]/ { c=$4+0; name=$NF; if (c >= n/2) printf "  %-18s %8d  %6.2f per file\n", name, c, c/n }' "$W/strace-$label.txt"
done;; esac

case " $SECTIONS " in *" rss "*)
echo "== peak resident set of one first entry"
for label in $BINS; do
  fresh "$label-rss"
  # shellcheck disable=SC2046 # the environment list is meant to split
  env $(envOf "$label") NIX_CONFIG="$(cfgOf "$label")" XDG_CACHE_HOME="$W/cache-$label-rss" \
    /usr/bin/time -v "$(binOf "$label")/nix" --store "$W/root-$label-rss" eval --raw --impure \
    --expr "(builtins.fetchGit { url = \"$R\"; }).outPath" > /dev/null 2> "$W/rss-$label.txt"
  printf '%-11s %s\n' "$label" "$(grep -i 'maximum resident' "$W/rss-$label.txt")"
done;; esac

echo "load at end: $(cat /proc/loadavg)"
echo "== done"
