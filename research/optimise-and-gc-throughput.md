# Optimise + GC throughput: sharded `.links/`, replica spillover, parallel workers, io_uring

This branch ships four mostly-independent changes that share a
hot path (Nix's `.links/`-mediated content-address store):

1. an io_uring dispatch path for GC's bulk-unlink phases,
2. a 1024-way sharded `.links/` directory layout,
3. a replica/spillover scheme that lets a single content hash
   span multiple canonical inodes when the per-inode hardlink
   ceiling saturates,
4. parallel optimise / GC machinery (per-worker gc sockets,
   per-directory chmod batching, three thread-count knobs),

plus the bench infrastructure that justifies (and in places
disproves) each of them.

(2) and (3) are wired up in the code as orthogonal axes: a flat
store can use replicas, a sharded store can be configured
without replicas, and the bench rig exercises all four
combinations.

A fifth axis — **CoW reflinks as the sibling-dedup mechanism** —
was investigated end-to-end and rejected. Both the
implementation and the analysis are preserved in this document
(see "Reflinks: considered and dropped"), even though no reflink
code ships.

Every claim here was cross-checked against the sources in
`src/libstore/`, `src/libstore-tests/`,
`tests/nixos/nix-store-bench/`, and `tests/functional/`.

---

## TL;DR

- **`gc-links-use-io-uring`** (off by default) routes both
  Phase-2 orphan deletion and Phase-3 `.links/` cleanup through
  `io_uring_prep_unlinkat` instead of the thread-pool path.
- **`sharded-links`** (experimental feature, off by default)
  changes the `.links/` directory layout from one flat directory
  to 1024 shards indexed by a 2-character Nix-base32 prefix
  (`.links/<pfx>/...`). Strictly a directory-tree-shape axis —
  it says nothing about replicas. A one-shot in-place migration
  runs on first `optimiseStore` after enabling the flag.
- **`max-link-replicas`** (default 100) controls a replica /
  spillover walk: when the primary canonical's `st_nlink` hits
  the filesystem's per-inode hardlink ceiling, additional
  canonical inodes are allocated under `.<NN>` suffixes
  (`<hash>.01`, `<hash>.02`, …) so dedup can continue past the
  limit. **Independent** of `sharded-links`: replicas work in
  the flat layout (`.links/<hash>.<NN>`) and in the sharded
  layout (`.links/<pfx>/<hash>.<NN>`); the bench rig exercises
  all four combinations of {flat, sharded} × {single-replica,
  multi-replica}.
- **Parallel optimise/GC**: `optimise-threads`, `gc-links-threads`,
  and `gc-delete-threads` control the three relevant TBB worker
  pools. The per-thread gc-socket fix in `addTempRoot`
  (`tbb::enumerable_thread_specific<AutoCloseFD>`) recovers a
  ~135× speedup at the (16, 16) cell of
  `optimise_with_concurrent_gc` vs the previous shared-socket
  design.
- **All dedup goes through `link(2)`.** CoW reflinks via
  `ioctl(FICLONE)` were prototyped, benchmarked, and removed —
  the per-file syscall cost was higher than hardlinks while the
  semantic benefits (per-file metadata, accidental-write
  isolation, no per-inode hardlink ceiling) don't materialise
  on the filesystems that support reflinks, since those
  filesystems already lack the hardlink ceiling.
- **The bench rig** (matrix-registered VM tests, ad-hoc CLI,
  bpftrace-gated VFS/syscall counters, a Python decide script,
  and a re-shaped `gc_clusters` workload) is what actually
  resolves these design questions empirically.
- **Headline result**: io_uring alone does not beat the thread
  pool because both dispatches serialise on the same
  parent-directory `i_rwsem`. The structural win comes from
  splitting that lock — i.e. sharding `.links/`. Replicas help
  only when something actually hits the per-inode hardlink
  ceiling (ext4: 65000).

---

## What was implemented

### `gc-links-use-io-uring`: io_uring dispatch for GC's two unlink phases

A single setting toggles two independent code paths inside
`LocalStore::collectGarbage`. Both live in `src/libstore/gc.cc`
and are compiled in only when `HAVE_LIBURING` is set.

- **Phase 3 — `.links/` sweep (`cleanupLinksIoUring`)**:
  operates on the entry list `collectGarbage` has already
  enumerated via `forEachLinkEntry` (which walks both flat and
  sharded layouts). It submits a single high-QD ring of
  `IORING_OP_STATX` ops to read `st_nlink`, accumulates the
  same `actualSize` / `unsharedSize` totals the thread-pool
  path produces, then issues a second pass of
  `IORING_OP_UNLINKAT` for any entry observed with
  `nlink == 1`. ENOENT is tolerated on both passes (concurrent
  optimise / GC can race).

- **Phase 2 — orphan deletion (`cleanupOrphansIoUring`)**: each
  Phase-1-renamed orphan subtree is independent, so a TBB
  `parallel_for` walks the trees in parallel (purely CPU work:
  `readdir`/`lstat`, plus `chmod` u+rwx to match `_deletePath`
  on store-canonical 0555 directories), collecting every
  regular file/symlink into `files` and every directory into
  `dirs` in post-order. Two io_uring `UNLINKAT` passes follow:
  first `files` with `flags=0`, then `dirs` with
  `AT_REMOVEDIR`. Walk failures (e.g. ENOENT mid-iteration)
  skip pushing the dir into the rmdir batch so Stage C never
  hits a non-empty `AT_REMOVEDIR` target. `ignoreFailure=true`
  (the `ignore-gc-delete-failure` setting) downgrades all
  non-ENOENT unlink errors to a bounded number of warnings.

Both functions call `io_uring_register_iowq_max_workers`
best-effort (older kernels silently ignore) but with different
shapes: `cleanupOrphansIoUring` sets *both* bounded and
unbounded worker pools to `gcDeleteThreads * 16` because every
io-wq worker handles a file unlink; `cleanupLinksIoUring`
only sets bounded to `gcLinksThreads` (leaving the unbounded
pool default) because Phase-3 is bounded-only work. Both also
route the standard `EINTR`-during-`io_uring_wait_cqe` through
`checkInterrupt` so Ctrl-C surfaces as `Interrupted` rather
than a raw "interrupted system call".

The setting is plumbed via `LocalSettings::getGCSettings().gcLinksUseIoUring`
and defaults to `false`, so existing behaviour is unchanged
unless a caller asks for it.

### `Xp::ShardedLinks`: 1024-shard directory layout

Enabled by the `sharded-links` experimental feature (see
`src/libutil/experimental-features.cc`). Strictly a
directory-tree-shape axis — it says nothing about how entries
are named within a directory. When set:

- At LocalStore construction, all 1024 shard directories
  (`linksDir/<c1><c2>` for every pair of Nix-base32 characters)
  are pre-created. Each is empty until populated. Lazy creation
  would race `link(2)`/`mkdir(2)` between concurrent optimise
  workers.
- New canonical entries land under
  `<linksDir>/<pfx>/<filename>` rather than
  `<linksDir>/<filename>`, where `<pfx>` is the first two
  Nix-base32 characters of the hash. `<filename>` itself is
  governed independently by the replica axis (see the next
  section). `LocalStore::linkPathFor` is the single source of
  truth for combining both axes into an on-disk path.
- On the first `optimiseStore()` after the flag flips on,
  `migrateLinksDirToSharded()` runs once. It walks any
  remaining flat `.links/` entries — possibly including replica
  spillover entries (`<hash>.<NN>`) — strips the replica suffix
  to compute each entry's shard, then `link(2)`s it into the
  matching shard at the first free replica slot, `unlink`ing
  the source on success. Idempotent under crash recovery, and
  deliberately avoids `rename(2)` (which would atomically
  clobber a pre-existing destination and orphan every hardlink
  pointing at it).
- GC's `.links/` sweep (`forEachLinkEntry` in `local-store.cc`)
  walks both layouts unconditionally — a store can be toggled
  in or out of `sharded-links` between runs without leaking
  entries from the old layout.

### `max-link-replicas`: spillover past the per-inode hardlink ceiling

A separate axis from `sharded-links`. The replica/spillover
scheme is governed by two filename-level pieces and one
threshold:

- **Filename encoding**: `LocalStore::linkPathFor(hash, replica)`
  returns `<hash>` for replica 0 and `<hash>.<NN>` for replicas
  > 0 (zero-padded two-digit decimal). The directory part is
  separately chosen by the `sharded-links` axis, so the four
  on-disk forms are `<linksDir>/<hash>`,
  `<linksDir>/<hash>.<NN>`, `<linksDir>/<pfx>/<hash>`, and
  `<linksDir>/<pfx>/<hash>.<NN>`. Replica 0 having no suffix
  preserves backward compatibility with pre-`fffca655b`
  `.links/<hash>` entries.
- **Walk-side**: `optimisePath_` walks
  `0..min(max-link-replicas, maxReplicaSlots)` slots when
  trying to dedup a file. `maxReplicaSlots = 100` is a
  compile-time constant in `local-store.hh` (the encoding cap —
  `.NN` is two digits); `max-link-replicas` is the runtime
  setting (default 100, clamped to `maxReplicaSlots`). Setting
  it to `1` disables spillover entirely: the walk stops after
  the primary, and any EMLINK at that point leaves the file
  undeduped.
- **Spill threshold**: a candidate replica is skipped (spill to
  the next) when its `st_nlink >= linkMax`, where `linkMax` is
  the filesystem's per-inode hardlink ceiling probed via
  `pathconf(_PC_LINK_MAX)` at LocalStore open (ext4: 65000;
  tmpfs/XFS/ZFS: effectively unlimited). The `_link-max-override`
  test-only setting replaces `linkMax` when positive, so
  benches can force the spill path on filesystems that
  otherwise never trigger it.

With the encoding cap of 100 replicas and ext4's 65000-link
ceiling, a single content hash can dedup roughly 6.5 million
user files before optimise gives up. The decoupling from
`sharded-links` lets the bench rig enumerate all four cells of
the (layout × replicas) matrix — `flat_single_replica_hardlink`,
`flat_multi_replica_hardlink`, `sharded_single_replica_hardlink`,
`sharded_multi_replica_hardlink` — to attribute the
directory-layout cost separately from the spillover cost.

### Parallel optimise + parallel GC

Three orthogonal thread-count settings:

- `optimise-threads` — parallel `optimisePath_` calls inside
  one `optimiseStore` invocation.
- `gc-links-threads` — parallel `.links/` cleanup workers (the
  Phase-3 sweep), used by both the thread-pool and io_uring
  paths (the io_uring path uses it to size the io-wq pool).
- `gc-delete-threads` — parallel `_deletePath` workers
  (Phase 2 of GC), used by both the thread-pool and io_uring
  paths.

The per-thread gc-socket fix lives in `LocalStore::addTempRoot`
(in `gc.cc`). When a parallel `optimiseStore` runs against a
store with a concurrent GC, every per-path `addTempRoot` call
needs to round-trip over the gc-socket. The earlier shared
socket (`Sync<AutoCloseFD>`) serialised every worker through
one blocking writev+read; the
`optimise_with_concurrent_gc/(16, 16)` cell measured a 15×
**slowdown** vs (1, 1) under that design. The replacement
(`tbb::enumerable_thread_specific<AutoCloseFD> _fdRootsSockets`,
declared in `local-store.hh`) gives each TBB worker its own
connection, and the same cell now runs in ~24 ms on the
200-path fixture — a ~135× improvement over the pre-fix
baseline. See the docstring on `optimise_with_concurrent_gc` in
`optimise-bench.cc` for the exact comparison.

Also relevant: `optimisePath_`'s `DirWritability` RAII helper
batches the directory chmod toggles (a 0555-store directory
needs to go writable for an unlink+rename and then back to
0555). A directory with K children that all need replacing
used to cost 2K chmods + 2K journal commits; the helper makes
it 2 per directory.

### Bench harness (`src/libstore-tests/optimise-bench.cc`)

Built when `nix-store-tests` is overridden with `withBenchmarks = true`.

The bench fixture (`BenchFixture`) generates a synthetic store
of `nPaths` × ~10 files. Three properties:

- **Heavy-tailed in-degree distribution** for store-path
  references (`FixtureSpec::Topology::Barabasi`, the default
  for `optimise` / `optimise_with_concurrent_gc` /
  `gc_barabasi`).
- **Cluster topology** (`FixtureSpec::Topology::Clusters`) for
  `gc_clusters`: a small platform/base layer plus many
  isolated cluster DAGs of ~50 paths each, with only ~1% of
  clusters (by count) given a GC root. The unrooted clusters
  are genuinely unreachable, so `gcDeleteDead` actually deletes
  the bulk of the store. The BA topology can't model this
  workload: each random root pulls most hubs into its forward
  closure, so 1% roots in a BA graph leaves >99% of paths
  alive.
- **Variable file count and size**, deterministic per
  `(nPaths, avgFilesPerPath)` seed, with a 256-blob
  sqrt-Zipf-weighted content pool so dedup is meaningful.

Each bench function takes the seed parameters from
`state.range(...)` and the variant/dispatch from
`BENCHMARK_CAPTURE` args. The full matrix:

| Bench | Variant axis | Dispatch axis | Primary signal |
|---|:---:|:---:|---|
| `optimise` | yes | — | direct per-file dedup cost |
| `optimise_migrate` | hard-coded | — | one-shot `migrateLinksDirToSharded` |
| `optimise_with_concurrent_gc` | yes | — | optimise vs GC contention |
| `gc_barabasi` | yes | yes | GC on mostly-live store |
| `gc_clusters` | yes | yes | GC on mostly-dead store (bulk delete) |
| `invalidate_paths` | — | — | batched SQLite invalidation |

The `Variant` enum has four tags — the cartesian product of
layout × replicas:

- `flat_single_replica_hardlink` — flat layout, no replica spill
  (`max-link-replicas=1`). Pre-`fffca655b` baseline.
- `flat_multi_replica_hardlink` — flat layout, replica spill
  enabled (`max-link-replicas=100`). Compared against
  `flat_single_replica_hardlink`, the delta is purely the
  spillover cost (without the sharding axis confounding it).
- `sharded_single_replica_hardlink` — sharded layout, replica
  spill disabled. Compared against
  `flat_single_replica_hardlink`, the delta is purely the
  directory-layout cost (without the replica axis confounding
  it).
- `sharded_multi_replica_hardlink` — both axes on. Compared
  against the two single-axis cells, decomposes the contributions
  cleanly.

`SettingsGuard::applyVariant` (in the same file) is the single
point where a variant gets translated into live settings. It
toggles `Xp::ShardedLinks`, `max-link-replicas`, and
`_link-max-override`. For the two multi-replica cells it sets
`_link-max-override=100` so the spill path actually fires on
filesystems with effectively-infinite hardlink ceilings (tmpfs,
ZFS); the single-replica cells get 0.

The `Dispatch` enum is just `Syscall` vs `IoUring`; it only
applies to the two GC benches because there's no io_uring path
for `optimisePath_`.

Per-stage timings are surfaced through `OptimiseStats::Timings`
and `GCResults::Timings`, both filled by `StageTimer` (a tiny
RAII cursor in `src/libstore/include/nix/store/stage-timer.hh`).
The bench reads them via `accumulateStageTimings` and exposes
them as google-benchmark counters:

- Optimise: `t_setup_ms`, `t_migrate_ms`, `t_query_ms`,
  `t_load_ihash_ms`, `t_parallel_ms`.
- GC: `t_roots_ms`, `t_load_paths_ms`, `t_traverse_ms`,
  `t_phase2_ms`, `t_cleanup_ms`.

Plus a derived `t_total_ms` summed by the bench harness, which
equals google-benchmark's `Time` column by construction
(`UseManualTime()` + `SetIterationTime`).

Two harness-side hacks worth knowing about:

- **`truncateBenchTempRoots`**: between a warm-up `optimiseStore`
  and a timed `collectGarbage`, the bench truncates
  `<stateDir>/temproots/<pid>` to zero bytes. Without this,
  every path the warm-up touched is in the file, and GC's
  `findTempRoots` treats them all as live — Phase-2 deletion
  becomes a no-op and the bench silently measures nothing. The
  underlying issue is that `optimiseStore` legitimately calls
  `addTempRoot` for every path (so a concurrent GC in another
  process can't delete them mid-optimise); the bench owns the
  whole chroot, so erasing the tempRoots between the two
  in-process calls is safe.

- **`NIX_BENCH_STORE_ROOT`**: when set, `BenchFixture` uses the
  given directory as the store root and skips `remove_all` in
  the destructor. This is what `mk-test.nix`'s `multiProcess`
  scenario uses to run a real `nix-store --optimise` loop
  against the bench's store from a sibling process.

`bench-main.cc` calls `pthread_setname_np(pthread_self(), "nix-bench")`
so bpftrace's `comm == "nix-bench"` filter is stable across
binary renames (a different file name won't change the
self-set thread name).

### VM rig (`tests/nixos/nix-store-bench/`)

Four files form the rig:

- **`mk-test.nix`**: scenario module. Takes typed bench params
  — `fs`, `throttle`, `dmDelayMs`, `nPaths`, `threads`,
  `threads2`, `benchRepetitions`, `benchName`, `layoutDedup`,
  `cores`, `memorySize`, `diskSize`, `multiProcess` — and
  returns a NixOS test module. Handles the
  filesystem/throttle/dm-delay setup and the bpftrace
  attach/detach around each dispatch run.
- **`mk-bench.nix`**: shared library exposing `mkBench` (build
  one scenario as a derivation) and `mkBenchMatrix` (cartesian
  product over a scenario family). Used by both `default.nix`
  (Hydra-registered cells) and `adhoc.nix` (CLI one-offs), so
  the two paths can't drift.
- **`adhoc.nix`**: CLI entry point. Accepts the same
  parameters as `mk-test.nix` plus a `flakeRoot`, coerces
  string `--argstr` values to the right types, and returns a
  buildable test derivation with an auto-generated name.
- **`decide.py`**: packaged into the VM as `bench-decide`.
  Two modes:
  - A/B (4 args, used for GC benches): compares the
    syscall-mode and iouring-mode JSONs + bpf.txt files, prints
    a comparison table (`wall`, `p99`, `stddev`, `vfs`,
    `syscalls` per side), and a `VERDICT: PASS` / `VERDICT: FAIL`
    based on three criteria:
    - VFS op count parity between dispatches (within
      `VFS_PARITY_TOLERANCE = 5%`)
    - syscalls reduced under io_uring
      (`io_sys / sc_sys <= SYSCALL_REDUCTION_RATIO = 0.75`)
    - wall improved under io_uring
      (`(sc_wall - io_wall) / sc_wall >= WALL_IMPROVEMENT_MIN = 0.10`)
    All three must hold for PASS; any failure prints a bullet
    list and exits non-zero, which fails the NixOS test.
  - `--summary` (1 arg, used for non-dispatch benches): prints
    per-bench n/mean/median/p99/stddev/min/max over the
    iterations. Always exits 0.

The bpftrace block (inside `mk-test.nix`'s `testScript`) uses
six pieces:

1. Two `tracepoint:syscalls:sys_enter_openat` probes gated on
   `comm == "nix-bench"` and the marker filenames
   `/tmp/.bench_gc_start` / `/tmp/.bench_gc_end`. The bench
   opens these as `O_RDONLY`; they don't exist on disk, so the
   `open(2)` syscalls fail with `ENOENT`, but the openat
   syscall itself is enough to flip a global `@on` flag.
2. The start marker also records the bench's `pid` (which in
   bpftrace is the userspace process ID = kernel TGID, shared
   by every thread) into `@bench_pid`.
3. `kprobe:do_unlinkat` and `kprobe:vfs_unlink` count every
   unlink in the kernel during the gated window. `do_unlinkat`
   is the canonical entry point for both the `unlinkat(2)`
   syscall and io_uring's `io_unlinkat` op handler — a single
   probe catches both dispatch modes' kernel-side work, which
   is what makes the "did io_uring do the same amount of
   work?" parity check possible. These probes are scoped by
   `@on` alone, not by pid: io-wq kernel workers run on behalf
   of the bench under a different `pid`, and filtering by pid
   would drop them.
4. `tracepoint:raw_syscalls:sys_enter` counts userspace
   syscalls per syscall-id, gated by `@on == 1 && pid == @bench_pid`.
   Matching on `pid` (bpftrace builtin = TGID) includes every
   thread of the bench process, so TBB workers contribute
   their syscall count and the baseline isn't undercounted.

(The earlier version of this script used `tgid` and
`@bench_tgid`. `tgid` isn't a bpftrace builtin and bpftrace
aborts the program with `Unknown identifier: 'tgid'` before
any data is collected; the symptom was empty `.bpf.txt` files
and an error in `.bpf.log`. Don't reintroduce that.)

`packaging/hydra.nix` transposes the per-system NixOS tests
into the Hydra-conventional `tests.<name>.<system>` shape, so
each bench cell is reachable as
`hydraJobs.tests.bench-<bench>-<scenario>.{x86_64,aarch64}-linux`.

### Functional-test helpers (`tests/functional/common/functions.sh`)

Two shell helpers wrap the dedup invariants the optimise
functional tests assert:

- **`findCanonicalLink <path>`** — walks both `.links/` layouts
  (flat `<hash>[.NN]` and sharded `<pfx>/<hash>[.NN]`) and
  prints the entry whose `sha256sum` content matches the input
  file. Returns 1 if no match.
- **`assertDeduplicated <a> <b>`** — succeeds when the two
  paths share an inode (hardlink dedup), fails otherwise.
  All dedup goes through `link(2)`, so the check is a simple
  `stat --format=%i` comparison; an earlier draft of this
  helper supported a CoW-reflink branch (distinct inodes with
  shared extents, verified via FIEMAP) that was removed
  alongside the reflink mechanism itself.

The optimise tests use `findCanonicalLink` to locate the
`.links/<hash>` (or `<pfx>/<hash>`) entry and assert its
`nlink == N + 1` (N user-files + 1 entry name) where N is the
expected dedup degree.

---

## Findings

### 1. The original churn fixture wasn't measuring deletion

The first attempt at a "mostly-dead" GC benchmark reused the
Barabási–Albert preferential-attachment fixture from
`gc_barabasi` and tried to flip it from "mostly-live" to
"mostly-dead" by lowering `nRootsOverride` from `sqrt(N)` to
`N/100`. It didn't work: kprobe counts on `do_unlinkat` showed
six unlinks for a 50k-path workload regardless of nPaths or
filesystem. Two separate root causes:

- **BA topology doesn't produce dead paths.** Each new path
  attaches preferentially to high-in-degree existing paths
  (hubs), so picking *any* random late path as a root forces
  its whole forward closure into the alive set. With 1% of
  paths randomly rooted in a 50k-path BA graph, the union of
  closures covers nearly the whole graph through shared hubs.
  This is now spelled out on the `FixtureSpec::Topology` enum
  comment in `optimise-bench.cc`. The fix is the Clusters
  topology: a small platform/base layer plus many isolated
  cluster DAGs (clusterSize=50, default), only ~1% of which
  get a GC root at their cluster's "top". The remaining
  clusters are genuinely unreachable.
- **`optimiseStore` pins every path against in-process GC.**
  Inside `optimiseStore`'s `tbb::parallel_for_each` body, every
  worker calls `addTempRoot(path)` before `optimisePath_(...)`
  to keep a concurrent GC from another process from deleting
  the path mid-optimisation. The bench's warm-up
  `optimiseStore` therefore writes every path into
  `<stateDir>/temproots/<pid>`, and when the timed
  `collectGarbage` runs `findTempRoots`, it sees every path as
  live. The fix is `truncateBenchTempRoots` in
  `optimise-bench.cc` — between warm-up and timed call, the
  bench truncates its own temproots file to zero bytes. The
  bench owns the chroot, so it's safe; production users still
  get the original `addTempRoot` guarantee.

Together: cluster topology + tempRoots truncate let
`gc_clusters` actually exercise Phase-2 bulk deletion (>95%
of paths dead, Phase-2 fires for tens of thousands of orphans
on a 50k store).

### 2. io_uring Phase-3 only: wash to slight regression

Once the bench actually measured deletion, the io_uring port of
Phase-3 (`.links/` cleanup) showed roughly the following shape
on a mostly-dead 50k-path workload:

- On tmpfs (fastest backend, lock contention dominates): io_uring
  is consistently ~5–10% **slower** than the thread pool.
- On ext4-gp3 (3000 IOPS / 125 MiB/s + 5 ms dm-delay): io_uring
  is again ~5–10% slower at 50k. The 10k cell shows higher
  variance and an occasional outlier that flips the sign.

VFS-side counters confirm both dispatches do the same kernel
work: `do_unlinkat` event counts agree to within 0.01% between
syscall and iouring modes across every scenario. The dispatch
saving on the userspace side is real — `unlink(2)` count drops
from hundreds of thousands to near-zero, replaced by an
`io_uring_enter` count — but it doesn't translate into wall
time.

Two reasons:

- **Phase-3 alone is the minority of the work.** A
  mostly-dead 50k-path GC spends ~72% of its kernel time in
  Phase-2 `unlinkat(2)` from TBB workers in `_deletePath`,
  ~26% in Phase-3 unlinks on `.links/`, and ~2% in Phase-1
  rename-aside. Even a perfectly-batched Phase-3 caps the
  achievable wall speedup at ~25%.
- **The `.links/` directory's `i_rwsem` is the kernel-side
  bottleneck.** Both dispatches' workers — userspace pthreads
  in the thread-pool path, kernel io-wq workers in the
  io_uring path — serialise on the same per-inode lock. io_uring
  trades userspace dispatch cost for io-wq dispatch cost but
  doesn't relieve the contention. The structural i_rwsem
  bottleneck that motivated the io_uring port in the first
  place doesn't go away under io_uring.

### 3. io_uring Phase-2 also: 6–30% uniform regression

The natural next move was to port Phase-2 too: each orphan
subtree is independent (no shared parent inode across
orphans), so the lock argument shouldn't apply. The result is
`cleanupOrphansIoUring` (see above), gated by the same
`gc-links-use-io-uring` flag.

On the same fixture, Phase-2 + Phase-3 io_uring is uniformly
slower than the thread pool: roughly +5–15% on tmpfs and
+15–30% on ext4-gp3. VFS parity still matches (kernel work is
identical); userspace `unlinkat(2)` count drops to 3 (just the
marker syscalls).

Two structural reasons in addition to the same i_rwsem story:

- **Per-op pathwalk overhead.** The implementation uses
  `unlinkat(AT_FDCWD, abspath, …)` for every op, so the kernel
  walks ~8 path components per unlink
  (`/tmp/.../nix/store/.gc-N-hash/f-K`). The TBB path opens
  each orphan's directory fd once and reuses it via
  `unlinkat(parentfd, name)`, amortising the pathwalk across
  ~10 files per orphan. At ~1 µs of namei work per unlink,
  millions of unlinks add up to seconds of pure pathwalk
  overhead — visible primarily on tmpfs where I/O doesn't
  dominate.
- **Worker-per-orphan beats fanout for cache-warmth.** The
  TBB path runs `gcDeleteThreads` worker pthreads, each
  handling one orphan end-to-end. Within an orphan, the ~10
  file unlinks serialise on that orphan's parent-dir
  `i_rwsem`, but the worker thread keeps the lock cache-warm
  for its sequential unlinks. The io_uring path fanouts the
  same unlinks across many io-wq kernel workers, each
  acquiring `i_rwsem` from cold; we're trading sequential
  lock-warm passes for parallel lock acquisitions on the same
  lock and getting nothing in return (the kernel already
  serialises on each parent inode).

A pathwalk-optimised version (open parent fds during the
TBB walk, pass `parentfd + filename` to
`io_uring_prep_unlinkat`) might close the pathwalk gap but
wouldn't relieve the parent-`i_rwsem` problem. The expected
ceiling is matching the thread pool, not beating it.

### 4. Net for the io_uring port

On the bench, the io_uring path is **off by default for good
reason**:

- Phase-3 only: wash-to-slight-regression at scale.
- Phase-2 + 3: uniform 6–30% regression.

The original premise — "i_rwsem contention on `.links/` during
journaling on slow AWS EBS" — describes real kernel-side
contention. But moving the syscalls to io_uring doesn't relieve
it. The hot path is the kernel's serialisation on the parent
inode lock, not the userspace dispatch cost.

The setting and the code path are kept off-by-default but
present, mostly so a curious operator can A/B it on workloads
the existing matrix hasn't characterised (NVMe-over-network,
very different kernel versions, much larger fixtures). The
code is a few hundred lines of straightforward liburing usage
and doesn't cost much to maintain.

### 5. Sharded `.links/` is the structural fix

Splitting the lock — not the dispatch — is what unblocks real
parallelism. The `sharded-links` layout puts each canonical
entry under a 2-character shard prefix; each shard directory
has its own `i_rwsem`. Concurrent unlinks (and concurrent
canonical creates from `optimisePath_`) on different prefixes
proceed in parallel for both syscall *and* io_uring dispatch.

The four-cell bench matrix
(`{flat,sharded}_{single,multi}_replica_hardlink`) decomposes
the contributions: comparing `sharded_single_replica_hardlink`
vs `flat_single_replica_hardlink` isolates the directory-layout
effect with no spillover confound; comparing
`flat_multi_replica_hardlink` vs `flat_single_replica_hardlink`
isolates the spillover cost with no sharding confound; the
cross-comparisons cover the interactions. The two GC benches
(`gc_barabasi` and `gc_clusters`) cross the variant axis with
the syscall/iouring dispatch axis, so the full picture is
`4 × 2 = 8` rows per `(nPaths, threads)` pair.

### 6. Reflinks: considered and dropped

A fourth axis was prototyped end-to-end and bench-measured: have
`optimisePath_` use `ioctl(FICLONE)` (CoW reflink) instead of
`link(2)` for sibling dedup on filesystems that support it.
After the analysis below the implementation was removed; this
section captures what the bench data and the design analysis
showed.

**What the implementation looked like.** A `use-reflinks` setting
(default `true`), a `detectReflinkSupport` probe at LocalStore
construction (creates two scratch files, attempts FICLONE on a
4 KiB block), and a `canReflink` flag. When both
`canReflink && use-reflinks` were true, `optimisePath_`'s
`tryReplaceWith` branched between `link(2)` and a `reflinkCopy`
helper (`open` source O_RDONLY + `fstat` + `open` dest O_WRONLY|O_CREAT|O_EXCL
+ `ioctl(FICLONE)` + cleanup) — six syscalls per dedup vs
`link(2)`'s one. A "hybrid anchor" design kept GC's existing
`nlink == 1` deletion criterion bit-exact: `tryCreateCanonicalAt`
unconditionally used `link(2)` so the canonical's nlink was at
least 2 (anchor user-file + the `.links/` entry itself), even
when sibling user-files were reflinked.

**What the bench measured.** Per-file dedup cost (the `optimise`
bench, where `optimisePath_` is the hot loop):

- ext4 / tmpfs / non-reflink-capable FSes: probe fails, code path
  collapses to hardlinks. Same wall time as
  `flat_single_replica_hardlink`. No signal either way.
- btrfs / xfs(reflink=1) / bcachefs / ZFS(block_cloning):
  reflinks were 30–75% slower than hardlinks at small fixture
  sizes (2k–10k paths). On larger fixtures the I/O wait
  amortised the syscall overhead and the gap narrowed.
- `OptimiseStats::bytesFreed` over-counted: each pass re-reflinks
  every sibling because the `inodeHash` fast-path doesn't fire
  (each reflinked sibling has a distinct inode from the
  canonical). Cheap per-op (metadata-only — extents are already
  shared) but cosmetically wrong unless you switch the ioctl to
  `FIDEDUPERANGE`.

**What the design analysis showed.** Reflinks would buy four
things over hardlinks:

1. **No per-inode hardlink ceiling.** On hardlink dedup, ext4's
   65000-link ceiling forces a replica-spillover scheme. On
   reflinks each user-file has its own inode and the canonical's
   nlink stays at 2. But reflinks only exist on btrfs / XFS-with-
   reflink=1 / bcachefs / ZFS-with-block-cloning, and *those
   filesystems don't have a low hardlink ceiling*. The benefit
   doesn't materialise on the filesystems where reflinks are
   available. The ceiling problem is fully solved by replica
   spillover (`.links/<hash>.<NN>` up to 100 replicas ≈ 6.5M
   files per hash on ext4).
2. **Independent per-file metadata.** Hardlinks alias `st_mode`,
   `st_mtime`, `st_uid`, xattrs, etc. across every linked path;
   reflinks don't. But `canonicalisePathMetaData` normalises
   every store file to identical metadata (mode 0444, mtime=1,
   owner=root), so the aliasing has nothing to disagree about
   in practice.
3. **CoW isolation on accidental writes.** If something writes
   to a 0444 store path (chmod + write, or root, or a sandbox
   escape), hardlinks would propagate the write to every path
   sharing the content; reflinks would CoW-fork on the spot.
   Real semantic difference, but the store is 0444 by convention
   and on many setups read-only-mounted; the failure mode is
   rare enough that the cost of paying CoW overhead on every
   dedup is hard to justify.
4. **Backup / tooling ergonomics.** Some tools (rsync without
   `-H`, naïve `cp -a`, container builders) don't preserve
   hardlinks correctly. Reflinks look like normal files to
   those tools. Genuine but niche.

**Why we dropped it.** The Nix store is immutable by
convention — every entry is created once, canonicalised to
0444, and never modified. CoW's central semantic feature
(safe-to-write through one inode without affecting the others)
is unused. Meanwhile the per-file syscall overhead is real and
measurable, and the workaround for the cost (`fstat`
fast-path, FIDEDUPERANGE for idempotent re-reflink, an inode
breadcrumb in xattrs to short-circuit the `inodeHash` miss)
would be ongoing engineering surface for a feature that
doesn't pull its weight. The structural problem this branch was
chasing — `.links/` `i_rwsem` contention — is solved by
sharding the directory, not by changing the dedup mechanism.

A future Nix that wants to use CoW more deeply — store-level
snapshots, cheap experiment-fork-then-rollback, block-level
binary-cache replication via btrfs/ZFS send — would build on
the filesystem-feature layer, not on the `.links/` dedup layer.
Those are out of scope for this branch.

---

## How to test, benchmark, and run ad-hoc cells

### Local bench runs

Build the bench binary:

```bash
nix build --builders '' --no-link --print-out-paths --impure --expr '
  let f = builtins.getFlake (toString ./.); in
  f.packages.x86_64-linux.nix-store-tests.override { withBenchmarks = true; }
' -o /tmp/nix-bench
```

The bench binary is at `/tmp/nix-bench/bin/nix-store-benchmarks`.
Run a single cell:

```bash
TMPDIR=/tmp/bench-debug \
  /tmp/nix-bench/bin/nix-store-benchmarks \
  --benchmark_filter='gc_clusters/syscall_sharded_multi_replica_hardlink/2000/4/manual_time$' \
  --benchmark_repetitions=3 --benchmark_min_time=1x
```

A few patterns:

- `optimise/flat_single_replica_hardlink/` — all baseline thread-scaling cells
- `optimise/flat_multi_replica_hardlink/` — replica spill without sharding
- `optimise/sharded_(single|multi)_replica_hardlink/` — sharded × replicas axis at threads=4
- `gc_(barabasi|clusters)/(syscall|iouring)_flat_single_replica_hardlink/` — dispatch A/B on baseline
- `gc_clusters/(syscall|iouring)_sharded_multi_replica_hardlink/` — dispatch A/B on full sharded+multi-replica

The trailing `$` is a regex end-anchor; without it, longer
matching names can show up too.

`$TMPDIR` selects the test filesystem because `BenchFixture`
puts the store root under it. tmpfs (default `/tmp` on most
distros) is fastest and measures the CPU/VFS ceiling. Run on
ext4 / xfs / btrfs / bcachefs / ZFS to measure those backends.

`NIX_BENCH_VERBOSITY=4` re-enables Nix's `printInfo` /
`printError` (silenced by `bench-main.cc`) — useful for
debugging why a fixture isn't deduping or why GC isn't
deleting anything.

To verify dedup is actually happening end-to-end:

```bash
rm -rf /tmp/store && mkdir -p /tmp/store/nix/store
echo data >/tmp/x && echo data >/tmp/y
./result/bin/nix-store --store local?root=/tmp/store --add /tmp/x
./result/bin/nix-store --store local?root=/tmp/store --add /tmp/y
./result/bin/nix-store --store local?root=/tmp/store --optimise
stat --format="%i %h %n" /tmp/store/nix/store/*-x* /tmp/store/nix/store/*-y*
```

The two user-files should share an inode with `nlink >= 3`
(both user paths + the `.links/` entry).

### Reading the output

A typical row:

```
optimise/sharded_multi_replica_hardlink/10000/4/manual_time
   Time: 1820 ms   CPU: 6710 ms   Iters: 1
   items_per_second=109.89k/s
   t_setup_ms=0.024    t_migrate_ms=0.5    t_query_ms=8.4
   t_load_ihash_ms=42  t_parallel_ms=1769  t_total_ms=1820
```

- **Time** — wall-clock of one timed call, mean over
  iterations. Equals `t_total_ms` by construction
  (`UseManualTime` + explicit `SetIterationTime`).
- **CPU** — total worker-thread CPU time. `CPU / Time` is the
  achieved parallelism (with `optimise-threads=4` on a
  CPU-bound stage, expect ~4×; lower = serialised or I/O-bound,
  higher = spin without useful work).
- **Iters** — how many times the inner loop ran to hit
  `--benchmark_min_time` (usually 1 on multi-thousand-path
  fixtures; one `optimiseStore` already exceeds the default).
  Stage counters are per-iteration means in either case.
- **items_per_second** — `nPaths × nShared / Time`. Useful for
  ranking; the unit is fixture-specific.
- **`t_<stage>_ms`** — per-stage means. Sum to `t_total_ms`
  minus a small uncharged remainder (Activity ctor/dtor,
  parallel-arena teardown, etc.).

With `--benchmark_repetitions=N`, google-benchmark also emits
`<counter>_mean` / `_median` / `_stddev` / `_cv` aggregates, so
the stage counters get variance numbers for free.

Regressions to watch for, by stage:

- `t_setup_ms` shouldn't budge between cells. Movement implies
  lock-acquisition contention with another process or a
  `_PC_LINK_MAX` regression.
- `t_migrate_ms` is zero on every cell except
  `optimise_migrate`. Non-zero anywhere else means migration is
  being triggered on a fresh store — a regression in the
  "is anything flat?" check or in layout-toggle handling.
- `t_query_ms` scales with `nPaths`; a jump suggests
  `queryAllValidPaths` or SQLite plan regression.
- `t_load_ihash_ms` is sensitive to `.links/` topology. Flat
  with millions of entries is slow (htree depth); sharded is
  bounded. A jump on a small fixture points at
  stat/openat-per-entry inefficiency.
- `t_parallel_ms` dominates `optimise`. This is where the
  replica-spill cost and the sharded layout's per-directory
  scaling show up.
- `t_phase2_ms` dominates `gc_clusters`. Zero or near-zero on
  `gc_barabasi` is the expected shape (mostly-live, nothing to
  Phase-2-delete); a non-zero value on `gc_barabasi` means
  `truncateBenchTempRoots` has regressed.
- `t_cleanup_ms` is the `.links/` sweep — the only stage that
  responds to the `Dispatch` axis.

Variant comparisons (hold size and threads constant):

| Δ vs `flat_single_replica_hardlink` | Mechanism |
|---|---|
| `flat_multi_replica_hardlink` − `flat_single_replica_hardlink` | Cost of the replica-spill walk in the flat layout under `_link-max-override=100` |
| `sharded_single_replica_hardlink` − `flat_single_replica_hardlink` | Pure layout overhead (extra `readdir` traffic on shard subdirs, htree drift) |
| `sharded_multi_replica_hardlink` − `sharded_single_replica_hardlink` | Cost of the spill walk on top of the sharded layout |
| `sharded_multi_replica_hardlink` − `flat_multi_replica_hardlink` | Net effect of sharding when both have spill enabled |

### VM bench tests (Hydra cells)

Each registered cell is reachable as `.#hydraJobs.tests.<name>.<system>`
where `<system>` is `x86_64-linux` or `aarch64-linux`. The
matrix is defined in `tests/nixos/default.nix` via
`mkBenchMatrix` calls; scenario families are in the
`benchScenarios` let-binding. Currently:

- `base` (5 cells): canonical FS × throttle — `ext4-gp3`,
  `xfs-gp3`, `btrfs-gp3`, `zfs-gp3`, `tmpfs`.
- `flatMultiReplica` (2 cells): ext4 + tmpfs with
  `layoutDedup="flat_multi_replica_hardlink"`. Isolates the
  replica-spill cost without sharding.
- `sharded` (4 cells): ext4 + tmpfs × {single, multi}-replica.
- `ext4Sweep` (5 cells): canonical ext4 with one knob varied —
  `ext4-unthrottled`, `ext4-io2`, `ext4-nvme`, `ext4-gp3-delay1`,
  `ext4-gp3-delay20`.
- `gcThreadSweep` (2 cells): thread sweep on ext4-gp3 —
  `ext4-gp3-threads16` (`cores=24` so 16 worker threads aren't
  scheduler-thrashing on an 8-core VM), `ext4-gp3-200-threads1`
  (the `{50000, 1}` row isn't a registered `BENCHMARK_CAPTURE`
  arg, so this cell overrides `nPaths=200`). Both set
  `dispatchOnly = "syscall"` because the thread-sweep
  `BENCHMARK_CAPTURE` rows (the 200 cell, and `(<size>, 16)`)
  exist only on the syscall side; the iouring sibling for the
  baseline variant is `(<size>, 4)`-only.
- `multiProcess` (1 cell): `tmpfs-concurrent-optimise`. The
  noise loop (`bench-noise.service`, a parallel `nix store
  optimise` against the same root) touches a sentinel file
  after each pass; setup polls for the first sentinel so the
  bench doesn't race an empty `.links/`.

**Validation gaps to be aware of**: `mk-test.nix`'s eval-time
`validNPathsFor` rejects an `nPaths` that isn't in the registered
set for `(benchName, layoutDedup, dispatchOnly)`, but it does
*not* validate the `threads` (or `threads2` for
`optimise_with_concurrent_gc`) axis. If you set
`threads=8` on a scenario for which no `(nPaths, 8)` cell exists,
eval passes and the VM testScript's runtime hard-fail
("Failed to match any benchmarks") catches it instead. The
runtime check is the backstop; if you find yourself iterating
on adhoc invocations, prefer values you can see in
`BENCHMARK_CAPTURE` rows.

**Matrix budget**: the full registered matrix is ~25 VM cells
across `gc_barabasi` / `gc_clusters` / `optimise` /
`optimise_migrate` / `optimise_with_concurrent_gc` /
`invalidate_paths`. Each runs ~5–25 minutes depending on
`nPaths` × `dmDelayMs`; the ext4 `delay20` variant on 50 k is
the longest pole. The matrix as committed is meant for periodic
runs (Hydra cron, manual local sweep), not per-PR CI. Slot a
small subset — typically `{ext4-gp3, tmpfs}` × `{gc_barabasi,
gc_clusters, optimise}` — into PR CI; reserve the full set for
release-gating sweeps.

Each `mkBenchMatrix` call picks which families compose by
`//`-overlay. To build one cell:

```bash
nix build -j0 -L .#hydraJobs.tests.bench-gc-clusters-ext4-gp3.x86_64-linux
```

`-L` is important: VM tests are extremely chatty and the
default output truncation hides the bench's stdout. `-j0`
forces the work onto a remote builder when one is configured
(handy for VM tests that need lots of memory).

Result-dir layout:
- `{syscall,iouring}.json` — google-benchmark JSON.
  For benches without a dispatch axis it's `single.json`.
- `{syscall,iouring}.bpf.txt` — bpftrace counter dump (only
  for GC benches).
- `{syscall,iouring}.bpf.log` — bpftrace attach log. Empty
  `.bpf.txt` + an error here usually means the probe failed
  to attach (`CAP_BPF` / `ulimit -n` / a bpftrace syntax
  regression).
- `{syscall,iouring}.stdout` — bench stdout.

### Ad-hoc VM runs (`adhoc.nix`)

For combinations not in the registered matrix, use
`tests/nixos/nix-store-bench/adhoc.nix`. Both `--argstr`
(string) and `--arg` (typed) work; the file coerces strings.

```bash
nix-build --no-out-link --impure \
  tests/nixos/nix-store-bench/adhoc.nix \
  --argstr benchName gc_barabasi \
  --argstr fs btrfs --argstr throttle nvme \
  --argstr dmDelayMs 0 \
  --argstr nPaths 10000 \
  --argstr layoutDedup flat_multi_replica_hardlink
```

Or from Nix code:

```nix
import ./tests/nixos/nix-store-bench/adhoc.nix {
  benchName = "gc_clusters";
  fs = "zfs"; throttle = "nvme"; dmDelayMs = 0;
  nPaths = 10000; layoutDedup = "sharded_multi_replica_hardlink";
}
```

Test name is auto-generated as
`adhoc-<bench-with-hyphens>-<fs>-<throttle>` unless `name` is
passed.

**Valid `nPaths` values** are constrained by
`BENCHMARK_CAPTURE` rows in
`src/libstore-tests/optimise-bench.cc`. The thread-scaling
sweep is concentrated on the `flat_single_replica_hardlink`
baseline; the three replica/sharding variants register only
`(<size>, 4)` cells so the matrix size stays manageable.

- `gc_barabasi`, `gc_clusters`: 2000, 10000, 50000 for every
  (variant, dispatch) combo. The wider sweep —
  `(200, {1,4,16})`, `(2000, 16)`, `(10000, 16)`, `(50000, 16)`
  — is only on `flat_single_replica_hardlink/syscall`; the
  iouring side of the same variant is `(<size>, 4)`-only. The
  VM rig runs syscall and iouring back-to-back per scenario, so
  only the *intersection* of the two dispatches' cells is
  end-to-end runnable through `mk-test.nix` / `adhoc.nix`. Use
  the bench binary directly (host-side) to hit the
  syscall-only cells.
- `optimise`: full sweep on `flat_single_replica_hardlink` —
  200/2000/10000 × threads ∈ {1, 4, 16} plus 50000 × threads
  ∈ {4, 16}. The other three variants register only
  2000/10000/50000 × threads = 4.
- `optimise_migrate`: 2000 × threads ∈ {1, 4}, 10000 × 4,
  50000 × 4. Hard-coded variant (no `BENCHMARK_CAPTURE` tag).
- `optimise_with_concurrent_gc`: same asymmetry as `optimise`.
  `flat_single_replica_hardlink` carries every cell — 200 ×
  {(1,1),(4,4),(16,16)}, 2000 × {(1,1),(4,4),(16,16),(16,1),(1,16)},
  10000 × {(4,4),(16,16)}, 50000 × {(4,4),(16,16)}. The other
  three variants are 2000/10000/50000 × (4, 4) only.
- `invalidate_paths`: 100, 500, 2000, 10000, 50000.

If you pass an `nPaths` that doesn't match any registered cell,
`mk-test.nix` fails at Nix evaluation time with a message listing
the valid sizes for the chosen `(benchName, layoutDedup)`. A
second backstop in the VM testScript catches the remaining cases
(unsupported `threads` or `threads2` on an otherwise-valid size)
and fails the test with a "Failed to match any benchmarks" error
pointing at `optimise-bench.cc`. Both replaced the original silent
failure where the bench wrote empty JSONs and `decide.py` quietly
skipped.

### Reading the VM A/B output

For GC benches the rig invokes `bench-decide` on the two
JSONs + two bpf.txt files. Approximate shape:

```
=== gc dispatch comparison ===
syscall: wall= 4.225s  p99=... stddev=... vfs=12340 syscalls=27812
iouring: wall= 4.180s  p99=... stddev=... vfs=12341 syscalls= 8104

VERDICT: PASS
```

PASS requires all three of:
- VFS op count parity (within `VFS_PARITY_TOLERANCE = 5%`).
- iouring syscalls ≤ 75% of syscall mode's count
  (`SYSCALL_REDUCTION_RATIO`).
- iouring wall ≤ syscall wall × 90%
  (`WALL_IMPROVEMENT_MIN = 10%`).

Any failing criterion is printed as a bullet under
`VERDICT: FAIL` and the script exits non-zero, failing the
NixOS test. That's intentional: we don't want to ship
"io_uring is faster" when it's actually doing less kernel
work.

For non-dispatch benches the rig invokes `bench-decide --summary`,
which prints per-bench `n / mean / median / p99 / stddev / min / max`
rows and always exits 0.

### Functional tests

The optimise-related functional tests
(`tests/functional/optimise-store{,-race,-parallel}.sh`) use
the two helpers in `tests/functional/common/functions.sh`:
`findCanonicalLink` (locates the `.links/<hash>[.NN]` entry
across both layouts by sha256-matching against a user file)
and `assertDeduplicated` (same-inode test). They assert the
hardlink-mode `st_nlink == N + 1` invariant on the canonical
where appropriate.

---

## Known limitations / known issues

- **Migration is single-threaded.** `migrateLinksDirToSharded`
  walks flat entries one at a time. On a large pre-populated
  store this is the dominant cost the first time the
  `sharded-links` feature is flipped on. Subsequent optimise
  passes are fast.
- **Sharded layout pre-creates all 1024 shard directories at
  LocalStore open**, even on stores that may never grow large
  enough to populate them. Each shard is initially empty
  (cost depends on the filesystem — one inode and one or two
  directory blocks). Negligible on any modern filesystem but
  worth flagging for unusually small embedded stores.
- **`max-link-replicas` semantics change on toggle.** Lowering
  the runtime cap after a store has populated higher replicas
  doesn't migrate those entries back to the primary — they
  remain as additional canonical inodes that GC will sweep
  normally, but `optimisePath_`'s walk stops short of them on
  future passes. This is observable only as "previously
  deduped files stay deduped" — no correctness issue.

---

## Open questions / next steps

- A pathwalk-optimised Phase-2 io_uring port (open parent fds
  during the TBB walk, keep them alive across the submit
  batch, pass `parentfd + filename` to
  `io_uring_prep_unlinkat`) would close the per-op overhead
  gap vs the current TBB path, but wouldn't fix the
  parent-`i_rwsem` serialisation. Worth implementing only if a
  profile flags the pathwalk cost specifically.
- `clusterSize` (default 50 in the `Clusters` topology) is
  arbitrary. A 10/50/200 sweep would tell us how
  cluster-size-vs-Phase-2-worker-chunk-size trades off under
  throttle.
- Parallel `migrateLinksDirToSharded` — the migration loop is
  embarrassingly parallel per-shard-prefix. Would matter only
  on first-flip-on of a large pre-populated store.

---

## Pointers (where to look in the code)

**Implementation:**
- `src/libstore/include/nix/store/local-store.hh` —
  `shardedLinks`, `maxReplicaSlots`, `linkPathFor`,
  `migrateLinksDirToSharded`, `forEachLinkEntry`,
  `_fdRootsSockets`.
- `src/libstore/local-store.cc` — the LocalStore ctor (shard
  pre-creation, `linkMax` probe), `linkPathFor`,
  `stripReplicaSuffix`, `forEachLinkEntry`.
- `src/libstore/optimise-store.cc` — `optimisePath_`, the
  replica walk, `migrateLinksDirToSharded`, the parallel
  `optimiseStore` body that calls `addTempRoot`,
  `DirWritability`.
- `src/libstore/gc.cc` — `cleanupLinksIoUring`,
  `cleanupOrphansIoUring`, the `gc-links-use-io-uring`
  branches in `collectGarbage`, `addTempRoot` with
  per-thread sockets.
- `src/libstore/include/nix/store/local-settings.hh` —
  `maxLinkReplicas`, `linkMaxOverride`, `gcLinksThreads`,
  `gcDeleteThreads`, `optimiseThreads`, `gcLinksUseIoUring`.
- `src/libstore/include/nix/store/stage-timer.hh` — RAII timer
  used to fill `OptimiseStats::Timings` and `GCResults::Timings`.
- `src/libutil/{experimental-features.hh,experimental-features.cc}`
  — `Xp::ShardedLinks` definition.

**Benchmarks:**
- `src/libstore-tests/optimise-bench.cc` — `Variant` /
  `Dispatch` enums, `SettingsGuard::applyVariant`, the
  `FixtureSpec` model, `ContentPool::kHardlinkCap`,
  `truncateBenchTempRoots`, all six bench bodies, the
  `BENCHMARK_CAPTURE` matrix.
- `src/libstore-tests/bench-main.cc` —
  `pthread_setname_np` to `"nix-bench"` and the
  `NIX_BENCH_VERBOSITY` plumbing.

**VM tests:**
- `tests/nixos/nix-store-bench/mk-test.nix` — scenario module
  with the bpftrace block and the bench command.
- `tests/nixos/nix-store-bench/mk-bench.nix` — shared
  `mkBench` / `mkBenchMatrix`.
- `tests/nixos/nix-store-bench/adhoc.nix` — CLI one-off entry
  point.
- `tests/nixos/nix-store-bench/decide.py` — packaged as
  `bench-decide`; A/B mode + `--summary` mode.
- `tests/nixos/default.nix` — `benchScenarios` plus the
  per-bench `mkBenchMatrix` calls.
- `packaging/hydra.nix` — per-system → `tests.<test>.<system>`
  transpose.

**Functional tests:**
- `tests/functional/common/functions.sh` — `findCanonicalLink`,
  `assertDeduplicated`.
- `tests/functional/optimise-store.sh`,
  `tests/functional/optimise-store-race.sh`,
  `tests/functional/optimise-store-parallel.sh` — the updated
  tests.
