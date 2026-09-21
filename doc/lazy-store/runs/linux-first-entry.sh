#!/usr/bin/env bash
# The first entry of a new tree, taken apart: where its time goes
# (doc/lazy-store/05-validation.md section 6, the paragraphs from *The first
# entry of a new tree, taken apart* to *The copy's sibling directories on a
# bounded pool*).  Run inside an aarch64-linux derivation by
# linux-measure-first-entry.nix, which sets every variable below.
#
# MEASURE_SECTIONS chooses what runs:
#   entry    the whole first entry, three trials a label, fresh store and
#            fetcher cache each trial
#   phases   the naming alone (treeHash/narHash), cold and warm, then the copy
#   strace   `strace -f -c -w` of one entry, reported per file
#   profile  callgrind of one entry, inclusive and exclusive
#   split    `bash`'s real/user/sys of one entry
#   perf     `perf stat` cycles and instructions, then `perf record -g`
set -uo pipefail
# BINDIRS: "label=<bindir> label=<bindir> ...".  A label beginning with
# "master" gets master's configuration (auto-optimise-store off, or on for a
# label ending in "-on"); every other gets the branch's.
BINDIRS=${MEASURE_BINDIRS:?}
W=${MEASURE_ROOT:-$TMPDIR/m}; mkdir -p "$W"; R=$W/repo; N=${MEASURE_N:-40000}
SECTIONS=${MEASURE_SECTIONS:-"entry phases strace"}
BINS=${MEASURE_BINS:-"branch master-off master-on"}
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
echo "files: $(find "$R" -type f -not -path '*/.git/*' | wc -l)  head: $(git -C "$R" rev-parse HEAD)"

binOf() { local e; for e in $BINDIRS; do [ "${e%%=*}" = "$1" ] && { echo "${e#*=}"; return; }; done
  echo "no bindir for $1" >&2; exit 1; }
cfgOf() { case $1 in
  master-on) printf 'extra-experimental-features = nix-command flakes\nauto-optimise-store = true\n';;
  master*)   printf 'extra-experimental-features = nix-command flakes\nauto-optimise-store = false\n';;
  *)         printf 'extra-experimental-features = nix-command flakes\n';; esac; }
fresh() { local root=$W/root-$1; [ -e "$root" ] && chmod -R u+w "$root" 2>/dev/null; rm -rf "$root" "$W/cache-$1"; }
ev() { # <tag> <bin> <cfg> <attr>
  NIX_CONFIG="$3" XDG_CACHE_HOME="$W/cache-$1" "$2/nix" --store "$W/root-$1" eval --raw --impure \
    --expr "(builtins.fetchGit { url = \"$R\"; }).$4"; }

case " $SECTIONS " in *" entry "*)
echo "== first entry of the whole tree, fresh store and fetcher cache each trial (three trials)"
for label in $BINS; do
  line=""; for trial in 1 2 3; do fresh "$label"; line="$line $(t ev "$label" "$(binOf "$label")" "$(cfgOf "$label")" outPath)"; done
  printf '%-11s first entry (outPath)%s   load %s\n' "$label" "$line" "$(cut -d' ' -f1 /proc/loadavg)"
done;; esac

case " $SECTIONS " in *" phases "*)
echo "== the first entry in two phases (three trials): naming, then the copy with the memo warm"
for label in $BINS; do
  case $label in master*) attr=narHash;; *) attr=treeHash;; esac
  n1=""; n2=""; c=""
  for trial in 1 2 3; do
    fresh "$label-ph"
    n1="$n1 $(t ev "$label-ph" "$(binOf "$label")" "$(cfgOf "$label")" "$attr")"
    n2="$n2 $(t ev "$label-ph" "$(binOf "$label")" "$(cfgOf "$label")" "$attr")"
    c="$c $(t ev "$label-ph" "$(binOf "$label")" "$(cfgOf "$label")" outPath)"
  done
  printf '%-11s naming(%s) cold%s   warm%s   then copy%s\n' "$label" "$attr" "$n1" "$n2" "$c"
done;; esac

case " $SECTIONS " in *" strace "*)
echo "== syscalls of one first entry (strace -f -c -w), per file = count/$N"
for label in $BINS; do
  fresh "$label-st"
  NIX_CONFIG="$(cfgOf "$label")" XDG_CACHE_HOME="$W/cache-$label-st" \
    strace -f -c -w -o "$W/strace-$label.txt" "$(binOf "$label")/nix" --store "$W/root-$label-st" eval --raw --impure \
    --expr "(builtins.fetchGit { url = \"$R\"; }).outPath" > /dev/null 2>"$W/err-st-$label"
  echo "---- $label"; cat "$W/strace-$label.txt"
  echo "---- $label per file (N=$N), calls >= N/2:"
  awk -v n="$N" '/^ *[0-9]/ { c=$4+0; name=$NF; if (c >= n/2) printf "  %-18s %8d  %6.2f per file\n", name, c, c/n }' "$W/strace-$label.txt"
done;; esac

case " $SECTIONS " in *" profile "*)
echo "== callgrind of one first entry"
for label in $BINS; do
  fresh "$label-cg"
  NIX_CONFIG="$(cfgOf "$label")" XDG_CACHE_HOME="$W/cache-$label-cg" \
    valgrind --tool=callgrind --callgrind-out-file="$W/cg-$label.out" --fair-sched=yes --trace-children=yes \
    "$(binOf "$label")/nix" --store "$W/root-$label-cg" eval --raw --impure \
    --expr "(builtins.fetchGit { url = \"$R\"; }).outPath" > /dev/null 2>"$W/err-cg-$label"
  echo "---- callgrind $label inclusive"
  callgrind_annotate --threshold=99.5 --inclusive=yes "$W/cg-$label.out" > "$W/cg-$label-incl.txt" 2>&1
  sed -n '1,120p' "$W/cg-$label-incl.txt"
  echo "---- callgrind $label exclusive"
  callgrind_annotate --threshold=90 "$W/cg-$label.out" > "$W/cg-$label-excl.txt" 2>&1
  sed -n '1,120p' "$W/cg-$label-excl.txt"
done;; esac

echo "load at end: $(cat /proc/loadavg)"
echo "== done"

case " $SECTIONS " in *" split "*)
echo "== real/user/sys of one first entry (three trials)"
export TIMEFORMAT='   real %3R user %3U sys %3S'
for label in $BINS; do
  for trial in 1 2 3; do
    fresh "$label-sp"
    echo -n "$label trial $trial"
    NIX_CONFIG="$(cfgOf "$label")" XDG_CACHE_HOME="$W/cache-$label-sp" \
      bash -c "time \"$(binOf "$label")/nix\" --store \"$W/root-$label-sp\" eval --raw --impure --expr '(builtins.fetchGit { url = \"$R\"; }).outPath' > /dev/null 2>/dev/null"
  done
done;; esac

case " $SECTIONS " in *" perf "*)
echo "== cycles and instructions of one first entry (perf stat, user)"
for label in $BINS; do
  fresh "$label-pf"
  NIX_CONFIG="$(cfgOf "$label")" XDG_CACHE_HOME="$W/cache-$label-pf" \
    perf stat -e cycles:u,instructions:u \
      "$(binOf "$label")/nix" --store "$W/root-$label-pf" eval --raw --impure \
      --expr "(builtins.fetchGit { url = \"$R\"; }).outPath" > /dev/null 2> "$W/pf-$label.txt"
  echo "---- $label"; cat "$W/pf-$label.txt"
done
echo "== where the cycles go (perf record -g, inclusive then flat)"
for label in $BINS; do
  fresh "$label-pr"
  NIX_CONFIG="$(cfgOf "$label")" XDG_CACHE_HOME="$W/cache-$label-pr" \
    perf record -q -g --call-graph=dwarf,8192 -F 999 -o "$W/perf-$label.data" \
      "$(binOf "$label")/nix" --store "$W/root-$label-pr" eval --raw --impure \
      --expr "(builtins.fetchGit { url = \"$R\"; }).outPath" > /dev/null 2> "$W/pr-$label.err"
  echo "---- $label inclusive (children), >= 1%"
  perf report -i "$W/perf-$label.data" --stdio --children --percent-limit 1 -g none 2>/dev/null
  echo "---- $label flat (self), >= 0.4%"
  perf report -i "$W/perf-$label.data" --stdio --no-children --percent-limit 0.4 -g none 2>/dev/null
  for sym in "operator new\[\]" "base16" "BtreeIndexMoveto" "insert_and_rebalance" "encodeAttrs"; do
    echo "---- $label callers of $sym"
    perf report -i "$W/perf-$label.data" --stdio --no-children -g folded,0.5,caller --symbol-filter="$sym" --percent-limit 0.4 2>/dev/null
  done
done;; esac
