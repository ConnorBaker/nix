#!/usr/bin/env bash
# macOS: the whole first entry and the edited copy, with and without the holder
# prototype, on top of the object-database read (doc/lazy-store/06-holder-index.md
# section 4, *macOS, with the object-database read landed*, and *The edited copy*).
#   MAC_BASE  a directory holding the nix built from the branch tip plus "a
#             clean file's bytes are read from the object database"
#   MAC_CAND  the same plus the holder prototype
# A clean, `git gc --aggressive`'d checkout, so the object-database read
# applies.  Three trials, binaries alternated, `real`/`user`/`sys` -- `real` on
# this machine is noisy (the endpoint-security agent), `sys` is the column that
# moves and `user` is the control.  There is no derivation: the measurement is
# of this machine's filesystem and its agent.
#
#   MAC_BASE=<bindir> MAC_CAND=<bindir> [MAC_N=4000] [MAC_ROOT=<dir>] \
#     bash macos-holder-first-entry.sh
set -uo pipefail
BASE=${MAC_BASE:?MAC_BASE must name a directory holding the base nix}
CAND=${MAC_CAND:?MAC_CAND must name a directory holding the candidate nix}
N=${MAC_N:-4000}
W=${MAC_ROOT:-${TMPDIR:-/tmp}/lazy-store-macos-holder-first-entry}
R=$W/repo-$N
mkdir -p "$W"; export HOME=$W/home; mkdir -p "$HOME"
echo "== N=$N  $(uname -sr)  load: $(uptime | sed 's/.*averages: //')"
echo "base: $BASE/nix"
echo "cand: $CAND/nix"

if [ ! -d "$R/.git" ]; then
  rm -rf "$R"; mkdir -p "$R"; for d in $(seq 0 39); do mkdir -p "$R/dir$d"; done
  for i in $(seq 1 "$N"); do echo "content $i" > "$R/dir$((i % 40))/f$i"; done
  ( cd "$R" && git init -q . && git add . && git -c user.name=n -c user.email=n@x commit -q -m init \
      && git gc -q --aggressive --prune=now )
fi
( cd "$R" && git checkout -q -- . )
echo "files: $(find "$R" -type f -not -path '*/.git/*' | wc -l | tr -d ' ')  loose objects: $(find "$R/.git/objects" -type f -not -path '*pack*' | wc -l | tr -d ' ')  packs: $(find "$R/.git/objects/pack" -name '*.pack' | wc -l | tr -d ' ')"

binOf()   { case $1 in cand*) echo "$CAND";; *) echo "$BASE";; esac; }
fresh()   { local root=$W/root-$1; [ -e "$root" ] && chmod -R u+w "$root" 2>/dev/null; rm -rf "$root" "$W/cache-$1"; }
ev() { # <label> <attr>
  TIMEFORMAT='%3R %3U %3S'
  { time NIX_CONFIG='extra-experimental-features = nix-command flakes' \
      XDG_CACHE_HOME="$W/cache-$1" NIX_HOLDER_PROBE=point \
      "$(binOf "$1")/nix" --store "$W/root-$1" eval --raw --impure \
      --expr "(builtins.fetchGit { url = \"$R\"; }).$2" > /dev/null 2> "$W/err-$1" ; } 2>&1
}

echo "== whole first entry, fresh store and fetcher cache each trial (real user sys)"
for trial in 1 2 3; do
  for label in base cand; do
    fresh "$label"
    printf '%-5s trial %d  first entry  %s   load %s\n' "$label" "$trial" "$(ev "$label" outPath)" \
      "$(uptime | sed 's/.*averages: //')"
  done
done

echo "== the copy of an edited checkout, one file changed (real user sys)"
for label in base cand; do
  fresh "$label-e"
  printf '%-5s prime        %s\n' "$label" "$(ev "$label-e" outPath)"
done
for edit in 1 2 3; do
  echo "edited $edit $(date +%s%N)" > "$R/dir7/f7"
  for label in base cand; do
    printf '%-5s edit %d copy  %s   load %s\n' "$label" "$edit" "$(ev "$label-e" outPath)" \
      "$(uptime | sed 's/.*averages: //')"
  done
done
( cd "$R" && git checkout -q -- . )

echo "== the store afterwards"
for label in base cand; do
  root=$W/root-$label
  printf '%-5s files %s  object files %s\n' "$label" \
    "$(find "$root/nix/store" -type f 2>/dev/null | wc -l | tr -d ' ')" \
    "$(find "$root/nix/store/.objects" -type f 2>/dev/null | wc -l | tr -d ' ')"
done
echo "== done, load: $(uptime | sed 's/.*averages: //')"
