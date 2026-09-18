#!/usr/bin/env bash

# A fetched tree goes into the local store's object store through the one
# route every ingestion takes (doc/lazy-store/01-specification.md, section
# 9.10; every source is named by its git tree hash, section 9.11): the
# tarball's tree is streamed from the tarball cache into the sink, the
# store path is named by the root identifier the sink computed — no file
# read back — its files are links into the object store, and a second
# tarball sharing files with the first writes only the files it does not
# share.

source common.sh

needLocalStore "the object store is the local store's directory"

clearStore

objects=$NIX_STORE_DIR/.objects
nlink() { stat --format=%h "$1"; }
inode() { stat --format=%i "$1"; }
blobs() { find "$objects/blobs" -mindepth 1 -maxdepth 1 | wc -l | tr -d ' '; }

src=$TEST_ROOT/src
rm -rf "$src"
mkdir -p "$src/d"
echo one > "$src/a"
echo two > "$src/d/b"
echo three > "$src/d/c"
tar -C "$TEST_ROOT" -cf "$TEST_ROOT/one.tar" src

# The first tarball: every file written and entered.
p1=$(nix eval --raw --impure --expr "builtins.fetchTarball \"file://$TEST_ROOT/one.tar\"")
[[ $(blobs) == 3 ]]
[[ $(nlink "$p1/a") == 2 ]]
[[ $(cat "$p1/d/c") == three ]]

# Its name is the tree's identifier: the object hash the database holds,
# the content address and `nix hash path --mode git` agree, and the object
# store holds the tree.
rootId=$(nix hash path --mode git --algo sha256 --base16 "$p1")
[[ $(nix path-info --json --json-format 4 "$p1" | jq -r '.info | to_entries[0].value.objectHash') == "git:sha256:$rootId" ]]
[[ -e $objects/trees/$rootId ]]
[[ $(nix path-info --json --json-format 2 "$p1" | jq -r '.info | to_entries[0].value.ca.method') == git ]]
caHash=$(nix path-info --json --json-format 2 "$p1" | jq -r '.info | to_entries[0].value.ca.hash')
[[ $(nix hash convert --hash-algo sha256 --to base16 "$caHash") == "$rootId" ]]
narHash1=$(nix path-info --json --json-format 2 "$p1" | jq -r '.info | to_entries[0].value.narHash')

# The same tree fetched again after the path is deleted.  `nix store
# delete` deletes the path and sweeps nothing: the sweep costs a pass over
# the whole object store, which only a whole-store collection pays (gc.cc;
# 01 section 10, "The object store is swept on whole-store collections
# only"), so the deleted path's tree and blobs -- the only
# objects here -- stay until `nix-store --gc`, and go then.  The refetch is
# the same path with the same NAR hash, its files written again and
# entered as blobs.
nix store delete "$p1"
[[ $(blobs) == 3 ]]
[[ -e $objects/trees/$rootId ]]
nix-store --gc
[[ $(blobs) == 0 ]]
[[ ! -e $objects/trees/$rootId ]]
p1b=$(nix eval --raw --impure --expr "builtins.fetchTarball \"file://$TEST_ROOT/one.tar\"")
[[ $p1b == "$p1" ]]
[[ $(nix path-info --json --json-format 2 "$p1" | jq -r '.info | to_entries[0].value.narHash') == "$narHash1" ]]
[[ $(nlink "$p1/a") == 2 ]]
[[ $(blobs) == 3 ]]
[[ -e $objects/trees/$rootId ]]

# A second tarball differing in one file: one blob written, the rest linked.
echo four > "$src/d/c"
tar -C "$TEST_ROOT" -cf "$TEST_ROOT/two.tar" src
p2=$(nix eval --raw --impure --expr "builtins.fetchTarball \"file://$TEST_ROOT/two.tar\"")
[[ $p2 != "$p1" ]]
[[ $(blobs) == 4 ]]
[[ $(inode "$p2/a") == $(inode "$p1/a") ]]
[[ $(nlink "$p1/a") == 3 ]]
[[ $(cat "$p2/d/c") == four ]]

# The same tarball again is a cache hit and copies nothing.
p3=$(nix eval --raw --impure --expr "builtins.fetchTarball \"file://$TEST_ROOT/two.tar\"")
[[ $p3 == "$p2" ]]
[[ $(blobs) == 4 ]]
