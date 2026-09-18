#!/usr/bin/env bash
# timing-object-store.sh <branch-bin> <master-bin>
# Linux: the cost property of 01-specification.md sections 9.9/9.10 (the tables in 05-validation.md section 6),
# run inside an aarch64-linux derivation by linux-measure-object-store.nix.
#   copy:    `nix eval --raw` of fetchGit's outPath on a 4000-file repository, a fresh store root primed with the
#            clean commit, then three one-file edits (dirty checkout), each timed.
#   tarball: two tarballs of the tree differing in one file, fetchTarball of each into a fresh store root, fresh
#            fetcher cache per trial; three trials.
# master runs twice: auto-optimise-store = false (its default) and = true (the "shared" configuration); the branch
# has no setting (the object store is unconditional).
set -uo pipefail
BRANCH=${1:?}; MASTER=${2:?}
W=${MEASURE_ROOT:-$TMPDIR/m}; mkdir -p "$W"; R=$W/repo; N=${MEASURE_N:-4000}
export HOME=$W/home; mkdir -p "$HOME"

echo "== environment"
uname -a
echo "cpus: $(nproc)"
echo "tmpdir fs: $(stat -f -c %T "$TMPDIR")  ($TMPDIR)"
df -h "$TMPDIR" | tail -1
echo "load: $(cat /proc/loadavg)"
echo "branch: $BRANCH  $("$BRANCH/nix" --version 2>&1 | head -1)"
echo "master: $MASTER  $("$MASTER/nix" --version 2>&1 | head -1)"
echo "git: $(git --version)"

now() { date +%s.%N; }
t() { local s e rc; s=$(now); "$@" > /dev/null 2> "$W/err"; rc=$?; e=$(now)
  awk -v s="$s" -v e="$e" 'BEGIN { printf "%.2f", e - s }'
  [ $rc -ne 0 ] && printf '(exit %s: %s)' $rc "$(grep -m1 -i error "$W/err" | cut -c1-140)"; echo; }

echo "== repository: $N files in 40 directories"
rm -rf "$R"; mkdir -p "$R"; for d in $(seq 0 39); do mkdir -p "$R/dir$d"; done
for i in $(seq 1 "$N"); do echo "content $i" > "$R/dir$((i % 40))/f$i"; done
( cd "$R" && git init -q . && git add . && git -c user.name=n -c user.email=n@x commit -q -m init )
echo "files: $(find "$R" -type f -not -path '*/.git/*' | wc -l)  head: $(git -C "$R" rev-parse HEAD)"

CFG_BRANCH=$'extra-experimental-features = nix-command flakes'
CFG_MASTER_OFF=$'extra-experimental-features = nix-command flakes\nauto-optimise-store = false'
CFG_MASTER_ON=$'extra-experimental-features = nix-command flakes\nauto-optimise-store = true'

# ---- startup baseline: nix eval of a literal against a fresh store root -------------------------------------
echo "== baseline: nix eval --raw of a string literal, fresh store root (process start + store open)"
for pair in "branch|$BRANCH|$CFG_BRANCH" "master-off|$MASTER|$CFG_MASTER_OFF"; do
  label=${pair%%|*}; rest=${pair#*|}; bin=${rest%%|*}; cfg=${rest#*|}
  root=$W/base-root-$label; rm -rf "$root"
  base() { NIX_CONFIG="$cfg" XDG_CACHE_HOME=$W/cache-base-$label "$bin/nix" --store "$root" eval --raw --impure --expr '"x"'; }
  printf '%-11s baseline %s %s %s\n' "$label" "$(t base)" "$(t base)" "$(t base)"
done

# ---- copy of a one-file edit --------------------------------------------------------------------------------
copy_run() { # <label> <bindir> <cfg>
  local label=$1 bin=$2 cfg=$3 trial root; root=$W/copy-root-$label
  [ -e "$root" ] && chmod -R u+w "$root" 2>/dev/null; rm -rf "$root"; rm -rf "$W/cache-copy-$label"
  ( cd "$R" && git checkout -q -- . )
  # shellcheck disable=SC2329 # ev is invoked indirectly: `t ev` below passes it to the timer
  ev() { NIX_CONFIG="$cfg" XDG_CACHE_HOME=$W/cache-copy-$label "$bin/nix" --store "$root" eval --raw --impure \
           --expr "(builtins.fetchGit { url = \"$R\"; }).outPath"; }
  echo "load: $(cat /proc/loadavg)"
  printf '%-11s prime (whole tree, clean commit) %s\n' "$label" "$(t ev)"
  for trial in 1 2 3; do
    echo "copy $label $trial" > "$R/dir7/f$((407 + 40 * trial))"
    printf '%-11s edit %s (one file changed, dirty checkout) %s\n' "$label" $trial "$(t ev)"
  done
  ( cd "$R" && git checkout -q -- . )
}
echo "== copy of a one-file edit: nix eval --raw of fetchGit's outPath (the door copies the tree)"
copy_run branch     "$BRANCH" "$CFG_BRANCH"
copy_run master-off "$MASTER" "$CFG_MASTER_OFF"
copy_run master-on  "$MASTER" "$CFG_MASTER_ON"
# second round, reversed order, so that order and drift are visible
copy_run master-on  "$MASTER" "$CFG_MASTER_ON"
copy_run branch     "$BRANCH" "$CFG_BRANCH"

# ---- a tarball fetched twice, the second differing in one file ---------------------------------------------
T=$W/tarballs; mkdir -p "$T"
rm -rf "$T/src"; cp -R "$R" "$T/src"; rm -rf "$T/src/.git"
tar -C "$T" -cf "$T/one.tar" src
echo "changed $(date)" > "$T/src/dir7/f447"; tar -C "$T" -cf "$T/two.tar" src
echo "tarballs: $(stat -c %s "$T/one.tar") $(stat -c %s "$T/two.tar") bytes"
tar_run() { # <label> <bindir> <cfg>
  local label=$1 bin=$2 cfg=$3 trial root
  ev() { NIX_CONFIG="$cfg" XDG_CACHE_HOME=$T/cache-$label "$bin/nix" --store "$1" eval --raw --impure \
           --expr "(builtins.fetchTarball \"file://$T/$2.tar\")"; }
  echo "load: $(cat /proc/loadavg)"
  for trial in 1 2 3; do
    root=$T/root-$label-$trial; [ -e "$root" ] && chmod -R u+w "$root" 2>/dev/null; rm -rf "$root"; rm -rf "$T/cache-$label"
    printf '%-11s trial %s: first tarball %s   ' "$label" $trial "$(t ev "$root" one)"
    printf 'second (one file changed) %s\n' "$(t ev "$root" two)"
  done
}
echo "== a tarball fetched twice into a fresh store, the second differing in one file (fresh cache per trial)"
tar_run branch     "$BRANCH" "$CFG_BRANCH"
tar_run master-off "$MASTER" "$CFG_MASTER_OFF"
tar_run master-on  "$MASTER" "$CFG_MASTER_ON"
tar_run branch     "$BRANCH" "$CFG_BRANCH"

# ---- what the store holds, one look -------------------------------------------------------------------------
echo "== store shape after the tarball trials (files by link count)"
for label in branch master-off master-on; do
  root=$T/root-$label-3
  echo "$label: regular files $(find "$root/nix/store" -type f 2>/dev/null | wc -l), of which link count >= 2: $(find "$root/nix/store" -type f -links +1 2>/dev/null | wc -l)"
done
echo "load: $(cat /proc/loadavg)"
echo "== done $(date)"
