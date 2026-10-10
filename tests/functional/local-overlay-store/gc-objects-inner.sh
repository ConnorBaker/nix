#!/usr/bin/env bash

# The overlay owns its database's auxiliary values and only the upper
# layer's ingestion leases. Collection drops dead projections without
# touching lower files, hiding them with whiteouts, or changing live bytes.

set -eu -o pipefail

set -x

source common.sh

# Avoid store dir being inside sandbox build-dir
unset NIX_STORE_DIR
unset NIX_STATE_DIR

setupStoreDirs

initLowerStore

lowerObjects="$storeA/nix/store/.objects"
upperObjects="$storeBTop/.objects"
mergedObjects="$storeBRoot/nix/store/.objects"

# Plant dead lease pins in both layers. Upper cleanup must leave the lower
# pin visible after removing the duplicate upper pin, without a whiteout.
duplicateRelative="tmp/hold-4242-1/pin"
upperOnlyRelative="tmp/hold-4242-2/pin"
mkdir -p "$(dirname "$lowerObjects/$duplicateRelative")" "$storeA/nix/store/.links"
printf 'lower lease pin\n' > "$lowerObjects/$duplicateRelative"
printf 'lower legacy link fixture\n' > "$storeA/nix/store/.links/legacy"
mountOverlayfs

# Git's blob identifier under SHA-256, computed outside Nix: the hash of
# `blob <size>\0` followed by the bytes.
gitBlobId() { { printf 'blob %d\0' "$(stat --format=%s "$1")"; cat "$1"; } | sha256sum | cut -c1-64; }
gitBlobIdOfBytes() { { printf 'blob %d\0' "${#1}"; printf '%s' "$1"; } | sha256sum | cut -c1-64; }

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
hasAuxiliary() { sqlite3 "$db" "select exists(select 1 from AuxValues where kind = $1 and id = X'$2')"; }

# Two trees added through the overlay publish into its own database.
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

# Both typed projections are in the upper store's database; regular rows
# still name the actual destination files.
[[ $(hasAuxiliary 16384 "$deadTree") == 1 ]]
[[ $(hasAuxiliary 16384 "$liveTree") == 1 ]]
[[ $(hasAuxiliary 40960 "$deadLink") == 1 ]]
[[ $(hasAuxiliary 40960 "$liveLink") == 1 ]]
[[ $(holder "$deadBlob") == "$deadPath/f" ]]
[[ $(holder "$liveBlob") == "$livePath/g" ]]
[[ $(ingestions "$deadPath") == 1 ]]
[[ $(ingestions "$livePath") == 1 ]]
liveUpper=$(toRealPath "$storeBTop" "$livePath")
liveInode=$(stat --format=%i "$liveUpper/g")

mkdir -p "$(dirname "$upperObjects/$duplicateRelative")" "$(dirname "$upperObjects/$upperOnlyRelative")"
cp "$lowerObjects/$duplicateRelative" "$upperObjects/$duplicateRelative"
printf 'upper-only lease pin\n' > "$upperObjects/$upperOnlyRelative"
remountOverlayfs
cmp "$lowerObjects/$duplicateRelative" "$upperObjects/$duplicateRelative"
test -e "$mergedObjects/$upperOnlyRelative"

# Snapshot the lower lease fixture. Its explicit pin makes the
# preservation checks non-vacuous.
lowerBefore="$TEST_ROOT/lower-objects-before"
listObjects "$lowerObjects" > "$lowerBefore"
test -s "$lowerBefore"
hashObjects "$lowerObjects" > "$TEST_ROOT/lower-checksums-before"

# A root for the live path alone.
ln -sf "$livePath" "$stateB/gcroots/live"

nix-collect-garbage

remountOverlayfs

# The dead path and both projections are gone.
expect 1 test -e "$(toRealPath "$storeBRoot/nix/store" "$deadPath")"
[[ $(hasAuxiliary 16384 "$deadTree") == 0 ]]
[[ $(hasAuxiliary 40960 "$deadLink") == 0 ]]
[[ $(blobRows "$deadBlob") == 0 ]]
[[ $(ingestions "$deadPath") == 0 ]]

# The live path retains both projections, bytes, symlink target and inode.
test -e "$(toRealPath "$storeBRoot/nix/store" "$livePath")"
[[ $(hasAuxiliary 16384 "$liveTree") == 1 ]]
[[ $(hasAuxiliary 40960 "$liveLink") == 1 ]]
[[ $(holder "$liveBlob") == "$livePath/g" ]]
[[ $(ingestions "$livePath") == 1 ]]
cmp "$liveSrc/g" "$(toRealPath "$storeBRoot/nix/store" "$livePath")/g"
[[ $(stat --format=%i "$liveUpper/g") == "$liveInode" ]]
[[ $(readlink "$(toRealPath "$storeBRoot/nix/store" "$livePath")/link") == unique-live-target ]]

expect 1 test -e "$upperObjects/$duplicateRelative"
expect 1 test -e "$upperObjects/$upperOnlyRelative"
expect 1 test -e "$mergedObjects/$upperOnlyRelative"
test -e "$mergedObjects/$duplicateRelative"
cmp "$lowerObjects/$duplicateRelative" "$mergedObjects/$duplicateRelative"
expect 1 test -e "$(dirname "$upperObjects/$duplicateRelative")"
expect 1 test -e "$(dirname "$upperObjects/$upperOnlyRelative")"

# No whiteout under `.objects`: a whiteout is a character device in the
# upper layer, which an unlink through the merged mount of a file the lower
# store also has would leave there.
[ "$(find "$upperObjects" ! -type d ! -type f | wc -l)" = 0 ]

# Nor a whited-out `.links`: the legacy directory under the merged store
# directory is the lower store's, and this store removes none.
[ "$(find "$storeBTop" -maxdepth 1 -name .links | wc -l)" = 0 ]
[[ $(cat "$storeA/nix/store/.links/legacy") == 'lower legacy link fixture' ]]
cmp "$storeA/nix/store/.links/legacy" "$storeBRoot/nix/store/.links/legacy"

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
