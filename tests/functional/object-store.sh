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
# What this script asserted before and what it asserts now.  A
# regular-file blob used to have a canonical file of the store's own
# naming under `.objects/blobs/<xx>/<id>`, so the assertions were "the
# file is there" and "the path's file has one link more than the paths
# that share it".  A regular-file blob's canonical file is now a store
# path's own -- a *holder* -- and `.objects/blobs` holds the
# symlink-target blobs alone, which no path holds.  So: the file counts
# under `blobs/` are the symlink targets' and the rest are the index's
# rows, read with `sqlite3`; every link count loses the store's own link;
# and where the script named an object file for a regular file it now
# names the holder, which the index gives.  `.objects/trees` is
# unchanged.

source common.sh

needLocalStore "the object store is the local store's directory"
# The index is read straight out of the store's database; `package.nix`
# already carries `sqlite3` for `object-hash.sh`'s planted rows.
requireSqlite3() { type -p sqlite3 > /dev/null || skipTest "sqlite3 is not on PATH"; }
requireSqlite3

clearStore

objects=$NIX_STORE_DIR/.objects
db=$NIX_STATE_DIR/db/db.sqlite
inode() { stat --format=%i "$1"; }
nlink() { stat --format=%h "$1"; }
# The object store's file for an identifier: git's own layout, the
# identifier's first two hexadecimal digits a shard directory
# (doc/lazy-store/01-specification.md section 9.10).  Only trees and the
# symlink-target blobs have one.
objectFile() { echo "$objects/$1/${2:0:2}/$2"; }
# The blobs with files of the store's own naming: the symlink targets.
blobs() { find "$objects/blobs" -mindepth 2 -maxdepth 2 | wc -l | tr -d ' '; }
trees() { find "$objects/trees" -mindepth 2 -maxdepth 2 | wc -l | tr -d ' '; }
# The index (doc/lazy-store/06-holder-index.md section 2): a regular-file
# blob is a row naming the store path that holds it and the name under
# that path ("" for a plain-file root).  A row whose ingestion has no path
# is not yet (or never will be) true, and neither `link` nor this counts
# it.
blobRows() { sqlite3 "$db" "select count(*) from Blobs b join Ingestions i on i.id = b.ingestion where b.exec = ${1:-0} and i.path is not null"; }
# The holder of (identifier, mode): the file the store would link.
holder() {
    sqlite3 "$db" "select i.path || (case when b.name = '' then '' else '/' || b.name end)
                   from Blobs b join Ingestions i on i.id = b.ingestion
                   where lower(hex(b.id)) = '$1' and b.exec = ${2:-0} and i.path is not null"
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
[[ $(blobs) == 1 ]]                         # the symlink's "b" alone has a file
[[ $(find "$objects/blobs-x" -mindepth 2 -maxdepth 2 | wc -l | tr -d ' ') == 0 ]]
[[ $(blobRows 0) == 2 ]]                    # "one" and "two", held by the path's files
[[ $(blobRows 1) == 1 ]]                    # "one" under the executable bit
# The blobs are named by git's identifiers of the source's bytes, the
# symlink's by its target's.  The shell computation agrees with Nix's git
# hashing once, so the two oracles are tied.  A regular file's blob is
# named by the index, which gives the holder; the symlink's target has a
# file of the store's own naming.
[[ $(gitBlobId "$src/a") == "$(nix hash path --mode git --algo sha256 --base16 "$src/a")" ]]
[[ $(holder "$(gitBlobId "$src/a")") == "$p1/a" ]]
[[ $(holder "$(gitBlobId "$src/d/b")") == "$p1/d/b" ]]
[[ -e $(objectFile blobs "$(gitBlobIdOfBytes b)") ]]
[[ $(holder "$(gitBlobId "$src/x")" 1) == "$p1/x" ]]
[[ $(trees) == 3 ]]
[[ $(nlink "$p1/a") == 2 ]]                 # a and d/e/c; the holder is a itself
[[ $(inode "$p1/d/e/c") == $(inode "$p1/a") ]]
[[ $(nlink "$p1/d/b") == 1 ]]
[[ $(objectHash "$p1") == git:sha256:* ]]

# Entering changes nothing the store means: the NAR hash is the source's.
[[ $(narHash "$p1") == "$(nix hash path --mode nar --format sri "$src")" ]]

# The same bytes executable: one blob per mode, another file, never a link
# across the bit.  The bit is the holder's own -- the index's key is
# (identifier, mode), and each mode's row names a file of its own.
[[ $(inode "$p1/x") != $(inode "$p1/a") ]]
[[ $(nlink "$p1/x") == 1 ]]
cmp "$p1/x" "$p1/a"
[[ -x "$p1/x" ]]
[[ ! -x "$p1/a" ]]
[[ $(stat --format=%a "$p1/x") == 555 ]]
[[ $(stat --format=%a "$p1/a") == 444 ]]
for f in "$objects/blobs/"*/*; do [[ $(stat --format=%a "$f") == 444 ]]; done

# The identifiers are git's under SHA-256: a directory's object hash is
# its tree's, and the object store holds that tree.
rootId=$(nix hash path --mode git --algo sha256 --base16 "$p1")
[[ $(objectHash "$p1") == "git:sha256:$rootId" ]]
[[ -e $(objectFile trees "$rootId") ]]

# A changed tree: only the changed file's blob is added; the rest share
# the first tree's inodes, two directories down among them.
echo three > "$src/d/b"
p2=$(nix store add --mode nar "$src")
[[ $(blobs) == 1 ]]                         # still the symlink's target alone
[[ $(blobRows 0) == 3 ]]                    # "one", "two" and the new "three"
[[ $(blobRows 1) == 1 ]]
[[ $(holder "$(gitBlobId "$src/d/b")") == "$p2/d/b" ]]
[[ $(inode "$p2/a") == $(inode "$p1/a") ]]
[[ $(inode "$p2/d/e/c") == $(inode "$p1/d/e/c") ]]
[[ $(inode "$p2/x") == $(inode "$p1/x") ]]
[[ $(nlink "$p2/a") == 4 ]]
[[ $(nlink "$p2/d/b") == 1 ]]
[[ $(cat "$p2/d/b") == three ]]
[[ $(trees) == 5 ]]                         # p2's root and d differ, e is shared

# The collector frees a deleted path's objects and no others: the blobs
# with files of their own by their link count, the trees by reachability
# from the valid paths' object hashes, and a regular-file blob with its
# holder's path -- its row is stale once that path is invalid, and the
# stale-row sweep removes it (schema.sql).  No roots directory: the
# database is the root record.  `nix store delete` alone sweeps nothing
# (gc.cc: only a whole-store collection pays the pass over the object
# store); the objects and the rows go at the collection.
nix-store --add-root "$TEST_ROOT/root1" --indirect -r "$p1" > /dev/null
# `--quiet` takes the new CLI from `info` (stderr a file) to `notice`, the
# level it runs at on a terminal (main.cc), where the note must reach the
# interactive user it is for (before, it was printed at
# `info` and only a redirected stderr showed it).
nix store delete --quiet "$p2" 2> "$TEST_ROOT/delete.log"
# The freed figure counts p2's files whether or not the blob still holds
# them (physical accounting, 01 section 10); the report says when they go.
grepQuiet "reclaimed at the next \`nix-store --gc\`" "$TEST_ROOT/delete.log"
[[ $(blobs) == 1 ]]
[[ $(trees) == 5 ]]
[[ $(blobRows 0) == 3 ]]                    # p2's row is stale, not yet removed
nix-store --gc
[[ ! -e $objects/roots ]]
[[ -e $(objectFile trees "$rootId") ]]
[[ $(blobs) == 1 ]]
[[ $(trees) == 3 ]]
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
[[ $(blobs) == 1 ]]
[[ $(blobRows 0) == 3 ]]                    # "three" written again, now p3's
[[ $(holder "$(gitBlobIdOfBytes $'three\n')") == "$p3/d/b" ]]
[[ $(trees) == 5 ]]
p3Id=$(nix hash path --mode git --algo sha256 --base16 "$p3")
[[ $(objectHash "$p3") == "git:sha256:$p3Id" ]]
[[ -e $(objectFile trees "$p3Id") ]]

# `nix-store --optimise` is the migration of paths an older Nix wrote:
# here there are none, so it links no file, and it removes the legacy
# `.links` table (planted as an older store would have left it: a hard
# link to a file, so removing it frees nothing).  Running it again
# changes nothing.
[[ ! -e $NIX_STORE_DIR/.links ]]
mkdir "$NIX_STORE_DIR/.links"
ln "$p3/a" "$NIX_STORE_DIR/.links/legacy"
[[ $(nlink "$p1/a") == 5 ]]
optimiseLinksNothing
[[ ! -e $NIX_STORE_DIR/.links ]]
[[ $(nlink "$p1/a") == 4 ]]
[[ $(inode "$p3/a") == $(inode "$p1/a") ]]
[[ -e $(objectFile trees "$p3Id") ]]
[[ $(blobs) == 1 ]]
[[ $(blobRows 0) == 3 ]]
[[ $(trees) == 5 ]]
optimiseLinksNothing
[[ ! -e $NIX_STORE_DIR/.links ]]
[[ $(nlink "$p1/a") == 4 ]]
[[ $(blobs) == 1 ]]
[[ $(blobRows 0) == 3 ]]
[[ $(trees) == 5 ]]

# `nix-store --optimise` re-enters a holder the collector forgot
# (doc/lazy-store/06-holder-index.md section 2, *The collector*): the one
# case the holder representation pays for is a holder's path collected
# while another path shares its inode -- the blob is still held, but the
# row that named it went with the path.  The row is forgotten by hand
# here, as the sweep leaves it; `--optimise` walks every valid path, meets
# the file with no row for its blob, and enters it.
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
[[ -e $(objectFile trees "$(nix hash path --mode git --algo sha256 --base16 "$p4")") ]]

# Substitution from a binary cache comes back through the object store.
nix copy --to "file://$TEST_ROOT/cache" "$p1"
rm "$TEST_ROOT/root1"
nix store delete "$p1"
[[ ! -e $p1 ]]
nix copy --from "file://$TEST_ROOT/cache" --no-check-sigs "$p1"
[[ $(objectHash "$p1") == "git:sha256:$rootId" ]]
[[ -e $(objectFile trees "$rootId") ]]
# Law 3's fifth exception, made and then closed
# (doc/lazy-store/06-holder-index.md section 2, *The collector*): p1 held
# the blobs of "one" and "one"-executable, and its deletion left their
# rows naming a path that is gone, so the substitution wrote those files
# afresh instead of linking p3's -- the bytes are in the store twice.
# Within the path the sharing still holds, the copy's own pending set
# standing for the index (law 3 within one path).  `nix-store --optimise`,
# which is the documented repair, links the inodes that have grown apart
# back together.  Before, the blob file outlived the path and the
# substitution linked it straight away.
[[ $(inode "$p1/a") != $(inode "$p3/a") ]]
[[ $(inode "$p1/d/e/c") == $(inode "$p1/a") ]]
[[ $(inode "$p1/x") != $(inode "$p3/x") ]]
NIX_REMOTE="" nix-store --optimise 2> "$TEST_ROOT/optimise-relink.log"
grepQuietInverse "hard-linking 0 files" "$TEST_ROOT/optimise-relink.log"
[[ $(inode "$p1/a") == $(inode "$p3/a") ]]
[[ $(inode "$p1/d/e/c") == $(inode "$p3/a") ]]
[[ $(inode "$p1/x") == $(inode "$p3/x") ]]
optimiseLinksNothing
[[ $(objectHash "$p1") == "git:sha256:$rootId" ]]
[[ -e $(objectFile trees "$rootId") ]]

# Verification checks the objects' bytes against their identifiers and a
# path's files against its object hash.  One corruption of each kind that
# still exists: p1's `d/b` ("two"), which is that blob's *holder* -- a
# valid path's own file, so its corruption is the path's and the verifier
# reports the path and repairs it from a substituter, never unlinking the
# file itself (`08` K2); the symlink's target blob, which has a file of
# the store's own naming and is reported and removed as an object; and
# p1's root tree.  Before, "two" was an object file of the store's naming
# and the first case was the second's.
twoId=$(gitBlobIdOfBytes $'two\n')
bId=$(gitBlobIdOfBytes b)
[[ $(holder "$twoId") == "$p1/d/b" ]]
[[ $(nlink "$p1/d/b") == 1 ]]               # the holder is p1's own file
nix-store --verify --check-contents
corruptFirstByte "$p1/d/b"
[[ $(cat "$p1/d/b") == Xwo ]]               # the corruption is the path's
corruptFirstByte "$(objectFile blobs "$bId")"
corruptFirstByte "$(objectFile trees "$rootId")"
verifyOut=$(expectStderr 1 nix-store --verify --check-contents)
echo "$verifyOut" | grepQuiet "object \".*/blobs/.*/$bId\" was modified"
echo "$verifyOut" | grepQuiet "tree object \".*/trees/.*/$rootId\" was modified"
echo "$verifyOut" | grepQuiet -F "path '$p1' was modified"
[[ -e $(objectFile blobs "$bId") ]]                # reported, not removed, without --repair
[[ -e $(objectFile trees "$rootId") ]]
[[ $(cat "$p1/d/b") == Xwo ]]
nix-store --verify --check-contents --repair \
    --substituters "file://$TEST_ROOT/cache" --no-require-sigs 2> "$TEST_ROOT/repair.log"
grepQuiet "removed object \".*/blobs/.*/$bId\"" "$TEST_ROOT/repair.log"
grepQuiet "removed tree object \".*/trees/.*/$rootId\"" "$TEST_ROOT/repair.log"
nix-store --verify --check-contents
[[ $(cat "$p1/d/b") == two ]]
[[ $(holder "$twoId") == "$p1/d/b" ]]       # the repaired file is the blob's holder
[[ $(nlink "$p1/d/b") == 1 ]]
[[ -e $(objectFile blobs "$bId") ]]
[[ -e $(objectFile trees "$rootId") ]]
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
