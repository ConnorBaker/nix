#!/usr/bin/env bash
# copy-from-objects.sh <before-bin> <after-bin>: the edited-checkout copy, the branch before and
# after the copy was made from the store's own objects, N = 4000 and 40000, three edits each, plus
# one edit under strace -f -c -w for the syscall counts per unchanged file.  Run inside an
# aarch64-linux derivation by linux-measure-copy-from-objects.nix, which sets MEASURE_ROOT and OUT.
# doc/lazy-store/05-validation.md section 6, *The copy from the store's own objects on Linux*.
set -uo pipefail
BEFORE=${1:?}; AFTER=${2:?}
W=${MEASURE_ROOT:-$TMPDIR/m}; mkdir -p "$W"; export HOME=$W/home; mkdir -p "$HOME"
OUT=${OUT:?}
echo "== environment"; uname -a; echo "cpus: $(nproc)"; echo "tmpdir fs: $(stat -f -c %T "$TMPDIR") ($TMPDIR)"; df -h "$TMPDIR" | tail -1
echo "load: $(cat /proc/loadavg)"; echo "before: $BEFORE $("$BEFORE/nix" --version 2>&1 | head -1)"; echo "after: $AFTER $("$AFTER/nix" --version 2>&1 | head -1)"
now() { date +%s.%N; }
t() { local s e rc; s=$(now); "$@" > /dev/null 2> "$W/err"; rc=$?; e=$(now)
  awk -v s="$s" -v e="$e" 'BEGIN { printf "%.2f", e - s }'
  [ $rc -ne 0 ] && printf '(exit %s: %s)' $rc "$(grep -m1 -i error "$W/err" | cut -c1-140)"; echo; }
CFG=$'extra-experimental-features = nix-command flakes'

run() { # <label> <bindir> <repo> <N>
  local label=$1 bin=$2 R=$3 N=$4 root cache trial
  root=$W/root-$label-$N; cache=$W/cache-$label-$N
  [ -e "$root" ] && chmod -R u+w "$root" 2>/dev/null; rm -rf "$root" "$cache"
  nx() { NIX_CONFIG="$CFG" XDG_CACHE_HOME=$cache "$bin/nix" --store "$root" "$@"; }
  ev() { nx eval --raw --impure --expr "(builtins.fetchGit { url = \"$R\"; }).$1"; }
  echo "== $label N=$N"; echo "load: $(cat /proc/loadavg)"
  ( cd "$R" && git checkout -q -- dir7 )
  printf '%-7s baseline %s %s\n' "$label" "$(t nx eval --raw --impure --expr '"x"')" "$(t nx eval --raw --impure --expr '"x"')"
  printf '%-7s prime name-only (.treeHash, clean, cold memo) %s\n' "$label" "$(t ev treeHash)"
  printf '%-7s prime full (.outPath, clean) %s\n' "$label" "$(t ev outPath)"
  for trial in 1 2 3; do
    echo "copy $label $trial" > "$R/dir7/f$((407 + 40 * trial))"
    printf '%-7s edit %s name-only (.treeHash) %s\n' "$label" $trial "$(t ev treeHash)"
    printf '%-7s edit %s full (.outPath) %s\n' "$label" $trial "$(t ev outPath)"
    printf '%-7s edit %s hot (.outPath again) %s\n' "$label" $trial "$(t ev outPath)"
  done
  echo "copy $label 4" > "$R/dir7/f567"; ev treeHash > /dev/null 2>&1
  printf '%-7s edit 4 full under strace -f -c -w %s  (summary: strace-%s-%s.txt)\n' "$label" "$(t strace -f -c -w -o "$OUT/strace-$label-$N.txt" env NIX_CONFIG="$CFG" XDG_CACHE_HOME="$cache" "$bin/nix" --store "$root" eval --raw --impure --expr "(builtins.fetchGit { url = \"$R\"; }).outPath")" "$label" "$N"
  # the copied path dumps to the checkout's bytes
  # the copied path (under the chroot store root) dumps to the checkout's bytes
  local p; p=$(ev outPath 2>/dev/null); local real=$root$p
  local SRC=$W/src-$label-$N; rm -rf "$SRC"; cp -R "$R" "$SRC"; rm -rf "$SRC/.git"
  nx nar pack "$SRC" > "$W/src-$label-$N.nar" 2>/dev/null; nx nar pack "$real" > "$W/path-$label-$N.nar" 2>/dev/null
  if cmp -s "$W/src-$label-$N.nar" "$W/path-$label-$N.nar"; then echo "$label N=$N: copied path dumps to the checkout's bytes: yes"; else echo "$label N=$N: DUMP DIFFERS ($real)"; fi
  echo "$label N=$N: regular files under the path with link count >= 2: $(find "$real" -type f -links +1 | wc -l) of $(find "$real" -type f | wc -l)"
  ( cd "$R" && git checkout -q -- dir7 )
  echo "load: $(cat /proc/loadavg)"
}

for N in 4000 40000; do
  R=$W/repo$N; rm -rf "$R"; mkdir -p "$R"; for d in $(seq 0 39); do mkdir -p "$R/dir$d"; done
  for i in $(seq 1 "$N"); do echo "content $i" > "$R/dir$((i % 40))/f$i"; done
  ( cd "$R" && git init -q . && git add . && git -c user.name=n -c user.email=n@x commit -q -m init )
  echo "== repository $N: files $(find "$R" -type f -not -path '*/.git/*' | wc -l) head $(git -C "$R" rev-parse HEAD)"
  run before "$BEFORE" "$R" "$N"
  run after  "$AFTER"  "$R" "$N"
done
echo "== done $(date)"
