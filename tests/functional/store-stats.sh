#!/usr/bin/env bash

source common.sh

TODO_NixOS # auto-optimise-store is a restricted setting for non-trusted users

clearStoreIfPossible

# Build three derivations: foo1 and foo2 have identical contents (so
# deduplication kicks in), foo3 differs.
# shellcheck disable=SC2016
outPath1=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "foo1"; builder = builtins.toFile "builder" "mkdir $out; echo hello > $out/foo"; }' | nix-build - --no-out-link --auto-optimise-store)
# shellcheck disable=SC2016
outPath2=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "foo2"; builder = builtins.toFile "builder" "mkdir $out; echo hello > $out/foo"; }' | nix-build - --no-out-link --auto-optimise-store)
# shellcheck disable=SC2016
outPath3=$(echo 'with import '"${config_nix}"'; mkDerivation { name = "foo3"; builder = builtins.toFile "builder" "mkdir $out; echo unique > $out/foo"; }' | nix-build - --no-out-link --auto-optimise-store)

# Make sure deduplication actually happened so the test is exercising
# what it claims to exercise.
inode1=$(stat --format=%i "$outPath1"/foo)
inode2=$(stat --format=%i "$outPath2"/foo)
[[ "$inode1" = "$inode2" ]] || fail "foo1/foo and foo2/foo are not hardlinked; auto-optimise did not run"

# --- cheap query (no flags): SQL aggregate only. No histograms, no walks.

cheap_json=$(nix store stats --json)

[[ "$(echo "$cheap_json" | jq -r '.available')" = "true" ]] || fail "cheap query: stats not available"

cheap_path_count=$(echo "$cheap_json" | jq -r '.pathCount')
(( cheap_path_count >= 3 )) || fail "cheap query: pathCount $cheap_path_count < 3"

cheap_nar_total=$(echo "$cheap_json" | jq -r '.totalNarSize')
(( cheap_nar_total > 0 )) || fail "cheap query: totalNarSize is 0"

# Cheap query must not include optional sections.
[[ "$(echo "$cheap_json" | jq 'has("narSizeHistogram")')" = "false" ]] || fail "cheap query unexpectedly included narSizeHistogram"
[[ "$(echo "$cheap_json" | jq 'has("dedup")')" = "false" ]] || fail "cheap query unexpectedly included dedup"
[[ "$(echo "$cheap_json" | jq 'has("fullWalk")')" = "false" ]] || fail "cheap query unexpectedly included fullWalk"

# --- histograms-only: adds the NAR-size histogram, still no walks.

hist_json=$(nix store stats --json --histograms)

[[ "$(echo "$hist_json" | jq 'has("narSizeHistogram")')" = "true" ]] || fail "histograms: missing narSizeHistogram"
[[ "$(echo "$hist_json" | jq 'has("dedup")')" = "false" ]] || fail "histograms-alone unexpectedly included dedup"
hist_sum=$(echo "$hist_json" | jq '[.narSizeHistogram[].count] | add')
(( hist_sum == cheap_path_count )) || fail "histograms: nar histogram sums to $hist_sum, want $cheap_path_count"

# --- detailed: walks .links and every store path. No histogram unless --histograms is also passed.

detailed_json=$(nix store stats --json --detailed)

[[ "$(echo "$detailed_json" | jq 'has("dedup")')" = "true" ]] || fail "detailed: missing dedup section"
[[ "$(echo "$detailed_json" | jq 'has("fullWalk")')" = "true" ]] || fail "detailed: missing fullWalk section"
[[ "$(echo "$detailed_json" | jq '.dedup | has("sizeHistogram")')" = "false" ]] || fail "detailed (without --histograms) unexpectedly included .links histogram"

links_count=$(echo "$detailed_json" | jq -r '.dedup.linksFileCount')
unique_bytes=$(echo "$detailed_json" | jq -r '.dedup.uniqueBytes')
unique_disk_bytes=$(echo "$detailed_json" | jq -r '.dedup.uniqueDiskBytes')
dedup_bytes=$(echo "$detailed_json" | jq -r '.dedup.dedupBytes')
dedup_disk_bytes=$(echo "$detailed_json" | jq -r '.dedup.dedupDiskBytes')
deduped_count=$(echo "$detailed_json" | jq -r '.dedup.dedupedFileCount')
inodes_saved=$(echo "$detailed_json" | jq -r '.dedup.inodesSaved')

(( links_count > 0 )) || fail "detailed: linksFileCount is 0"
(( unique_bytes > 0 )) || fail "detailed: uniqueBytes is 0"
(( unique_disk_bytes >= unique_bytes )) || fail "detailed: uniqueDiskBytes ($unique_disk_bytes) < uniqueBytes ($unique_bytes), block padding should not reduce size"
(( deduped_count >= 1 )) || fail "detailed: dedupedFileCount is 0 despite known duplicate"
(( inodes_saved >= 1 )) || fail "detailed: inodesSaved is 0 despite known duplicate"
(( dedup_bytes >= 1 )) || fail "detailed: dedupBytes is 0 despite known duplicate"
(( dedup_disk_bytes >= dedup_bytes )) || fail "detailed: dedupDiskBytes ($dedup_disk_bytes) < dedupBytes ($dedup_bytes)"

total_disk=$(echo "$detailed_json" | jq -r '.fullWalk.totalDiskBytes')
total_inodes=$(echo "$detailed_json" | jq -r '.fullWalk.totalInodes')
file_inodes=$(echo "$detailed_json" | jq -r '.fullWalk.fileInodes')
dir_inodes=$(echo "$detailed_json" | jq -r '.fullWalk.dirInodes')
sym_inodes=$(echo "$detailed_json" | jq -r '.fullWalk.symlinkInodes')

(( total_disk > 0 )) || fail "detailed: totalDiskBytes is 0"
expected_total=$(( file_inodes + dir_inodes + sym_inodes ))
(( total_inodes == expected_total )) || fail "detailed: totalInodes ($total_inodes) != file+dir+sym ($expected_total)"
(( file_inodes > 0 )) || fail "detailed: fileInodes is 0"
(( dir_inodes > 0 )) || fail "detailed: dirInodes is 0 (store root and .links should be counted)"

# Cross-check: full walk's total disk bytes >= dedup walk's unique disk bytes
# (full walk also covers directories and symlinks).
(( total_disk >= unique_disk_bytes )) || fail "detailed: totalDiskBytes ($total_disk) < uniqueDiskBytes ($unique_disk_bytes)"

# --- detailed + histograms: now the .links histogram appears.

detailed_hist_json=$(nix store stats --json --detailed --histograms)
[[ "$(echo "$detailed_hist_json" | jq '.dedup | has("sizeHistogram")')" = "true" ]] || fail "detailed+histograms: missing .links histogram"
links_hist_sum=$(echo "$detailed_hist_json" | jq '[.dedup.sizeHistogram[].count] | add')
(( links_hist_sum == links_count )) || fail ".links histogram sums to $links_hist_sum, want $links_count"

# --- human-readable smoke checks.

human=$(nix store stats --detailed --histograms 2>&1)
echo "$human" | grep -q 'Valid paths:' || fail "human output missing 'Valid paths:'"
echo "$human" | grep -q 'Total NAR size:' || fail "human output missing 'Total NAR size:'"
echo "$human" | grep -q 'From .links walk:' || fail "human output missing 'From .links walk:'"
echo "$human" | grep -q 'Inodes saved by dedup:' || fail "human output missing 'Inodes saved by dedup:'"
echo "$human" | grep -q 'From full store walk:' || fail "human output missing 'From full store walk:'"
echo "$human" | grep -q 'Total disk bytes:' || fail "human output missing 'Total disk bytes:'"
echo "$human" | grep -q 'NAR size distribution:' || fail "human output missing 'NAR size distribution:'"
