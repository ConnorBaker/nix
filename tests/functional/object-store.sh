#!/usr/bin/env bash

# The store's object store (doc/lazy-store/01-specification.md, section
# 9.10) is unconditional: from the moment a path is added its regular
# files are hard links into /nix/store/.objects, git objects under
# SHA-256; the same bytes executable and not are one blob and two files,
# never one inode; the identifiers are git's; the collector frees a
# deleted path's objects and no others; `nix-store --optimise` is the
# migration of what an older Nix wrote and otherwise links nothing;
# substitution comes back through the object store; verification checks
# every object's bytes against its identifier, and under --repair a
# corrupt object is removed and its path repaired from a substituter.

source common.sh

needLocalStore "the object store is the local store's directory"

clearStore

objects=$NIX_STORE_DIR/.objects
inode() { stat --format=%i "$1"; }
nlink() { stat --format=%h "$1"; }
blobs() { find "$objects/blobs" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' '; }
trees() { find "$objects/trees" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' '; }
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
[[ $(blobs) == 3 ]]                         # "one", "two", and the symlink's "b"
[[ $(find "$objects/blobs-x" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ') == 1 ]]
# The blob files are named by git's identifiers of the source's bytes,
# the symlink's by its target's.  The shell computation agrees with Nix's
# git hashing once, so the two oracles are tied.
[[ $(gitBlobId "$src/a") == "$(nix hash path --mode git --algo sha256 --base16 "$src/a")" ]]
[[ -e $objects/blobs/$(gitBlobId "$src/a") ]]
[[ -e $objects/blobs/$(gitBlobId "$src/d/b") ]]
[[ -e $objects/blobs/$(gitBlobIdOfBytes b) ]]
[[ -e $objects/blobs-x/$(gitBlobId "$src/x") ]]
[[ $(trees) == 3 ]]
[[ $(nlink "$p1/a") == 3 ]]                 # a, d/e/c, and the blob's file
[[ $(inode "$p1/d/e/c") == $(inode "$p1/a") ]]
[[ $(nlink "$p1/d/b") == 2 ]]
[[ $(objectHash "$p1") == git:sha256:* ]]

# Entering changes nothing the store means: the NAR hash is the source's.
[[ $(narHash "$p1") == "$(nix hash path --mode nar --format sri "$src")" ]]

# The same bytes executable: one blob, another file, never a link across the bit.
[[ $(inode "$p1/x") != $(inode "$p1/a") ]]
[[ $(nlink "$p1/x") == 2 ]]
cmp "$p1/x" "$p1/a"
[[ -x "$p1/x" ]]
[[ ! -x "$p1/a" ]]
for f in "$objects/blobs-x/"*; do [[ $(stat --format=%a "$f") == 555 ]]; done
for f in "$objects/blobs/"*; do [[ $(stat --format=%a "$f") == 444 ]]; done

# The identifiers are git's under SHA-256: a directory's object hash is
# its tree's, and the object store holds that tree.
rootId=$(nix hash path --mode git --algo sha256 --base16 "$p1")
[[ $(objectHash "$p1") == "git:sha256:$rootId" ]]
[[ -e $objects/trees/$rootId ]]

# A changed tree: only the changed file's blob is added; the rest share
# the first tree's inodes, two directories down among them.
echo three > "$src/d/b"
p2=$(nix store add --mode nar "$src")
[[ $(blobs) == 4 ]]
[[ $(inode "$p2/a") == $(inode "$p1/a") ]]
[[ $(inode "$p2/d/e/c") == $(inode "$p1/d/e/c") ]]
[[ $(inode "$p2/x") == $(inode "$p1/x") ]]
[[ $(nlink "$p2/a") == 5 ]]
[[ $(nlink "$p2/d/b") == 2 ]]
[[ $(cat "$p2/d/b") == three ]]
[[ $(trees) == 5 ]]                         # p2's root and d differ, e is shared

# The collector frees a deleted path's objects and no others: blobs by
# their link count, trees by reachability from the valid paths' object
# hashes.  No roots directory: the database is the root record.  `nix
# store delete` alone sweeps nothing (gc.cc: only a whole-store collection
# pays the pass over the object store); the objects go at the collection.
nix-store --add-root "$TEST_ROOT/root1" --indirect -r "$p1" > /dev/null
# `--quiet` takes the new CLI from `info` (stderr a file) to `notice`, the
# level it runs at on a terminal (main.cc), where the note must reach the
# interactive user it is for (before, it was printed at
# `info` and only a redirected stderr showed it).
nix store delete --quiet "$p2" 2> "$TEST_ROOT/delete.log"
# The freed figure counts p2's files whether or not the blob still holds
# them (physical accounting, 01 section 10); the report says when they go.
grepQuiet "reclaimed at the next \`nix-store --gc\`" "$TEST_ROOT/delete.log"
[[ $(blobs) == 4 ]]
[[ $(trees) == 5 ]]
nix-store --gc
[[ ! -e $objects/roots ]]
[[ -e $objects/trees/$rootId ]]
[[ $(blobs) == 3 ]]
[[ $(trees) == 3 ]]
[[ $(nlink "$p1/a") == 3 ]]
[[ $(cat "$p1/a") == one ]]

# Added again after the collection: the path is entered at the add, with
# no setting to turn that on; its tree is in the object store and its
# files share p1's inodes as soon as the add returns.
p3=$(nix store add --mode nar "$src")
[[ $(inode "$p3/a") == $(inode "$p1/a") ]]
[[ $(nlink "$p1/a") == 5 ]]
[[ $(cat "$p3/d/b") == three ]]
[[ $(blobs) == 4 ]]
[[ $(trees) == 5 ]]
p3Id=$(nix hash path --mode git --algo sha256 --base16 "$p3")
[[ $(objectHash "$p3") == "git:sha256:$p3Id" ]]
[[ -e $objects/trees/$p3Id ]]

# `nix-store --optimise` is the migration of paths an older Nix wrote:
# here there are none, so it links no file, and it removes the legacy
# `.links` table (planted as an older store would have left it: a hard
# link to a file, so removing it frees nothing).  Running it again
# changes nothing.
[[ ! -e $NIX_STORE_DIR/.links ]]
mkdir "$NIX_STORE_DIR/.links"
ln "$p3/a" "$NIX_STORE_DIR/.links/legacy"
[[ $(nlink "$p1/a") == 6 ]]
optimiseLinksNothing
[[ ! -e $NIX_STORE_DIR/.links ]]
[[ $(nlink "$p1/a") == 5 ]]
[[ $(inode "$p3/a") == $(inode "$p1/a") ]]
[[ -e $objects/trees/$p3Id ]]
[[ $(blobs) == 4 ]]
[[ $(trees) == 5 ]]
optimiseLinksNothing
[[ ! -e $NIX_STORE_DIR/.links ]]
[[ $(nlink "$p1/a") == 5 ]]
[[ $(blobs) == 4 ]]
[[ $(trees) == 5 ]]

# A restore that spills to a temporary directory (the daemon's usual route
# for anything above nar-buffer-size) enters the object store all the same.
echo five > "$src/d/b"
p4=$(nix store add --mode nar --option nar-buffer-size 0 "$src")
[[ $(inode "$p4/a") == $(inode "$p3/a") ]]
[[ -e $objects/trees/$(nix hash path --mode git --algo sha256 --base16 "$p4") ]]

# Substitution from a binary cache comes back through the object store.
nix copy --to "file://$TEST_ROOT/cache" "$p1"
rm "$TEST_ROOT/root1"
nix store delete "$p1"
[[ ! -e $p1 ]]
nix copy --from "file://$TEST_ROOT/cache" --no-check-sigs "$p1"
[[ $(inode "$p1/a") == $(inode "$p3/a") ]]
[[ $(inode "$p1/d/e/c") == $(inode "$p3/a") ]]
[[ $(inode "$p1/x") == $(inode "$p3/x") ]]
[[ $(objectHash "$p1") == "git:sha256:$rootId" ]]
[[ -e $objects/trees/$rootId ]]

# Verification checks the objects' bytes against their identifiers.  The
# blob of p1's d/b ("two"), which p1 alone links, is corrupted in place
# (same length), so the object and the path are one corruption; p1's root
# tree is corrupted too.  Without --repair each is reported; with it both
# objects are removed and p1 is repaired from the cache written above,
# re-entered through the object store: the file's content is back, its
# inode is the new blob's, and the tree is back.
twoId=$(gitBlobIdOfBytes $'two\n')
[[ $(inode "$p1/d/b") == $(inode "$objects/blobs/$twoId") ]]
[[ $(nlink "$p1/d/b") == 2 ]]               # p1 alone links it
nix-store --verify --check-contents
corruptFirstByte "$objects/blobs/$twoId"
[[ $(cat "$p1/d/b") == Xwo ]]               # the corruption reached the path
corruptFirstByte "$objects/trees/$rootId"
verifyOut=$(expectStderr 1 nix-store --verify --check-contents)
echo "$verifyOut" | grepQuiet "object \".*/blobs/$twoId\" was modified"
echo "$verifyOut" | grepQuiet "tree object \".*/trees/$rootId\" was modified"
echo "$verifyOut" | grepQuiet -F "path '$p1' was modified"
[[ -e $objects/blobs/$twoId ]]              # reported, not removed, without --repair
[[ -e $objects/trees/$rootId ]]
nix-store --verify --check-contents --repair \
    --substituters "file://$TEST_ROOT/cache" --no-require-sigs 2> "$TEST_ROOT/repair.log"
grepQuiet "removed object \".*/blobs/$twoId\"" "$TEST_ROOT/repair.log"
grepQuiet "removed tree object \".*/trees/$rootId\"" "$TEST_ROOT/repair.log"
nix-store --verify --check-contents
[[ $(cat "$p1/d/b") == two ]]
[[ -e $objects/blobs/$twoId ]]
[[ $(inode "$p1/d/b") == $(inode "$objects/blobs/$twoId") ]]
[[ $(nlink "$p1/d/b") == 2 ]]
[[ -e $objects/trees/$rootId ]]
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
[[ $(nlink "$q1/empty") == 2 ]]
[[ $(nlink "$q1/exe") == 2 ]]
[[ -x "$q1/exe" ]]
[[ -e $objects/blobs/$(gitBlobIdOfBytes '') ]]
[[ -e $objects/blobs-x/$(gitBlobIdOfBytes x) ]]
echo other > "$src2/other"
q2=$(nix store add --mode nar "$src2")
[[ $(inode "$q2/empty") == $(inode "$q1/empty") ]]
[[ $(inode "$q2/exe") == $(inode "$q1/exe") ]]
[[ $(nlink "$q1/empty") == 3 ]]

# Both routes to a link: a file the store holds is linked without being
# written when it is under 1 MiB, and written and then replaced by a link
# above that.  Either way the second tree's file is the first's inode.
big=$TEST_ROOT/big
rm -rf "$big"
mkdir -p "$big"
dd if=/dev/zero of="$big/z" bs=1048576 count=2 status=none
b1=$(nix store add --mode nar "$big")
[[ $(nlink "$b1/z") == 2 ]]
echo tag > "$big/t"
b2=$(nix store add --mode nar "$big")
[[ $(inode "$b2/z") == $(inode "$b1/z") ]]
[[ $(nlink "$b1/z") == 3 ]]
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
[[ $(nlink "$q2/other") == 3 ]]
nix-store --gc
[[ ! -e $NIX_STORE_DIR/.links ]]
[[ $(cat "$q2/other") == other ]]
[[ $(nlink "$q2/other") == 2 ]]
