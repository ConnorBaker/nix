# `nix-store-bench`

A NixOS-VM rig for the `optimise-store` and GC throughput benches in
[`src/libstore-tests/optimise-bench.cc`](../../../src/libstore-tests/optimise-bench.cc).
Wraps the `nix-store-benchmarks` binary with filesystem / throttle /
dm-delay / bpftrace scaffolding so each cell measures the operation
itself, not the cost of repopulating a synthetic store.

## What the underlying patches do

Four mostly-independent changes share the `.links/`-mediated
content-address hot path; the bench rig exists to justify (and in
places disprove) each of them.

- **`gc-links-use-io-uring`** (off by default) routes both Phase-2
  orphan deletion and Phase-3 `.links/` cleanup through
  `io_uring_prep_unlinkat` instead of the thread-pool path.
- **`sharded-links`** (experimental feature, off by default) changes
  the `.links/` layout from one flat directory to 1024 shards indexed
  by a 2-character Nix-base32 prefix. A one-shot in-place migration
  runs on first `optimiseStore` after enabling the flag.
- **`max-link-replicas`** (default 100) controls a replica/spillover
  walk: when the primary canonical's `st_nlink` hits the filesystem's
  per-inode hardlink ceiling, additional canonical inodes are
  allocated under `.<NN>` suffixes so dedup can continue past the
  limit. Independent of `sharded-links`: replicas work in both
  layouts. The bench rig exercises all four combinations of
  `{flat, sharded} × {single-replica, multi-replica}`.
- **Parallel optimise/GC**: `optimise-threads`, `gc-links-threads`,
  and `gc-delete-threads` size three TBB worker pools. The
  per-thread gc-socket fix in `addTempRoot`
  (`tbb::enumerable_thread_specific<AutoCloseFD>`) recovers a ~135×
  speedup at the (16, 16) cell of `optimise_with_concurrent_gc` vs
  the previous shared-socket design.

All dedup goes through `link(2)`. CoW reflinks via
`ioctl(FICLONE)` were prototyped, benchmarked, and removed — see
"Rejected: CoW reflinks" below.

**Headline result**: io_uring alone does not beat the thread pool
because both dispatches serialise on the same parent-directory
`i_rwsem`. The structural win comes from splitting that lock — i.e.
sharding `.links/`. Replicas help only when something actually hits
the per-inode hardlink ceiling (ext4: 65000).

## File layout

| File | Role |
| ---- | ---- |
| `adhoc.nix`         | Entry point. Takes CLI / Nix args, runs `evalTest`, surfaces assertions + warnings, returns the test derivation. |
| `bench-options.nix` | Module schema. Declares every user-facing knob (with `type` / `default` / `description`) plus derived data (`useBlockDev`, `throttleParams`, etc.) and cross-option `assertions` / `warnings`. |
| `mk-test.nix`       | NixOS test module. Reads `config.bench.*`, emits `nodes.machine` and a `testScript` (constants prelude + `readFile ./testScript.py`). |
| `testScript.py`     | Bulk of the test driver — fixture setup, throttle daemon launch, bench invocation, bpftrace gating, result extraction. Name-bound by `mk-test.nix`'s prelude (see file header). |
| `decide.py`         | `bench-decide` CLI. Two subcommands: `summary` (one JSON) and `ab` (two JSONs + bpftrace dumps + threshold flags). |
| `throttle-daemon.sh`| In-VM daemon that toggles cgroup `io.max` + dm-delay around the measured call. Lock-step protocol with the bench's `ThrottleGate` C++ class. |

There are **no Hydra-jobset cells** here on purpose. The matrix of
(fs × throttle × layout × replica × benchName) is too large and the
`decide.py` thresholds are tuned for production-relevant fixture
sizes, not small-fixture CI smoke runs. Run scenarios via `adhoc.nix`.

## Eval flow

```
adhoc.nix
  │
  │  args@{ flakeRoot ? …, … }: view binding captures only the keys
  │  the caller actually supplied; schema defaults handle the rest
  ▼
nixos-lib.evalTest
  │  imports [ bench-options.nix, mk-test.nix, { config.bench = benchArgs; } ]
  │  (benchArgs = args minus the entry-point-only `flakeRoot` key)
  │
  ├──> bench-options.nix
  │      ─ declares options.bench.* (strict types) + assertions + warnings
  │      ─ computes derived data (useBlockDev, throttleParams, …)
  │      ─ derives bench.name default if not user-set
  │      ─ builds the assertions list
  │
  └──> mk-test.nix
         ─ reads config.bench.*
         ─ emits config.nodes.machine and config.testScript
         ─ testScript = constants prelude + readFile ./testScript.py

after evalTest returns:
  adhoc.nix
    ─ checks failedAssertions (throws on any)
    ─ lib.showWarnings on config.warnings
    ─ deepSeq config.bench (forces eager type checks)
    ─ returns config.test
```

The `runTest`-equivalent code is inlined into `adhoc.nix` rather than
in a separate `mk-bench.nix`, because there's only one entry point.

### Auto-derived test name

If the caller doesn't supply `name`, `bench-options.nix` derives one
from every distinguishing field:
`adhoc-<benchName>-<fs>-<throttle>-<layout>-<replica>-n<nPaths>-t<threads>[-multi]`.
Two scenarios that differ in any visible parameter get different
derivation hashes — no silent cache collisions.

### Why the assertions are surfaced manually

`runTest`'s nixosTest class does not declare `options.assertions` or
`options.warnings` (see `nixos/lib/testing/legacy.nix:11`). The
module declares both itself, and `adhoc.nix` reads them after
`evalTest` — `throw`ing on failed assertions and printing warnings
via `lib.showWarnings`.

## CLI examples

Run a default GC bench on ext4:

```
nix build --no-link -L -f tests/nixos/nix-store-bench/adhoc.nix
```

Override individual fields. The schema is strict about types: pass
string-valued options with `--argstr` and everything else (ints,
bools, null) with `--arg` plus a Nix expression.

```
nix build --no-link -L -f tests/nixos/nix-store-bench/adhoc.nix \
  --argstr benchName gc_clusters \
  --argstr fs btrfs --argstr throttle nvme \
  --argstr layout sharded --argstr replica multi \
  --argstr dispatchOnly syscall \
  --arg nPaths 10000 --arg threads 4
```

Two caveats on these flags:

- **Neither `nPaths` nor the thread axes are validated at eval time.**
  Pass a registered combo from `BENCHMARK_CAPTURE` in
  `src/libstore-tests/optimise-bench.cc`; unregistered values fail
  at run time with `Failed to match any benchmarks`. `threads = 4`
  is registered for every cell; `1` / `16` only on parts of the
  `flat_single_replica_hardlink` baseline (raise `cores` for `16`).

- **GC benches without `dispatchOnly` run a regression-gated A/B.**
  The test feeds both `syscall` and `iouring` JSONs into
  `bench-decide ab`, which checks three things: VFS-op parity (≤ 5%
  delta, a correctness check), syscall ratio (`b/a ≤ 1.05`, i.e. up
  to 5% more syscalls allowed), and wall improvement
  (`(a-b)/a ≥ -0.05`, i.e. up to 5% slower wall allowed). The
  defaults are a "no regression vs syscall path" gate, not a
  "demand a 10% win" gate — io_uring's per-worker-rings design
  hits parity with the syscall path on the canonical
  `gc_barabasi/ext4/flat/single` scenario; demanding a win would
  flag a passing run on the very scenario the code was tuned for.
  To demand an actual win for performance investigations, pass
  `--wall-improvement 0.10` etc. when running `bench-decide ab`
  outside the VM on the copied JSONs. A `VERDICT: FAIL` from the
  in-VM run is a real regression; check the printed `vfs=` parity
  first to confirm kernel work is still comparable. To skip the A/B
  step entirely (e.g. when iterating on something unrelated), pass
  `--argstr dispatchOnly syscall` (or `iouring`) to switch the
  in-VM step to `bench-decide summary` (no thresholds).

Programmatic (from another `.nix` file):

```nix
import tests/nixos/nix-store-bench/adhoc.nix {
  benchName = "optimise_with_concurrent_gc";
  threads = 4;
  threads2 = 4;
  fs = "ext4";
  throttle = "io2";
}
```

## `bench-decide`

After a test finishes, the JSON / bpftrace artefacts are decoded by
`bench-decide`:

```
# Summary of one JSON
bench-decide summary /tmp/syscall.json

# A/B with default thresholds (syscall vs iouring)
bench-decide ab /tmp/syscall.json /tmp/iouring.json \
                /tmp/syscall.bpf.txt /tmp/iouring.bpf.txt

# A/B with custom labels and loosened wall-time threshold
bench-decide ab a.json b.json a.bpf b.bpf \
                --a-name baseline --b-name patched \
                --wall-improvement 0.05
```

Run `bench-decide ab --help` / `bench-decide summary --help` for the
full flag list.

## Running the bench binary directly (no VM)

Build the bench binary:

```bash
nix build --builders '' --no-link --print-out-paths --impure --expr '
  let f = builtins.getFlake (toString ./.); in
  f.packages.x86_64-linux.nix-store-tests.override { withBenchmarks = true; }
' -o /tmp/nix-bench
```

Run a single cell:

```bash
TMPDIR=/tmp/bench-debug \
  /tmp/nix-bench/bin/nix-store-benchmarks \
  --benchmark_filter='gc_clusters/syscall_sharded_multi_replica_hardlink/2000/4/manual_time$' \
  --benchmark_repetitions=3 --benchmark_min_time=1x
```

Useful filter patterns:

- `optimise/flat_single_replica_hardlink/` — all baseline thread-scaling cells
- `optimise/flat_multi_replica_hardlink/` — replica spill without sharding
- `optimise/sharded_(single|multi)_replica_hardlink/` — sharded × replicas axis at threads=4
- `gc_(barabasi|clusters)/(syscall|iouring)_flat_single_replica_hardlink/` — dispatch A/B on baseline
- `gc_clusters/(syscall|iouring)_sharded_multi_replica_hardlink/` — dispatch A/B on full sharded+multi-replica

The trailing `$` is a regex end-anchor; without it, longer matching
names show up too.

`$TMPDIR` selects the test filesystem because `BenchFixture` puts
the store root under it. tmpfs (default `/tmp` on most distros) is
fastest and measures the CPU/VFS ceiling. Run on ext4 / xfs /
btrfs / bcachefs / ZFS to measure those backends.

`NIX_BENCH_VERBOSITY=4` re-enables Nix's `printInfo` / `printError`
(silenced by `bench-main.cc`) — useful for debugging why a fixture
isn't deduping or why GC isn't deleting anything.

## Reading the output

A typical row:

```
optimise/sharded_multi_replica_hardlink/10000/4/manual_time
   Time: 1820 ms   CPU: 6710 ms   Iters: 1
   items_per_second=109.89k/s
   t_setup_ms=0.024    t_migrate_ms=0.5    t_query_ms=8.4
   t_load_ihash_ms=42  t_parallel_ms=1769  t_total_ms=1820
```

- **Time** — wall-clock of one timed call, mean over iterations.
  Equals `t_total_ms` by construction (`UseManualTime` +
  `SetIterationTime`).
- **CPU** — total worker-thread CPU time. `CPU / Time` is the
  achieved parallelism (with `optimise-threads=4` on a CPU-bound
  stage, expect ~4×; lower = serialised or I/O-bound, higher = spin
  without useful work).
- **Iters** — how many times the inner loop ran to hit
  `--benchmark_min_time`.
- **`t_<stage>_ms`** — per-stage means. Sum to `t_total_ms` minus a
  small uncharged remainder (Activity ctor/dtor, parallel-arena
  teardown).

Regressions to watch for, by stage:

- `t_setup_ms` shouldn't budge between cells. Movement implies
  lock-acquisition contention or a `_PC_LINK_MAX` regression.
- `t_migrate_ms` is zero on every cell except `optimise_migrate`.
  Non-zero anywhere else means migration is being triggered on a
  fresh store — a regression in the "is anything flat?" check.
- `t_query_ms` scales with `nPaths`; a jump suggests
  `queryAllValidPaths` or SQLite-plan regression.
- `t_load_ihash_ms` is sensitive to `.links/` topology. Flat with
  millions of entries is slow (htree depth); sharded is bounded.
- `t_parallel_ms` dominates `optimise`. This is where the
  replica-spill cost and the sharded layout's per-directory scaling
  show up.
- `t_phase2_ms` dominates `gc_clusters`. Zero or near-zero on
  `gc_barabasi` is the expected shape; a non-zero value there means
  `truncateBenchTempRoots` has regressed.
- `t_cleanup_ms` is the `.links/` sweep — the only stage that
  responds to the `Dispatch` axis.

Variant comparisons (hold size and threads constant):

| Δ vs `flat_single_replica_hardlink` | Mechanism |
|---|---|
| `flat_multi_replica_hardlink` − `flat_single_replica_hardlink` | Cost of the replica-spill walk in the flat layout under `NIX_TEST_LINK_MAX_OVERRIDE=100` |
| `sharded_single_replica_hardlink` − `flat_single_replica_hardlink` | Pure layout overhead (extra `readdir` traffic on shard subdirs, htree drift) |
| `sharded_multi_replica_hardlink` − `sharded_single_replica_hardlink` | Cost of the spill walk on top of the sharded layout |
| `sharded_multi_replica_hardlink` − `flat_multi_replica_hardlink` | Net effect of sharding when both have spill enabled |

## Findings

### io_uring Phase-3 only: wash to slight regression

Once the bench actually measures deletion, the io_uring port of
Phase-3 (`.links/` cleanup) shows roughly:

- On tmpfs (lock contention dominates): io_uring is consistently
  ~5–10% **slower** than the thread pool.
- On ext4-gp3 (3000 IOPS / 125 MiB/s + 5 ms dm-delay): ~5–10%
  slower at 50k; higher variance at 10k.

VFS-side counters confirm both dispatches do the same kernel work
(`do_unlinkat` event counts agree to within 0.01%). Userspace
syscalls drop near-zero but wall time doesn't follow because the
`.links/` `i_rwsem` is the kernel-side bottleneck. Both dispatches'
workers — userspace pthreads vs kernel io-wq workers — serialise on
the same per-inode lock.

### io_uring Phase-2 also: 6–30% uniform regression

Each orphan subtree has an independent parent inode, so the lock
argument shouldn't apply. But the result is uniformly slower:
roughly +5–15% on tmpfs and +15–30% on ext4-gp3. Two reasons in
addition to the same i_rwsem story:

- **Per-op pathwalk overhead.** `unlinkat(AT_FDCWD, abspath, …)`
  walks ~8 path components per unlink. The TBB path opens each
  orphan's directory fd once and reuses it via
  `unlinkat(parentfd, name)`, amortising the pathwalk across ~10
  files per orphan.
- **Worker-per-orphan beats fanout for cache-warmth.** The TBB path
  keeps each orphan's `i_rwsem` cache-warm for its sequential
  unlinks; the io_uring path fanouts the same unlinks across many
  io-wq kernel workers, each acquiring the same `i_rwsem` from
  cold.

A pathwalk-optimised io_uring port (open parent fds during the TBB
walk, pass `parentfd + filename` to `io_uring_prep_unlinkat`) might
close the pathwalk gap but wouldn't relieve the parent-`i_rwsem`
problem. The expected ceiling is matching the thread pool, not
beating it.

### Net for the io_uring port

The setting and code path are kept off-by-default but present,
mostly so a curious operator can A/B it on workloads the existing
matrix hasn't characterised (NVMe-over-network, very different
kernel versions, much larger fixtures). The code is a few hundred
lines of straightforward liburing usage and doesn't cost much to
maintain.

### Sharded `.links/` is the structural fix

Splitting the lock — not the dispatch — is what unblocks real
parallelism. The `sharded-links` layout puts each canonical entry
under a 2-character shard prefix; each shard directory has its own
`i_rwsem`. Concurrent unlinks (and concurrent canonical creates
from `optimisePath_`) on different prefixes proceed in parallel for
both syscall *and* io_uring dispatch.

The four-cell bench matrix
(`{flat,sharded}_{single,multi}_replica_hardlink`) decomposes the
contributions: `sharded_single - flat_single` isolates the
directory-layout effect; `flat_multi - flat_single` isolates the
spillover cost; cross-comparisons cover interactions. The two GC
benches (`gc_barabasi`, `gc_clusters`) cross the variant axis with
the syscall/iouring dispatch axis.

### Rejected: CoW reflinks

A fourth axis was prototyped end-to-end: `optimisePath_` using
`ioctl(FICLONE)` instead of `link(2)` on filesystems that support
it (btrfs / XFS-reflink / bcachefs / ZFS-block_cloning). The bench
measured reflinks 30–75% slower than hardlinks at small fixture
sizes (six syscalls per dedup vs `link(2)`'s one); on larger
fixtures the gap narrowed but never reversed. The design analysis
showed the four reflink benefits don't materialise in practice for
a Nix store:

1. **No per-inode hardlink ceiling.** The ceiling problem is fully
   solved by replica spillover; ext4 (where it matters) doesn't
   support reflinks; reflink-capable FSes don't have a low ceiling.
2. **Independent per-file metadata.** `canonicalisePathMetaData`
   normalises every store file to identical metadata
   (mode 0444, mtime=1, owner=root), so hardlink aliasing has
   nothing to disagree about.
3. **CoW isolation on accidental writes.** The store is 0444 by
   convention and often read-only-mounted; failure mode is rare
   enough that paying CoW overhead on every dedup is unjustified.
4. **Backup/tooling ergonomics.** Real but niche.

A future Nix that wants CoW more deeply — store-level snapshots,
cheap experiment-fork-then-rollback, block-level binary-cache
replication via btrfs/ZFS send — would build on the
filesystem-feature layer, not on the `.links/` dedup layer.

## Known limitations

- **Migration is single-threaded.** `migrateLinksDirToSharded`
  walks flat entries one at a time. On a large pre-populated store
  this is the dominant cost the first time the `sharded-links`
  feature is flipped on. Subsequent optimise passes are fast.
- **Sharded layout pre-creates all 1024 shard directories at
  LocalStore open**, even on stores that may never grow large
  enough to populate them. Each shard is initially empty
  (cost depends on the filesystem — one inode and one or two
  directory blocks).
- **`max-link-replicas` semantics change on toggle.** Lowering the
  runtime cap after a store has populated higher replicas doesn't
  migrate those entries back to the primary — they remain as
  additional canonical inodes that GC will sweep normally, but
  `optimisePath_`'s walk stops short of them on future passes.

## Open questions

- A pathwalk-optimised Phase-2 io_uring port (open parent fds
  during the TBB walk, pass `parentfd + filename` to
  `io_uring_prep_unlinkat`) would close the per-op overhead gap vs
  the current TBB path. Worth implementing only if a profile flags
  the pathwalk cost specifically.
- `clusterSize` (default 50 in the `Clusters` topology) is
  arbitrary. A 10/50/200 sweep would tell us how
  cluster-size-vs-Phase-2-worker-chunk-size trades off under
  throttle.
- Parallel `migrateLinksDirToSharded` — the migration loop is
  embarrassingly parallel per-shard-prefix. Would matter only on
  first-flip-on of a large pre-populated store.

## Pointers

**Implementation:**
- `src/libstore/include/nix/store/local-store.hh` —
  `shardedLinks`, `maxReplicaSlots`, `linkPathFor`,
  `migrateLinksDirToSharded`, `forEachLinkEntry`, `_fdRootsSockets`.
- `src/libstore/local-store.cc` — ctor (shard pre-creation,
  `linkMax` probe), `linkPathFor`, `stripReplicaSuffix`,
  `forEachLinkEntry`.
- `src/libstore/optimise-store.cc` — `optimisePath_`, the replica
  walk, `migrateLinksDirToSharded`, the parallel `optimiseStore`
  body that calls `addTempRoot`, `DirWritability`.
- `src/libstore/gc.cc` — `cleanupLinksIoUring`,
  `cleanupOrphansIoUring`, the `gc-links-use-io-uring` branches in
  `collectGarbage`, `addTempRoot` with per-thread sockets.
- `src/libstore/include/nix/store/local-settings.hh` —
  `maxLinkReplicas`, `gcLinksThreads`, `gcDeleteThreads`,
  `optimiseThreads`, `gcLinksUseIoUring`.
- `src/libutil/{experimental-features.hh,experimental-features.cc}`
  — `Xp::ShardedLinks` definition.

**Benchmarks:**
- `src/libstore-tests/optimise-bench.cc` — `Variant` / `Dispatch`
  enums, `SettingsGuard::applyVariant`, the `FixtureSpec` model,
  `truncateBenchTempRoots`, all six bench bodies, the
  `BENCHMARK_CAPTURE` matrix.
- `src/libstore-tests/bench-main.cc` — `pthread_setname_np` to
  `"nix-bench"` and the `NIX_BENCH_VERBOSITY` plumbing.

**Functional tests:**
- `tests/functional/common/functions.sh` — `findCanonicalLink`,
  `assertDeduplicated`, `realiseFromExpr`.
- `tests/functional/optimise-store{,-race,-parallel}.sh`,
  `tests/functional/gc-{batched-invalidation,keep-flags,
  links-parallel,optimise-concurrent}.sh`.
