#!/usr/bin/env bash

# The collector's sweep of the object store on the overlay store
# (doc/lazy-store/01-specification.md, section 9.10, "Collection"; section
# 10, *the local overlay store's object store is not its own*): the store
# owns the upper layer's objects and sweeps those alone.  A path added
# through the overlay writes its objects into the upper layer; when it dies
# they go, while the lower store's objects are neither enumerated nor
# touched, no whiteout is made under `.objects`, the legacy `.links`
# directory is left to the lower store, and a live path's objects stay.

set -eu -o pipefail

set -x

source common.sh

# Avoid store dir being inside sandbox build-dir
unset NIX_STORE_DIR
unset NIX_STATE_DIR

setupStoreDirs

initLowerStore

mountOverlayfs

lowerObjects="$storeA/nix/store/.objects"
upperObjects="$storeBTop/.objects"
mergedObjects="$storeBRoot/nix/store/.objects"

# Git's blob identifier under SHA-256, computed outside Nix: the hash of
# `blob <size>\0` followed by the bytes.
gitBlobId() { { printf 'blob %d\0' "$(stat --format=%s "$1")"; cat "$1"; } | sha256sum | cut -c1-64; }
gitBlobIdOfBytes() { { printf 'blob %d\0' "${#1}"; printf '%s' "$1"; } | sha256sum | cut -c1-64; }

# The object store's file for an identifier: git's own layout, the
# identifier's first two hexadecimal digits a shard directory
# (doc/lazy-store/01-specification.md section 9.10).
objectFile() { echo "$1/$2/${3:0:2}/$3"; }

listObjects() { ( cd "$1" && find . -type f | sort ); }
hashObjects() { ( cd "$1" && find . -type f -print0 | sort -z | xargs -0 -r sha256sum ); }

export NIX_REMOTE="$storeB"
stateB="$storeBRoot/nix/var/nix"
mkdir -p "$stateB/gcroots"
db="$stateB/db/db.sqlite"
holder() {
    sqlite3 "$db" "select i.path || (case when b.name = '' then '' else '/' || b.name end)
                   from Blobs b join Ingestions i on i.id = b.ingestion
                   join ValidPaths v on v.path = i.path
                   where b.id = X'$1' and b.exec = 0
                   order by b.dev, b.ino, b.ingestion limit 1"
}
blobRows() { sqlite3 "$db" "select count(*) from Blobs where id = X'$1' and exec = 0"; }
ingestions() { sqlite3 "$db" "select count(*) from Ingestions where path = '$1'"; }

# Two trees added through the overlay.  Their bytes are this store's alone,
# so the merged object store lacked every object of them and each write
# landed in the upper layer.
deadSrc="$TEST_ROOT/dead-src"
mkdir -p "$deadSrc"
echo "content only the upper layer has, dead" > "$deadSrc/f"
ln -s unique-dead-target "$deadSrc/link"
liveSrc="$TEST_ROOT/live-src"
mkdir -p "$liveSrc"
echo "content only the upper layer has, live" > "$liveSrc/g"
ln -s unique-live-target "$liveSrc/link"

deadPath=$(nix-store --store "$storeB" --add "$deadSrc")
livePath=$(nix-store --store "$storeB" --add "$liveSrc")
remountOverlayfs

# A directory's object hash is its git tree's, and each file's blob is
# named by git's identifier of its bytes: computed here outside Nix.
deadTree=$(nix hash path --mode git --algo sha256 --base16 "$(toRealPath "$storeBRoot/nix/store" "$deadPath")")
liveTree=$(nix hash path --mode git --algo sha256 --base16 "$(toRealPath "$storeBRoot/nix/store" "$livePath")")
deadBlob=$(gitBlobId "$deadSrc/f")
liveBlob=$(gitBlobId "$liveSrc/g")
deadLink=$(gitBlobIdOfBytes "$(readlink "$deadSrc/link")")
liveLink=$(gitBlobIdOfBytes "$(readlink "$liveSrc/link")")

# Both paths' objects are the upper layer's, and none of them the lower
# store's.
test -e "$(objectFile "$upperObjects" trees "$deadTree")"
test -e "$(objectFile "$upperObjects" trees "$liveTree")"
test -e "$(objectFile "$upperObjects" blobs "$deadLink")"
test -e "$(objectFile "$upperObjects" blobs "$liveLink")"
[[ $(holder "$deadBlob") == "$deadPath/f" ]]
[[ $(holder "$liveBlob") == "$livePath/g" ]]
[[ $(ingestions "$deadPath") == 1 ]]
[[ $(ingestions "$livePath") == 1 ]]
expect 1 test -e "$(objectFile "$lowerObjects" trees "$deadTree")"
expect 1 test -e "$(objectFile "$lowerObjects" blobs "$deadLink")"

duplicate=$(find "$lowerObjects/trees" -type f -print -quit)
test -n "$duplicate"
duplicateRelative=${duplicate#"$lowerObjects/"}
mkdir -p "$(dirname "$upperObjects/$duplicateRelative")"
cp "$duplicate" "$upperObjects/$duplicateRelative"
remountOverlayfs
cmp "$duplicate" "$upperObjects/$duplicateRelative"

# The lower store's objects as they stand.  The comparison after the
# collection is not vacuous: `initLowerStore` built that store.
lowerBefore="$TEST_ROOT/lower-objects-before"
listObjects "$lowerObjects" > "$lowerBefore"
test -s "$lowerBefore"
hashObjects "$lowerObjects" > "$TEST_ROOT/lower-checksums-before"

# A root for the live path alone.
ln -sf "$livePath" "$stateB/gcroots/live"

nix-collect-garbage

remountOverlayfs

# The dead path is gone, and so are its objects in the upper layer.
expect 1 test -e "$(toRealPath "$storeBRoot/nix/store" "$deadPath")"
expect 1 test -e "$(objectFile "$upperObjects" trees "$deadTree")"
expect 1 test -e "$(objectFile "$upperObjects" blobs "$deadLink")"
[[ $(blobRows "$deadBlob") == 0 ]]
[[ $(ingestions "$deadPath") == 0 ]]

# The live path's objects are kept: the sweep takes only what nothing
# reaches.
test -e "$(toRealPath "$storeBRoot/nix/store" "$livePath")"
test -e "$(objectFile "$upperObjects" trees "$liveTree")"
test -e "$(objectFile "$upperObjects" blobs "$liveLink")"
[[ $(holder "$liveBlob") == "$livePath/g" ]]
[[ $(ingestions "$livePath") == 1 ]]
cmp "$liveSrc/g" "$(toRealPath "$storeBRoot/nix/store" "$livePath")/g"
[[ $(readlink "$(toRealPath "$storeBRoot/nix/store" "$livePath")/link") == unique-live-target ]]

expect 1 test -e "$upperObjects/$duplicateRelative"
test -e "$mergedObjects/$duplicateRelative"

# No whiteout under `.objects`: a whiteout is a character device in the
# upper layer, which an unlink through the merged mount of a file the lower
# store also has would leave there.
[ "$(find "$upperObjects" ! -type d ! -type f | wc -l)" = 0 ]

# Nor a whited-out `.links`: the legacy directory under the merged store
# directory is the lower store's, and this store removes none.
[ "$(find "$storeBTop" -maxdepth 1 -name .links | wc -l)" = 0 ]

# The lower store's objects are untouched, and still visible through the
# merged mount.
lowerAfter="$TEST_ROOT/lower-objects-after"
listObjects "$lowerObjects" > "$lowerAfter"
diff "$lowerBefore" "$lowerAfter"
hashObjects "$lowerObjects" > "$TEST_ROOT/lower-checksums-after"
diff "$TEST_ROOT/lower-checksums-before" "$TEST_ROOT/lower-checksums-after"
while read -r object; do
  test -e "$mergedObjects/${object#./}"
done < "$lowerBefore"

# And the store is sound.
nix-store --store "$storeB" --verify --check-contents
