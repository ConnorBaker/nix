#!/usr/bin/env bash

# A fetched tree goes into the local store's object store through the one
# route every ingestion takes (doc/lazy-store/01-specification.md, section
# 9.10; every source is named by its git tree hash, section 9.11): the
# tarball's tree is streamed from the tarball cache into the sink, the
# store path is named by the root identifier the sink computed — no file
# read back — its files are the store's blobs, and a second tarball
# sharing files with the first writes only the files it does not share.
#
# Regular holders and auxiliary tree bodies are both recorded in the
# receiving ingestion. Reused regular files preserve their inode sharing.

source common.sh

needLocalStore "the object store is the local store's directory"
type -p sqlite3 > /dev/null || skipTest "sqlite3 is not on PATH"

clearStore

db=$NIX_STATE_DIR/db/db.sqlite
nlink() { stat --format=%h "$1"; }
inode() { stat --format=%i "$1"; }
hasTree() { sqlite3 "$db" "select exists(select 1 from AuxValues where kind = 16384 and id = X'$1')"; }
# Every candidate whose ingestion has a path, including reused blobs.
blobRows() { sqlite3 "$NIX_STATE_DIR/db/db.sqlite" "select count(*) from Blobs b join Ingestions i on i.id = b.ingestion where b.exec = 0 and i.path is not null"; }

src=$TEST_ROOT/src
rm -rf "$src"
mkdir -p "$src/d"
echo one > "$src/a"
echo two > "$src/d/b"
echo three > "$src/d/c"
tar -C "$TEST_ROOT" -cf "$TEST_ROOT/one.tar" src

# The first tarball: every file written and entered.
p1=$(nix eval --raw --impure --expr "builtins.fetchTarball \"file://$TEST_ROOT/one.tar\"")
[[ $(blobRows) == 3 ]]
[[ $(sqlite3 "$db" "select count(*) from AuxValues where kind = 40960") == 0 ]]
[[ $(nlink "$p1/a") == 1 ]]
[[ $(cat "$p1/d/c") == three ]]

# Its name is the tree's identifier: the object hash the database holds,
# the content address and `nix hash path --mode git` agree, and the object
# store holds the tree.
rootId=$(nix hash path --mode git --algo sha256 --base16 "$p1")
[[ $(nix path-info --json --json-format 4 "$p1" | jq -r '.info | to_entries[0].value.objectHash') == "git:sha256:$rootId" ]]
[[ $(hasTree "$rootId") == 1 ]]
[[ $(nix path-info --json --json-format 2 "$p1" | jq -r '.info | to_entries[0].value.ca.method') == git ]]
caHash=$(nix path-info --json --json-format 2 "$p1" | jq -r '.info | to_entries[0].value.ca.hash')
[[ $(nix hash convert --hash-algo sha256 --to base16 "$caHash") == "$rootId" ]]
narHash1=$(nix path-info --json --json-format 2 "$p1" | jq -r '.info | to_entries[0].value.narHash')

# Specific deletion leaves ingestion rows until whole-store collection.
# Collection drops both projections; refetch restores the same path/NAR
# and records its newly written regular holders and auxiliary tree bodies.
nix store delete "$p1"
[[ $(blobRows) == 3 ]]
[[ $(hasTree "$rootId") == 1 ]]
nix-store --gc
[[ $(blobRows) == 0 ]]
[[ $(hasTree "$rootId") == 0 ]]
p1b=$(nix eval --raw --impure --expr "builtins.fetchTarball \"file://$TEST_ROOT/one.tar\"")
[[ $p1b == "$p1" ]]
[[ $(nix path-info --json --json-format 2 "$p1" | jq -r '.info | to_entries[0].value.narHash') == "$narHash1" ]]
[[ $(nlink "$p1/a") == 1 ]]
[[ $(blobRows) == 3 ]]
[[ $(hasTree "$rootId") == 1 ]]

# A second tarball differing in one file: one blob written, the rest linked.
echo four > "$src/d/c"
tar -C "$TEST_ROOT" -cf "$TEST_ROOT/two.tar" src
p2=$(nix eval --raw --impure --expr "builtins.fetchTarball \"file://$TEST_ROOT/two.tar\"")
[[ $p2 != "$p1" ]]
[[ $(blobRows) == 6 ]]
for file in a d/b; do
    [[ $(inode "$p2/$file") == $(inode "$p1/$file") ]]
    [[ $(nlink "$p1/$file") == 2 ]]
done
[[ $(inode "$p2/d/c") != $(inode "$p1/d/c") ]]
[[ $(cat "$p2/d/c") == four ]]

# The same tarball again is a cache hit and copies nothing.
p3=$(nix eval --raw --impure --expr "builtins.fetchTarball \"file://$TEST_ROOT/two.tar\"")
[[ $p3 == "$p2" ]]
[[ $(blobRows) == 6 ]]
