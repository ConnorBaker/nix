#!/usr/bin/env bash
# macos-copy-from-objects.sh <before-bin> <after-bin>: the edited-checkout copy on this Mac, once
# per N, before and after the copy was made from the store's own objects.  Direction only
# (doc/lazy-store/05-validation.md section 5: macOS timings are dominated by the machine's per-open
# cost).  WORK names the working directory; there is no derivation, the measurement being of this
# machine.  The Linux counterpart is linux-copy-from-objects.sh beside this file.
set -uo pipefail
BEFORE=${1:?}; AFTER=${2:?}
W=${WORK:?}; mkdir -p "$W"; export HOME=$W/home; mkdir -p "$HOME"
now() { perl -MTime::HiRes=time -e 'printf "%.3f\n", time'; }
t() { local s e rc; s=$(now); "$@" > /dev/null 2> "$W/err"; rc=$?; e=$(now)
  perl -e 'printf "%.2f", $ARGV[1]-$ARGV[0]' "$s" "$e"
  [ $rc -ne 0 ] && printf '(exit %s: %s)' $rc "$(grep -m1 -i error "$W/err" | cut -c1-160)"; echo; }
CFG=$'extra-experimental-features = nix-command flakes'
echo "== $(uname -a)"; echo "before: $BEFORE $("$BEFORE/nix" --version)"; echo "after: $AFTER $("$AFTER/nix" --version)"
run() { # <label> <bindir> <repo> <N>
  local label=$1 bin=$2 R=$3 N=$4 root cache
  root=$W/root-$label-$N; cache=$W/cache-$label-$N
  [ -e "$root" ] && chmod -R u+w "$root" 2>/dev/null; rm -rf "$root" "$cache"
  nx() { NIX_CONFIG="$CFG" XDG_CACHE_HOME=$cache "$bin/nix" --store "$root" "$@"; }
  ev() { nx eval --raw --impure --expr "(builtins.fetchGit { url = \"$R\"; }).$1"; }
  echo "== $label N=$N"; echo "load: $(uptime)"
  ( cd "$R" && git checkout -q -- dir7 )
  printf '%-7s baseline %s\n' "$label" "$(t nx eval --raw --impure --expr '"x"')"
  printf '%-7s prime full (.outPath, clean) %s\n' "$label" "$(t ev outPath)"
  echo "copy $label 1" > "$R/dir7/f447"
  printf '%-7s edit name-only (.treeHash) %s\n' "$label" "$(t ev treeHash)"
  printf '%-7s edit full (.outPath) %s\n' "$label" "$(t ev outPath)"
  printf '%-7s edit hot (.outPath again) %s\n' "$label" "$(t ev outPath)"
  local p; p=$(ev outPath 2>/dev/null); local real=$root$p
  local SRC=$W/src-$label-$N; rm -rf "$SRC"; cp -R "$R" "$SRC"; rm -rf "$SRC/.git"
  nx nar pack "$SRC" > "$W/src-$label-$N.nar" 2>/dev/null; nx nar pack "$real" > "$W/path-$label-$N.nar" 2>/dev/null
  if cmp -s "$W/src-$label-$N.nar" "$W/path-$label-$N.nar"; then echo "$label N=$N: copied path dumps to the checkout's bytes: yes"; else echo "$label N=$N: DUMP DIFFERS ($real)"; fi
  echo "$label N=$N: regular files under the path with link count >= 2: $(find "$real" -type f -links +1 | wc -l | tr -d ' ') of $(find "$real" -type f | wc -l | tr -d ' ')"
  ( cd "$R" && git checkout -q -- dir7 )
  echo "load: $(uptime)"
}
for N in 4000 40000; do
  R=$W/repo$N; rm -rf "$R"; mkdir -p "$R"; for d in $(seq 0 39); do mkdir -p "$R/dir$d"; done
  for i in $(seq 1 "$N"); do echo "content $i" > "$R/dir$((i % 40))/f$i"; done
  ( cd "$R" && git init -q . && git add . && git -c user.name=n -c user.email=n@x commit -q -m init )
  echo "== repository $N: files $(find "$R" -type f -not -path '*/.git/*' | wc -l | tr -d ' ')"
  run before "$BEFORE" "$R" "$N"
  run after  "$AFTER"  "$R" "$N"
done
echo "== done $(date)"
