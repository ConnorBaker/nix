#!/usr/bin/env bash

# The store's object store (doc/lazy-store/01-specification.md, section
# 9.10; doc/lazy-store/06-holder-index.md) is unconditional: from the
# moment a path is added its regular files are the store's blobs, git
# objects under SHA-256, and a later ingestion of the same bytes is a hard
# link to the file the first one wrote; the same bytes executable and not
# are one blob per mode and two files, never one inode; the identifiers
# are git's; the collector frees a deleted path's objects and no others;
# `nix-store --optimise` is the migration of what an older Nix wrote and
# otherwise links nothing; substitution comes back through the object
# store; verification checks every object's bytes against its identifier,
# and under --repair a corrupt object is removed and its path repaired
# from a substituter.
#
# Regular bytes are held by immutable store-path files. Directory bodies and
# symlink targets are typed, ingestion-owned AuxValues rows; distinct IDs
# count shared values, while each receiving ingestion retains its own copy.

source common.sh

needLocalStore "the holder and auxiliary indexes are the local store's"
# The index is read straight out of the store's database; `package.nix`
# already carries `sqlite3` for `object-hash.sh`'s planted rows.
requireSqlite3() { type -p sqlite3 > /dev/null || skipTest "sqlite3 is not on PATH"; }
requireSqlite3

clearStore

db=$NIX_STATE_DIR/db/db.sqlite
inode() { stat --format=%i "$1"; }
nlink() { stat --format=%h "$1"; }
# Count distinct typed values, independently of chunks and donor copies.
auxCount() { sqlite3 "$db" "select count(distinct id) from AuxValues where kind = $1"; }
hasAuxiliary() { sqlite3 "$db" "select exists(select 1 from AuxValues where kind = $1 and id = X'$2')"; }
# The index: a candidate is a row naming the store path that holds it and the name under
# that path ("" for a plain-file root).  A row whose ingestion has no path
# is not yet (or never will be) visible. Count named candidates, including
# stale rows until collection removes them.
blobRows() { sqlite3 "$db" "select count(*) from Blobs b join Ingestions i on i.id = b.ingestion where b.exec = ${1:-0} and i.path is not null"; }
# The holder of (identifier, mode): the file the store would link.
holder() {
    sqlite3 "$db" "select i.path || (case when b.name = '' then '' else '/' || b.name end)
                   from Blobs b join Ingestions i on i.id = b.ingestion
                   join ValidPaths v on v.path = i.path
                   where lower(hex(b.id)) = '$1' and b.exec = ${2:-0}
                   order by b.dev, b.ino, b.ingestion limit 1"
}
# The store's root record for a path is the database's object hash.
objectHash() { nix path-info --json --json-format 4 "$1" | jq -r '.info | to_entries[0].value.objectHash'; }
narHash() { nix path-info --json --json-format 2 "$1" | jq -r '.info | to_entries[0].value.narHash'; }
optimiseLinksNothing() {
    nix-store --optimise 2> "$TEST_ROOT/optimise.log"
    grepQuiet "hard-linking 0 files; 0 files entered as new objects; 0 paths migrated to the object hash" "$TEST_ROOT/optimise.log"
}
# Git's blob identifier under SHA-256, computed outside Nix: the hash of
# `blob <size>\0` followed by the bytes (GNU coreutils' sha256sum).
gitBlobIdOfBytes() { { printf 'blob %d\0' "${#1}"; printf '%s' "$1"; } | sha256sum | cut -c1-64; }
gitBlobId() { { printf 'blob %d\0' "$(stat --format=%s "$1")"; cat "$1"; } | sha256sum | cut -c1-64; }
# Overwrite an object file's first byte in place (same length).  One
# statement: the mode is put back whether or not the write succeeded, and a
# failed write fails the test rather than leaving a writable object behind.
corruptFirstByte() {
    local status=0
    chmod u+w "$1" && { printf 'X' | dd of="$1" bs=1 count=1 conv=notrunc status=none; } || status=$?
    chmod u-w "$1"
    touch -d @1 "$1"
    return "$status"
}

src=$TEST_ROOT/src
rm -rf "$src"
mkdir -p "$src/d/e"
echo one > "$src/a"
echo two > "$src/d/b"
echo one > "$src/d/e/c"
echo one > "$src/x"
chmod +x "$src/x"
ln -s b "$src/d/l"

# A tree added: its files are the object store's as soon as the add
# returns, at the root and two directories below it.
p1=$(nix store add --mode nar "$src")
[[ $(auxCount 40960) == 1 ]]               # the symlink's "b" alone has an auxiliary value
[[ $(blobRows 0) == 2 ]]                    # "one" and "two", held by the path's files
[[ $(blobRows 1) == 1 ]]                    # "one" under the executable bit
# The blobs are named by git's identifiers of the source's bytes, the
# symlink's by its target's.  The shell computation agrees with Nix's git
# hashing once, so the two oracles are tied.  A regular file's blob is
# named by the index, which gives the holder; the symlink target has its
# own typed auxiliary value.
[[ $(gitBlobId "$src/a") == "$(nix hash path --mode git --algo sha256 --base16 "$src/a")" ]]
[[ $(holder "$(gitBlobId "$src/a")") == "$p1/a" ]]
[[ $(holder "$(gitBlobId "$src/d/b")") == "$p1/d/b" ]]
[[ $(hasAuxiliary 40960 "$(gitBlobIdOfBytes b)") == 1 ]]
[[ $(holder "$(gitBlobId "$src/x")" 1) == "$p1/x" ]]
[[ $(auxCount 16384) == 3 ]]
[[ $(nlink "$p1/a") == 2 ]]                 # a and d/e/c; the holder is a itself
[[ $(inode "$p1/d/e/c") == $(inode "$p1/a") ]]
[[ $(nlink "$p1/d/b") == 1 ]]
[[ $(objectHash "$p1") == git:sha256:* ]]

# Entering changes nothing the store means: the NAR hash is the source's.
[[ $(narHash "$p1") == "$(nix hash path --mode nar --format sri "$src")" ]]

# The same bytes executable: one blob per mode, another file, never a link
# across the bit.  The bit is the holder's own -- the index's key is
# (identifier, mode, ingestion), and each mode's candidates name files of their own.
[[ $(inode "$p1/x") != $(inode "$p1/a") ]]
[[ $(nlink "$p1/x") == 1 ]]
cmp "$p1/x" "$p1/a"
[[ -x "$p1/x" ]]
[[ ! -x "$p1/a" ]]
[[ $(stat --format=%a "$p1/x") == 555 ]]
[[ $(stat --format=%a "$p1/a") == 444 ]]

# The identifiers are git's under SHA-256: a directory's object hash is
# its tree's, and the object store holds that tree.
rootId=$(nix hash path --mode git --algo sha256 --base16 "$p1")
[[ $(objectHash "$p1") == "git:sha256:$rootId" ]]
[[ $(hasAuxiliary 16384 "$rootId") == 1 ]]

# A changed tree: only the changed file's blob is added; the rest share
# the first tree's inodes, two directories down among them.
echo three > "$src/d/b"
p2=$(nix store add --mode nar "$src")
[[ $(auxCount 40960) == 1 ]]               # still one distinct symlink target
[[ $(blobRows 0) == 4 ]]                    # two candidates from each ingestion
[[ $(blobRows 1) == 2 ]]
[[ $(holder "$(gitBlobId "$src/d/b")") == "$p2/d/b" ]]
[[ $(inode "$p2/a") == $(inode "$p1/a") ]]
[[ $(inode "$p2/d/e/c") == $(inode "$p1/d/e/c") ]]
[[ $(inode "$p2/x") == $(inode "$p1/x") ]]
[[ $(nlink "$p2/a") == 4 ]]
[[ $(nlink "$p2/d/b") == 1 ]]
[[ $(cat "$p2/d/b") == three ]]
[[ $(auxCount 16384) == 5 ]]               # p2's root and d differ, e is shared
# Each ingestion owns all three tree bodies and the symlink target,
# including the unchanged subtree, independently of the other donor.
for path in "$p1" "$p2"; do
    [[ $(sqlite3 "$db" "select count(*) from AuxValues a join Ingestions i on i.id = a.ingestion where i.path = '$path' and a.chunk = 0") == 4 ]]
done

# A whole-store collection removes stale ingestion projections; each live
# path retains its own regular witnesses and auxiliary bodies. Specific
# deletion leaves those rows until collection, without scanning all rows.
nix-store --add-root "$TEST_ROOT/root1" --indirect -r "$p1" > /dev/null
nix store delete --quiet "$p2" 2> "$TEST_ROOT/delete.log"
[[ $(auxCount 40960) == 1 ]]
[[ $(auxCount 16384) == 5 ]]
[[ $(blobRows 0) == 4 ]]                    # p2's candidates are stale, not yet removed
nix-store --gc
[[ $(hasAuxiliary 16384 "$rootId") == 1 ]]
[[ $(auxCount 40960) == 1 ]]
[[ $(auxCount 16384) == 3 ]]
# "three" went with the path that held it; "one" and "two" are p1's own
# files and their rows name p1.
[[ $(blobRows 0) == 2 ]]
[[ $(blobRows 1) == 1 ]]
[[ $(holder "$(gitBlobIdOfBytes $'one\n')") == "$p1/a" ]]
[[ $(nlink "$p1/a") == 2 ]]
[[ $(cat "$p1/a") == one ]]

# Added again after the collection: the path is entered at the add, with
# no setting to turn that on; its tree is in the object store and its
# files share p1's inodes as soon as the add returns.
p3=$(nix store add --mode nar "$src")
[[ $(inode "$p3/a") == $(inode "$p1/a") ]]
[[ $(nlink "$p1/a") == 4 ]]
[[ $(cat "$p3/d/b") == three ]]
[[ $(auxCount 40960) == 1 ]]
[[ $(blobRows 0) == 4 ]]                    # p3 records both its linked and written files
[[ $(blobRows 1) == 2 ]]
[[ $(holder "$(gitBlobIdOfBytes $'three\n')") == "$p3/d/b" ]]
[[ $(auxCount 16384) == 5 ]]
p3Id=$(nix hash path --mode git --algo sha256 --base16 "$p3")
[[ $(objectHash "$p3") == "git:sha256:$p3Id" ]]
[[ $(hasAuxiliary 16384 "$p3Id") == 1 ]]

# `nix-store --optimise` is the migration of paths an older Nix wrote:
# here there are none, so it links no file, and it removes the legacy
# `.links` table (planted as an older store would have left it: a hard
# link to a file, so removing it frees nothing).  Running it again
# changes no file; each completed ingestion replaces that path's candidates.
[[ ! -e $NIX_STORE_DIR/.links ]]
mkdir "$NIX_STORE_DIR/.links"
ln "$p3/a" "$NIX_STORE_DIR/.links/legacy"
[[ $(nlink "$p1/a") == 5 ]]
optimiseLinksNothing
[[ ! -e $NIX_STORE_DIR/.links ]]
[[ $(nlink "$p1/a") == 4 ]]
[[ $(inode "$p3/a") == $(inode "$p1/a") ]]
[[ $(hasAuxiliary 16384 "$p3Id") == 1 ]]
[[ $(auxCount 40960) == 1 ]]
[[ $(blobRows 0) == 4 ]]                    # the same two plain keys in each of two paths
[[ $(blobRows 1) == 2 ]]
[[ $(auxCount 16384) == 5 ]]
optimiseLinksNothing
[[ ! -e $NIX_STORE_DIR/.links ]]
[[ $(nlink "$p1/a") == 4 ]]
[[ $(auxCount 40960) == 1 ]]
[[ $(blobRows 0) == 4 ]]
[[ $(blobRows 1) == 2 ]]
[[ $(auxCount 16384) == 5 ]]

# `nix-store --optimise` reconstructs missing cache entries from valid
# paths. Remove every witness of one blob by hand; the next walk enters
# that file without rewriting its bytes or inode.
threeId=$(gitBlobIdOfBytes $'three\n')
[[ $(holder "$threeId") == "$p3/d/b" ]]
threeInode=$(inode "$p3/d/b")
sqlite3 "$db" "delete from Blobs where lower(hex(id)) = '$threeId' and exec = 0"
[[ -z $(holder "$threeId") ]]
NIX_REMOTE="" nix-store --optimise 2> "$TEST_ROOT/optimise-reentry.log"
[[ $(holder "$threeId") == "$p3/d/b" ]]
[[ $(inode "$p3/d/b") == "$threeInode" ]]   # re-entered, not rewritten
grepQuiet "1 files entered as new objects" "$TEST_ROOT/optimise-reentry.log"
optimiseLinksNothing

# A restore that spills to a temporary directory (the daemon's usual route
# for anything above nar-buffer-size) enters the object store all the same.
echo five > "$src/d/b"
p4=$(nix store add --mode nar --option nar-buffer-size 0 "$src")
[[ $(inode "$p4/a") == $(inode "$p3/a") ]]
[[ $(hasAuxiliary 16384 "$(nix hash path --mode git --algo sha256 --base16 "$p4")") == 1 ]]

# Substitution from a binary cache comes back through the object store.
nix copy --to "file://$TEST_ROOT/cache" "$p1"
rm "$TEST_ROOT/root1"
nix store delete "$p1"
[[ ! -e $p1 ]]
nix copy --from "file://$TEST_ROOT/cache" --no-check-sigs "$p1"
[[ $(objectHash "$p1") == "git:sha256:$rootId" ]]
[[ $(hasAuxiliary 16384 "$rootId") == 1 ]]
# p3 retained its own witnesses when it linked p1's files. Deleting p1
# therefore preserves sharing: substitution links the surviving inode
# immediately, for both executable modes and repeated aliases.
[[ $(inode "$p1/a") == $(inode "$p3/a") ]]
[[ $(inode "$p1/d/e/c") == $(inode "$p1/a") ]]
[[ $(inode "$p1/x") == $(inode "$p3/x") ]]
optimiseLinksNothing
[[ $(inode "$p1/a") == $(inode "$p3/a") ]]
[[ $(inode "$p1/d/e/c") == $(inode "$p3/a") ]]
[[ $(inode "$p1/x") == $(inode "$p3/x") ]]
[[ $(objectHash "$p1") == "git:sha256:$rootId" ]]
[[ $(hasAuxiliary 16384 "$rootId") == 1 ]]

# Verification checks physical bytes and typed auxiliary bodies. Corrupt
# p1's regular holder without unlinking it, and corrupt every incarnation
# of one symlink target and one tree. Reporting preserves the bad rows;
# repair discards them and reconstructs the values from valid paths.
twoId=$(gitBlobIdOfBytes $'two\n')
bId=$(gitBlobIdOfBytes b)
[[ $(holder "$twoId") == "$p1/d/b" ]]
[[ $(nlink "$p1/d/b") == 1 ]]               # the holder is p1's own file
nix-store --verify --check-contents
corruptFirstByte "$p1/d/b"
[[ $(cat "$p1/d/b") == Xwo ]]               # the corruption is the path's
sqlite3 "$db" "update AuxValues set body = cast(X'58' || substr(body, 2) as blob)
    where chunk = 0 and ((kind = 40960 and id = X'$bId') or (kind = 16384 and id = X'$rootId'))"
verifyOut=$(expectStderr 1 nix-store --verify --check-contents)
echo "$verifyOut" | grepQuiet "auxiliary cache body in ingestion .* was modified or has an incomplete chunk sequence"
echo "$verifyOut" | grepQuiet -F "path '$p1' was modified"
# Both corrupted typed keys remain present after reporting.
for key in "40960 $bId" "16384 $rootId"; do
    read -r kind id <<< "$key"
    [[ $(sqlite3 "$db" "select count(*) from AuxValues where kind = $kind and id = X'$id' and chunk = 0 and substr(body, 1, 1) = X'58'") -gt 0 ]]
done
[[ $(cat "$p1/d/b") == Xwo ]]
nix-store --verify --check-contents --repair \
    --substituters "file://$TEST_ROOT/cache" --no-require-sigs 2> "$TEST_ROOT/repair.log"
grepQuiet "auxiliary cache body in ingestion .* was modified or has an incomplete chunk sequence" "$TEST_ROOT/repair.log"
nix-store --verify --check-contents
[[ $(cat "$p1/d/b") == two ]]
[[ $(holder "$twoId") == "$p1/d/b" ]]       # the repaired file is the blob's holder
[[ $(nlink "$p1/d/b") == 1 ]]
[[ $(hasAuxiliary 40960 "$bId") == 1 ]]
[[ $(hasAuxiliary 16384 "$rootId") == 1 ]]
[[ $(objectHash "$p1") == "git:sha256:$rootId" ]]

# The boundaries of a regular file: an empty file and a one-byte executable
# are entered like any other.  The empty blob's identifier is git's for
# the empty blob under SHA-256, sha256("blob 0\0").
src2=$TEST_ROOT/src2
rm -rf "$src2"
mkdir -p "$src2"
: > "$src2/empty"
printf 'x' > "$src2/exe"
chmod +x "$src2/exe"
q1=$(nix store add --mode nar "$src2")
[[ $(nlink "$q1/empty") == 1 ]]
[[ $(nlink "$q1/exe") == 1 ]]
[[ -x "$q1/exe" ]]
[[ $(holder "$(gitBlobIdOfBytes '')") == "$q1/empty" ]]
[[ $(holder "$(gitBlobIdOfBytes x)" 1) == "$q1/exe" ]]
echo other > "$src2/other"
q2=$(nix store add --mode nar "$src2")
[[ $(inode "$q2/empty") == $(inode "$q1/empty") ]]
[[ $(inode "$q2/exe") == $(inode "$q1/exe") ]]
[[ $(nlink "$q1/empty") == 2 ]]

# Both routes to a link: a file the store holds is linked without being
# written when it is under 1 MiB, and written and then replaced by a link
# above that.  Either way the second tree's file is the first's inode.
big=$TEST_ROOT/big
rm -rf "$big"
mkdir -p "$big"
dd if=/dev/zero of="$big/z" bs=1048576 count=2 status=none
b1=$(nix store add --mode nar "$big")
[[ $(nlink "$b1/z") == 1 ]]
[[ $(holder "$(gitBlobId "$big/z")") == "$b1/z" ]]
echo tag > "$big/t"
b2=$(nix store add --mode nar "$big")
[[ $(inode "$b2/z") == $(inode "$b1/z") ]]
[[ $(nlink "$b1/z") == 2 ]]
cmp "$b2/z" "$big/z"

# The collector removes a legacy `.links` directory as well (01 section 10):
# every entry of it is a hard link to a file a store path also links, or a
# dead one an older Nix's collector would have unlinked, so the whole
# directory goes and no data with it -- master freed its dead entries on
# every collection; before this, only `nix-store --optimise` did.
nix-store --add-root "$TEST_ROOT/root-links" --indirect -r "$q2" > /dev/null
mkdir "$NIX_STORE_DIR/.links"
ln "$q2/other" "$NIX_STORE_DIR/.links/legacy"
echo dead > "$NIX_STORE_DIR/.links/dead"
[[ $(nlink "$q2/other") == 2 ]]
nix-store --gc
[[ ! -e $NIX_STORE_DIR/.links ]]
[[ $(cat "$q2/other") == other ]]
[[ $(nlink "$q2/other") == 1 ]]
[[ $(holder "$(gitBlobIdOfBytes $'other\n')") == "$q2/other" ]]
