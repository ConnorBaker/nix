R""(

# Examples

* Cheap summary (single SQL aggregate, no filesystem traversal):

  ```console
  # nix store stats
  ```

* Walk every store path for deduplication metrics and exact on-disk
  totals:

  ```console
  # nix store stats --detailed
  ```

* Add size-distribution histograms; combine with the JSON output:

  ```console
  # nix store stats --detailed --histograms --json
  ```

# Description

Summary statistics for the contents of a Nix store. Output is grouped
into sections; everything except the cheap default is opt-in.

## Default

Single SQL aggregate against `ValidPaths`. No filesystem traversal.

* `pathCount` — number of valid store paths.
* `totalNarSize` — sum of each path's NAR size. NAR (Nix Archive) is
  the canonical serialized form of a store path; `narSize` is the
  byte length of that uncompressed archive. **Logical and
  double-counted**: a file shared across N paths contributes N times.

## With `--detailed`

Recursive walk of every store path. Counts each inode once via
hardlink dedup tracking, so on-disk totals match
`du --block-size=1 $NIX_STORE_DIR` closely. Expect tens of seconds and
millions of `lstat` calls on a large store.

Dedup numbers (derived from the `.links` pool):

* `linksFileCount` — entries in `.links`, i.e. unique file contents.
* `dedupedFileCount` — `.links` entries with `nlink > 2` (a single
  unique content shared by 2+ store paths).
* `inodesSaved` — sum of `nlink - 2` over those entries: store-file
  inodes that hardlinking avoided creating.
* `uniqueBytes` — sum of `st_size`. Logical bytes for unique content.
* `uniqueDiskBytes` — sum of `st_blocks * 512`. On-disk bytes for
  unique content, accounting for filesystem block padding.
* `dedupBytes` / `dedupDiskBytes` — bytes that would be duplicated
  without hardlinking. Logical vs. on-disk.

Why `nlink > 2` for "deduplicated": a `.links` entry has `nlink = 1`
(itself) plus one per store file referencing it; real sharing starts at
two referencing files.

Full-walk numbers (every reachable entry, each inode once):

* `totalDiskBytes` — closest equivalent to `du --block-size=1` on the
  store directory.
* `fileInodes`, `dirInodes`, `symlinkInodes` — inode counts by type.
* `totalInodes` — sum of the above (derived in the CLI; not stored on
  the wire).

## With `--histograms`

Adds power-of-two-bucketed size distributions:

* NAR-size histogram across store paths — requires iterating
  `ValidPaths` rows rather than the cheap aggregate, but the cost is
  still small.
* If `--detailed` is also set, a `.links` size histogram across unique
  contents.

# Caveats

* `totalNarSize` is logical and double-counted; don't use it for "how
  much disk does my store use?"
* `uniqueDiskBytes` excludes directory and symlink inodes; use
  `totalDiskBytes` for the full picture, or `du` for ground truth.
* On btrfs/ZFS with transparent compression, `st_blocks` overstates
  what reaches the device.

Statistics are available for local stores and for daemon stores that
advertise the `query-store-stats` worker-protocol feature. Other store
types (binary caches, SSH stores) report that stats are unavailable.

)""
