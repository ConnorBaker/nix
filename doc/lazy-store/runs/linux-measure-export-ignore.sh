#!/usr/bin/env bash
# Driven by linux-measure-export-ignore.nix beside this file, inside an
# aarch64-linux derivation (doc/lazy-store/05-validation.md section 6, *The
# export-ignore filter's lookups* and *The attributes a commit's filter
# consults*).  $src, $beforeNix, $afterNix and $system come from the
# derivation's environment.
set -u

# The derivation's own attributes, named here so that they are declared.
src=${src:?}
beforeNix=${beforeNix:?}
afterNix=${afterNix:?}
system=${system:?}

export HOME=$TMPDIR/home
mkdir -p "$HOME"

echo "== building the repository"
mkdir -p "$TMPDIR/checkout"
cp -r "$src/." "$TMPDIR/checkout/"
chmod -R u+w "$TMPDIR/checkout"
cd "$TMPDIR/checkout" || exit 1
git init -q -b main .
git config user.email a@example.com
git config user.name a
git config gc.auto 0
git add -A
git commit -q -m one
git repack -a -d -q
REV=$(git rev-parse HEAD)
DIRS=$(git ls-tree -r -d --name-only HEAD | wc -l)
GA=$( (git ls-tree -r --name-only HEAD || true) | grep -c '\.gitattributes$' )
GAM=$( (git grep -l -e 'export-ignore' "$REV" -- '*.gitattributes' || true) | wc -l )
echo "rev=$REV  tree directories=$DIRS  .gitattributes files=$GA  of which naming export-ignore=$GAM"

git clone -q --bare "$TMPDIR/checkout" "$TMPDIR/bare.git"
cd "$TMPDIR" || exit 1

run() { # tag binary label expr
  tag=$1; bin=$2; label=$3; expr=$4
  root=$TMPDIR/env-$tag
  rm -rf "$root"
  mkdir -p "$root/store" "$root/var/nix" "$root/var/log/nix" "$root/etc" "$root/cache"
  export NIX_STORE_DIR=$root/store
  export NIX_STATE_DIR=$root/var/nix
  export NIX_LOG_DIR=$root/var/log/nix
  export NIX_CONF_DIR=$root/etc
  export XDG_CACHE_HOME=$root/cache
  export NIX_REMOTE=
  printf 'experimental-features = nix-command flakes\nbuild-users-group =\n' > "$root/etc/nix.conf"

  echo
  echo "---- $label [$tag]"
  t0=$(date +%s.%N)
  "$bin/bin/nix" eval --impure --raw --expr "$expr" > "$root/cold.out" 2> "$root/cold.err"
  rc=$?
  t1=$(date +%s.%N)
  if [ $rc -ne 0 ]; then
    echo "COLD FAILED rc=$rc"
    tail -30 "$root/cold.err"
    return 0
  fi
  echo "cold: $(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}') s -> $(cat "$root/cold.out")"

  walls=""
  for _ in 1 2 3; do
    t0=$(date +%s.%N)
    "$bin/bin/nix" eval --impure --raw --expr "$expr" > /dev/null 2>&1
    t1=$(date +%s.%N)
    walls="$walls $(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')"
  done
  echo "warm wall (3 runs):$walls s"

  strace -f -c -w -o "$root/strace.txt" "$bin/bin/nix" eval --impure --raw --expr "$expr" > /dev/null 2>&1
  echo "strace -f -c -w, one warm run:"
  cat "$root/strace.txt"
}

NPARGS='{ system = "'"$system"'"; }'

for which in before after; do
  case $which in
    before) bin=$beforeNix ;;
    after)  bin=$afterNix ;;
  esac
  echo
  echo "======== $which ($bin)"
  run "$which-checkout" "$bin" 'fetchGit { rev } on a checkout' \
    "(import (builtins.fetchGit { url = \"file://$TMPDIR/checkout\"; rev = \"$REV\"; shallow = true; }) $NPARGS).hello.name"
  run "$which-bare" "$bin" 'fetchGit { rev } on a bare repository' \
    "(import (builtins.fetchGit { url = \"file://$TMPDIR/bare.git\"; rev = \"$REV\"; shallow = true; }) $NPARGS).hello.name"
  run "$which-workdir" "$bin" 'fetchGit on the working directory' \
    "(import (builtins.fetchGit { url = \"$TMPDIR/checkout\"; }) $NPARGS).hello.name"
  run "$which-noei" "$bin" 'fetchGit { rev; exportIgnore = false; } on a checkout' \
    "(import (builtins.fetchGit { url = \"file://$TMPDIR/checkout\"; rev = \"$REV\"; shallow = true; exportIgnore = false; }) $NPARGS).hello.name"
done
echo
echo "== done"
