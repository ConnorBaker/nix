#!/usr/bin/env bash

set -eu -o pipefail

set -x

source common.sh

# Avoid store dir being inside sandbox build-dir
unset NIX_STORE_DIR
unset NIX_STATE_DIR

setupStoreDirs

initLowerStore

mountOverlayfs


## Initialise stores for test

# Realise a derivation from the lower store to propagate paths to overlay DB
nix-store --store "$storeB" --realise "$drvPath"

# Also ensure dummy file exists in overlay DB
dummyPath=$(nix-store --store "$storeB" --add ../dummy)

# Add something to the lower store that will not be propagated to overlay DB
lowerOnlyPath=$(addTextToStore "$storeA" lower-only "Only in lower store")

# Verify should be successful at this point
nix-store --store "$storeB" --verify --check-contents

# Make a backup so we can repair later
backupStore="$storeVolume/backup"
mkdir "$backupStore"
cp -ar "$storeBRoot/nix" "$backupStore"


## Deliberately corrupt store paths

# Delete one of the derivation inputs in the lower store
inputDrvFullPath=$(find "$storeA" -name "*-hermetic-input-1.drv")
inputDrvPath=${inputDrvFullPath/*\/nix\/store\///nix/store/}
rm -v "$inputDrvFullPath"

# Truncate the contents of dummy file in lower store
find "$storeA" -name "*-dummy" -exec truncate -s 0 {} \;

# Also truncate the file that only exists in lower store
truncate -s 0 "$storeA/$lowerOnlyPath"

# Ensure overlayfs is synchronised
remountOverlayfs


## Now test that verify and repair work as expected

# Verify overlay store without attempting to repair it
verifyOutput=$(expectStderr 1 nix-store --store "$storeB" --verify --check-contents)
<<<"$verifyOutput" grepQuiet "path '$inputDrvPath' disappeared, but it still has valid referrers!"
<<<"$verifyOutput" grepQuiet "path '$dummyPath' was modified! expected object hash"
<<<"$verifyOutput" expectStderr 1 grepQuiet "$lowerOnlyPath"  # Expect no error for corrupted lower-only path

# Attempt to repair using backup
addConfig "substituters = $backupStore"
repairOutput=$(nix-store --store "$storeB" --verify --check-contents --repair 2>&1)
<<<"$repairOutput" grepQuiet "copying path '$inputDrvPath'"
<<<"$repairOutput" grepQuiet "copying path '$dummyPath'"

remountOverlayfs
nix-store --store "$storeB" --verify --check-contents
cmp "$backupStore/$inputDrvPath" "$(toRealPath "$storeBRoot/nix/store" "$inputDrvPath")"
cmp ../dummy "$(toRealPath "$storeBRoot/nix/store" "$dummyPath")"
test -f "$storeBTop/${inputDrvPath##*/}"
test -f "$storeBTop/${dummyPath##*/}"
expect 1 test -e "$inputDrvFullPath"
test -f "$storeA/$dummyPath"
expect 1 test -s "$storeA/$dummyPath"
test -f "$storeA/$lowerOnlyPath"
expect 1 test -s "$storeA/$lowerOnlyPath"
expect 1 test -e "$storeBTop/${lowerOnlyPath##*/}"
