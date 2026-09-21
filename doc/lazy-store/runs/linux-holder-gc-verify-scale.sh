#!/usr/bin/env bash
# doc/lazy-store/06-holder-index.md section 4, *the collector and the verifier
# at scale*, run inside an aarch64-linux derivation by
# linux-measure-holder-gc-verify-scale.nix: `nix-store --gc` and
# `nix-store --verify --check-contents` on a store that already holds M other
# blobs, with the real binaries.
#
# The store is made by one first entry of a 40 000-file tree, and is then
# inflated to look like a large one: for the holder, M synthetic rows in
# `Blobs` attached to that path (so the collection's cascade meets them); for
# the tip, M synthetic files under `.objects/blobs/<xx>/` (so the sweep meets
# them).  Both are "M objects the collector must deal with"; the synthetic
# files are empty, which *understates* the tip's `--check-contents`, and that
# is said rather than hidden.
set -uo pipefail
BINDIRS=${MEASURE_BINDIRS:?}
W=${MEASURE_ROOT:-$TMPDIR/m}; mkdir -p "$W"; R=$W/repo; N=${MEASURE_N:-40000}
M=${MEASURE_M:-1000000}
BINS=${MEASURE_BINS:-"holder tip"}
export HOME=$W/home; mkdir -p "$HOME"

echo "== environment"; uname -a; echo "cpus: $(nproc)"
df -h "$TMPDIR" | tail -1; df -i "$TMPDIR" | tail -1
echo "load at start: $(cat /proc/loadavg)"; echo "N=$N M=$M bins=$BINS"

now() { date +%s.%N; }
t() { local s e rc; s=$(now); "$@" > /dev/null 2> "$W/err"; rc=$?; e=$(now)
  awk -v s="$s" -v e="$e" 'BEGIN { printf "%.2f", e - s }'
  [ $rc -ne 0 ] && printf '(exit %s: %s)' $rc "$(grep -m1 -i error "$W/err" | cut -c1-140)"; echo; }

echo "== repository: $N files in 40 directories"
rm -rf "$R"; mkdir -p "$R"; for d in $(seq 0 39); do mkdir -p "$R/dir$d"; done
for i in $(seq 1 "$N"); do echo "content $i" > "$R/dir$((i % 40))/f$i"; done
( cd "$R" && git init -q . && git add . && git -c user.name=n -c user.email=n@x commit -q -m init )

binOf() { local e; for e in $BINDIRS; do [ "${e%%=*}" = "$1" ] && { echo "${e#*=}"; return; }; done
  echo "no bindir for $1" >&2; exit 1; }
fresh() { local root=$W/root-$1; [ -e "$root" ] && chmod -R u+w "$root" 2>/dev/null; rm -rf "$root" "$W/cache-$1"; }

for label in $BINS; do
  for trial in 1 2 3; do
    fresh "$label"; root=$W/root-$label
    NIX_CONFIG='extra-experimental-features = nix-command flakes' XDG_CACHE_HOME="$W/cache-$label" \
      "$(binOf "$label")/nix" --store "$root" eval --raw --impure \
      --expr "(builtins.fetchGit { url = \"$R\"; }).outPath" > /dev/null 2>&1
    db=$root/nix/var/nix/db/db.sqlite
    inflate=""
    case $label in
      holder)
        # M synthetic rows, attached to the one valid path, in batches.
        s=$(now)
        sqlite3 "$db" <<SQL > /dev/null
pragma journal_mode = wal;
create temp table seq(i integer);
with recursive c(i) as (select 1 union all select i+1 from c where i < $M) insert into seq select i from c;
insert or replace into Blobs (id, exec, ino, dev, path, name, size)
  select lower(hex(randomblob(32))), 0, 2000000+i, 66, (select min(id) from ValidPaths), 'x/'||i, 12 from seq;
SQL
        e=$(now); inflate=$(awk -v s="$s" -v e="$e" 'BEGIN { printf "%.1f", e - s }')
        rows=$(sqlite3 "$db" "select count(*) from Blobs;")
        size=$(stat -c %s "$db")
        extra="rows $rows  db $(( size / 1048576 )) MiB"
        ;;
      tip)
        # M synthetic blob files, which the sweep must judge.
        s=$(now)
        python3 - "$root/nix/store/.objects/blobs" "$M" <<'PY' > /dev/null
import os, sys, hashlib
d, m = sys.argv[1], int(sys.argv[2])
for x in range(256):
    os.makedirs(os.path.join(d, '%02x' % x), exist_ok=True)
for i in range(m):
    h = hashlib.sha256(b'synthetic-%d' % i).hexdigest()
    p = os.path.join(d, h[:2], h)
    fd = os.open(p, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o444)
    os.close(fd)
PY
        e=$(now); inflate=$(awk -v s="$s" -v e="$e" 'BEGIN { printf "%.1f", e - s }')
        files=$(find "$root/nix/store/.objects/blobs" -type f | wc -l)
        extra="object files $files"
        ;;
    esac
    v=$(t env NIX_CONFIG='extra-experimental-features = nix-command flakes' \
          "$(binOf "$label")/nix-store" --store "$root" --verify --check-contents)
    g=$(t env NIX_CONFIG='extra-experimental-features = nix-command flakes' \
          "$(binOf "$label")/nix-store" --store "$root" --gc)
    printf '%-7s trial %d  inflate(%s) %ss  |  verify --check-contents %s  |  gc %s   [%s]  load %s\n' \
      "$label" "$trial" "$M" "$inflate" "$v" "$g" "$extra" "$(cut -d' ' -f1 /proc/loadavg)"
  done
done
echo "load at end: $(cat /proc/loadavg)"
echo "== done"
