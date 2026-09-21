# The blob representation: the link, the holder index, and what decides between them

`01-specification.md` section 9.10 says where a regular file's blob lives: a file
of its own under `.objects/blobs/<xx>/<id>` (0444) or `.objects/blobs-x/<xx>/<id>`
(0555), and a store path's regular file is a hard link to it. That sentence costs
one `linkat` per regular file on every first entry of a tree. This document holds
the whole design space of the alternative — a blob's canonical file is a store
path's own, and the store keeps an index from identifier to holder — with what was
built, what each variant measured, and what decided it. The decision is taken
and section 6 records it with its gate; this document is the design space and
the measurements, and the invariants and laws as they now stand are
`08-store-model.md` and `01-specification.md` section 9.10. Where section 2
below differs from those — it is the proposal as it was put, with a foreign
key's cascade the shipped code does not have — the proposal is what it records.

The measurement drivers are `runs/linux-measure-first-entry-holder.nix` and its
script `runs/linux-first-entry-holder.sh`,
`runs/linux-measure-holder-index-scale.nix` and its program
`runs/linux-holder-index-scale.c`, `runs/linux-measure-holder-gc-verify-scale.nix`
and its script `runs/linux-holder-gc-verify-scale.sh`,
`runs/linux-measure-floor-write-and-link.nix`, and, on macOS,
`runs/macos-holder-first-entry.sh` with `runs/macos-floor-write-and-link.c`
(`runs/macos-first-entry-phases.sh` is the same discipline against master, and
`runs/macos-read-tree.c` the per-open control).

## 1. The question

**What it costs.** The branch's sequential first entry of a 40 000-file tree was
1.51–1.69 s against master-with-sharing-off's 1.37–1.49 s before the copy was
parallelised (`05-validation.md` section 6, *The copy's sibling directories on a
bounded pool*). The whole of that difference is one `linkat` per regular file into
`.objects/blobs/<xx>/<id>`: the microbenchmark
(`runs/linux-measure-floor-write-and-link.nix`) charges it **0.19 to 0.21 s per
40 000 files** on the measurement's ext4 over NVMe — about 5 µs a call `[derived:
0.20 s ÷ 40 000]` — where everything else the branch does is already cheaper than
master, 21.7 syscalls a file against master-with-sharing-off's 27.1 and
master-with-sharing-on's 36.1. A second, independent microbenchmark of the same
shape (`runs/linux-holder-index-scale.c`'s ancestor, the floor run beside the
holder prototype, three trials, aarch64-linux, ext4 over NVMe, 20 CPUs, load
3.3–3.7) writes 40 000 files in 40 directories exactly as the copy writes them and
then links each once: **0.938 s to write them, 1.199 s to write and link them** —
the link is **0.261 s** `[derived: the medians 1.199 − 0.938]`.

**What it costs on APFS.** On this Mac (Darwin 25.6.0, arm64, APFS, an
endpoint-security agent active), `runs/macos-floor-write-and-link.c` times each
call separately over 4 000 files, three trials: one `linkat` costs **248 to
436 µs** at one thread and **1 513 to 1 603 µs** at eight (flat and sharded alike). That is two orders of
magnitude above ext4's, and it is the whole of what the phase `create+link` adds
over `create` (0.405–0.525 s to write, 1.444–2.850 s to write and link). The same
program says the agent charges 76–477 µs for the `open` that creates a file, so
the link is not an outlier of the measurement: it is what this filesystem charges.

**What master does.** Master with `auto-optimise-store` off makes no such call and
shares nothing. Master with it on writes the file, then runs an optimisation pass
that re-hashes and re-links it through `.links` — which is why master's second
fetch is slower than its first (`05-validation.md` section 6). The branch's
decision comes before the write (`01-specification.md` section 10, *The object
store is unconditional*), so the link is all that is left of the sharing; master
with sharing off is the floor the branch is measured against.

So the question is exactly: **can the map from blob to file be held somewhere
cheaper than a directory entry, so that a first entry writes each file once?**

## 2. The alternative: a blob's canonical file is a store path's own

The canonical file of a regular-file blob is *a store path's own file* — a
**holder** — and the object store keeps an **index** from (blob identifier, mode)
to that file. `.objects/blobs` and `.objects/blobs-x` then hold no regular-file
blob; they hold symlink-target blobs only, which no path holds (`01` section 9.10:
"The symlink's target is a blob too, kept for the encoding's completeness (law 1)
and linked by nothing"). `.objects/trees` is unchanged. A first entry writes each
file once and records rows: **no per-file `linkat`**. A later ingestion that meets
a blob the index knows links the holder — the sharing, paid only when there is
something to share.

**The index.** In the SQLite database the store already has, not a file per shard.
The rows must become true exactly when the path becomes valid, and a path is
written, verified and registered in one transaction (`registerValidPaths`,
`local-store.cc`; `08-store-model.md` section 1.3), so a row written in that
transaction is atomic with the registration by construction. The schema as first
built:

```sql
create table if not exists Blobs (
    id    text    not null,   -- the blob's identifier, hex
    exec  integer not null,   -- the mode: 0 for blobs/, 1 for blobs-x/
    ino   integer not null,   -- the holder's inode
    dev   integer not null,   -- and its device
    path  integer not null,   -- ValidPaths.id of the holder's store path
    name  text    not null,   -- the holder's name relative to that path ("" for a plain-file root)
    size  integer not null,
    primary key (id, exec),
    foreign key (path) references ValidPaths(id) on delete cascade
);
create index if not exists IndexBlobsPath on Blobs(path);
```

`primary key (id, exec)` is `01` section 9.10's "one such file per (blob, mode) in
use and never one for both modes". `name` is relative to the store path because a
store path never moves.

**The collector.** `ValidPaths` rows are deleted by `invalidatePath` under
`pragma foreign_keys = 1`, and `Refs` and `DerivationOutputs` already cascade off
it, so `on delete cascade` needs no collector code at all: the rows go when the
path does, in the transaction that invalidates it. The case the question names is
a holder path deleted while **another path shares the inode**: the blob is still
held and the row that has just gone was the only record of where. Scanning the
store for a surviving holder is refused — it is the pass this design exists to
remove. The answer taken is *let it go and let it be repaired*: the next ingestion
that meets the blob misses, writes the file and becomes the holder, and
`nix-store --optimise`, which already walks every valid path and is already the
documented migration, restores every lost row and links inodes that have grown
apart back together. The sweep's blob pass then judges **symlink-target blobs
alone**, on the rule it already has; regular-file blobs are owned by paths and
collected with them.

**The invariants, each as it stands and as it would become.**

| | today | under the holder | direction |
|---|---|---|---|
| **K1** | a blob file is created as a second link to a path's file or from bytes through a temporary; never written in place | *split*: a symlink-target blob is still created from bytes through a temporary; a regular-file blob has no file of its own — its holder is a store path's file, canonicalised before the row that names it is written | weaker for regular files, unchanged for symlink blobs and trees |
| **K2** | a path's file is linked to a blob file only after the blob's size was checked, and its bytes too under repair; a failing file is removed from the object store first (`08` K2) | a path's file is linked to a **holder** only after the row's size was checked against the holder's `lstat`, its `ino`/`dev` against the row, and its bytes under repair; a failing holder is **not removed** — the *row* is deleted, since removing the file would damage a valid store path | **changed in kind.** Strictly safer (no valid path is touched); strictly weaker as a repair (the bad file stays until `--verify --check-contents --repair` reaches its path) |
| **K3** | a concurrent entry of one identifier is resolved by `place` trying twice, the loser linking to the winner's file | resolved by `primary key (id, exec)` and the transaction that writes it: the loser's insert sees the winner's row. `place`'s two attempts remain for the on-disk walk | **stronger**: a database constraint in place of a retry over `EEXIST` |
| **K4** | a blob file with one link is removed by the sweep unless a reachable tree names it | unchanged in words; its *domain* is the symlink-target blobs alone | unchanged, narrower domain |
| **K5**, **K6**, **K7**, **K10**, **K12**, **K14** | the trees, the corrupt-tree gate, the walk's pass over `.objects`, the bottom-up order, the lazy shards, the schema rule | unchanged | unchanged |
| **K8** | every regular file of a store path the store could link has a link count of at least two; the on-disk walk trusts an inode as a blob's only at two links and in the table (`08` K8) | **false, and this is the largest change.** A holder no second path shares is at **one** link. `EnterVisitor::known` must drop the `st_nlink < 2` test and trust an inode the index maps, whatever its count | **weaker**: the link count stops being a witness. What replaces it is the index — an inode is a blob's exactly when a row says so, which is a stronger statement than the link count ever was. `blobInodes` becomes `select ino, id from Blobs`, one query instead of a walk |
| **K9** | the synthetic root tree of an executable or symlink root is written so its blob is reachable | unchanged in words; for an executable root the blob is now the path's own file | unchanged |
| **K11** | whether the store places a file already on disk is decided in one place; a file `place` could not enter for `ENOSPC` is placed by neither and expected by the verifier | unchanged in structure. `ENOSPC` on a full directory index **disappears as a failure mode for regular files**, there being no directory to fill | **stronger**: law 3's fourth exception dissolves for regular files |
| **K13** | on the overlay store the merged store directory shows the lower store's objects; the sweep's removal is confined to the upper layer | **changed**: the index is the *upper* store's database and holds no row for a lower path, so a blob a lower path holds is written afresh into the upper layer. Today the lower blob file is visible through the mount and linking it triggers overlayfs's copy-up | **weaker**: less cross-layer sharing. Stated as a limit, not hidden |
| **K16** | the hold: a locked directory with one link per object written or reused, none for a regular file whose own store path's link holds its blob | unchanged in words and smaller in fact; it gains a second duty — it carries the ingestion's pending rows to the registration | unchanged |
| **K17** | the copy's subdirectories on a bounded pool; the object store's writers hold no state | **changed**: `place`, `enterWritten` and `link` touch the index. `link`'s point query goes through the store's `_state`-locked database (`08` section 1.1), and the pending rows go onto the hold, which must be thread-safe | **weaker for parallelism**, and section 5 is what that turned out to cost |

**Law 3.** `01` section 9.10 law 3's converse direction — two files with the same
bytes and bit have one inode, with four exceptions — gains a **fifth**: between a
holder's deletion and the next `nix-store --optimise`, a blob whose bytes a live
path still holds may be written a second time. It is bounded (one file's storage
per blob whose holder was collected *and* which is met again before the next
`--optimise`) and self-limiting (the second write becomes the new holder). The
forward direction — two files with one inode have the same bytes and bit — is
untouched.

**Law 4.** "Every object that hash reaches is present in the object store" becomes,
for a regular-file blob, *the index has a row for (id, mode) whose holder file
exists with that inode*. Note what it is not: not a check that the bytes are
somewhere else, because the file the verifier is walking *is* a file with those
bytes. What the row adds is that the store **knows** it, which is what a later
ingestion needs; a file whose row is missing is a file the store will write again,
and that is the defect the verifier reports and `--repair` fixes by inserting the
row.

**What is lost, three things.** (1) A blob's bytes stop being readable by
identifier without the database — `ls .objects/blobs/<xx>/<id>` answers today and
would not. Mitigated by the index being derivable: `nix-store --optimise` rebuilds
it from the paths, so it is a cache with a primary key, not a fact of record.
(2) The link count stops being a witness (K8). (3) Cross-layer sharing on the
overlay store (K13).

**The materialise route** is otherwise unchanged: per unchanged regular file it
still pays one `fstatat` and one `linkat`, with the index behind them — the `lstat`
it already pays becomes the holder's rather than the blob file's.

## 3. What was built

Each variant is named by what it does. Every one was built and measured.

- **The point probe.** The design as stated: one SQLite point query per regular
  file at the moment its last byte arrives, each its own read transaction, because
  the decision "does the store hold this blob?" is taken per file and cannot be
  deferred.
- **`txn`.** The same queries inside **one** read transaction held open for the
  whole copy. Two lines. *Not shippable as it stands*: `08` section 1.1 says
  nothing that walks the filesystem may run inside a transaction, and this does.
- **`prefetch`.** Every `(id, mode)` key read into memory once before the copy, so
  a probe costs a hash-set lookup and no SQLite call. *Not shippable at store
  scale* — one entry per distinct blob, in memory, per process. It is here as the
  **upper bound of every batching scheme**: nothing can beat "no call per file", so
  what `prefetch` does not win, no batching wins.
- **The batched `IN (…)` probe.** A directory's identifiers asked for in one
  statement. Measured as a probe cost in isolation (section 4) and **never built
  into the copy**; it is the shippable form the `prefetch` bound stands for.
- **The 32-byte identifier.** The index keyed by the identifier's bytes rather than
  its 64-character hexadecimal rendering, its rows written in key order.
- **The sharded pending set.** The hold's pending rows sharded 256 ways by the
  identifier's first byte, so that `addRow` and `pendingHolder` no longer take one
  global mutex per regular file from every worker of the copy's pool.
- **The `norows` ablation.** The holder probing the index exactly as it would but
  writing no rows. It is not a candidate; it is the instrument that separates "what
  the probe costs" from "what the write costs", and section 4 turns on it.
- **Design (b): the `Ingestions` indirection and the overlapped writer.** `Blobs`
  stops hanging off a foreign key into `ValidPaths`. An ingestion's rows name an
  `Ingestions` row, made with a null path when the copy starts and given its path
  at registration — one update of one row — so rows may be written *before* the
  path exists. The copy's workers build their own relative names and hand batches
  to the store as the hold's global counter crosses 16 384, in key order, outside
  the shards' locks. The cascade is replaced by a **stale-row sweep**: `link`
  forgets a row whose holder is not there, and `sweepStaleBlobRows` removes every
  row whose ingestion names no valid path, under the GC lock, in two statements; an
  ingestion in flight is protected as its objects already are, by its hold under
  `tmp/`. (Design (a), the alternative, staged the rows in a temp table and copied
  them in with one sorted `INSERT … SELECT` at the registration; it was costed and
  not built — section 4.)
- **The continuous writer.** The same rows drained by a writer *thread* at a
  threshold of 1 024 rather than by the copy's own workers at 16 384, so that the
  write is spread through the copy instead of landing in two bursts.

Two spellings of design (b) are worth keeping apart because the first was much
worse than the serial writer: a per-shard threshold of 64 with 256 shards and about
156 rows a shard fires twice a shard — about five hundred transactions where the
serial writer used three. The fix is arithmetic: count the batch over every shard,
not within one.

## 4. The measurements

Constants are Linux's; macOS gives direction only (`05-validation.md` section 5).
Every number below is read from the log the run produced.

### 4.1 The floor, before anything was built

`runs/linux-floor-write-and-link.c`'s shape with a SQLite side added — that
variant is **not in the tree**, the design it was written for not having been
taken: 40 000 files of about a dozen bytes in 40 directories, read and SHA-256'd from a source tree and written
exactly as the copy writes them, then either one `linkat` into the sharded blobs
directory or one row into a database carrying the local store's own pragmas
(`journal_mode = wal`, `synchronous = normal`, `wal_autocheckpoint = 40000`,
`foreign_keys = 1`). Three trials, aarch64-linux, ext4 over NVMe, 20 CPUs, load
3.30–3.66. Seconds:

| what | trial 0 | trial 1 | trial 2 | median |
|---|---|---|---|---|
| create the 40 000 files (the floor) | 0.989 | 0.938 | 0.877 | **0.938** `[derived]` |
| + one `linkat` each (the route today) | 1.272 | 1.199 | 1.088 | **1.199** `[derived]` |
| + one index row each, batches of 16 384 | 0.949 | 0.919 | 0.991 | **0.949** `[derived]` |
| + one index row each, one transaction for all | 1.131 | 1.052 | 0.814 | 1.052 `[derived]` |
| second ingestion: `lstat` blob + `linkat` (today) | 0.679 | 0.675 | 0.683 | 0.679 `[derived]` |
| second ingestion: index `select` + `lstat` + `linkat` | 0.769 | 0.611 | 0.741 | 0.741 `[derived]` |
| collection: sweep's blob pass (`lstat` 40 000, unlink) | 0.627 | 0.628 | 0.597 | 0.627 `[derived]` |
| collection: delete 40 000 rows by path | 0.064 | 0.066 | 0.062 | **0.064** `[derived]` |

Read off: the `linkat` costs **0.261 s** and the batched rows **0.011 s**
`[derived: the medians]` — inside the floor's own noise. Batching matters and the
table says so: one transaction for all 40 000 rows is *worse* than batches of
16 384. **This measurement is what the design was proposed on, and it is
incomplete**: it measured the index's *writes* and not its *probes*, and the probe
is one per file whether or not anything is shared. That is the caveat on method
that the rest of this section corrects; the correction is recorded rather than the
claim quietly dropped.

### 4.2 Linux first entry: the prototype against the route it replaces

Two rounds, orders reversed, three trials a label, N = 40 000, aarch64-linux, ext4
over NVMe, 20 CPUs; run 1 at load 3.54–4.41 and run 2 at 2.34–2.51. Seconds, and
the median of the six:

| binary | run 1 | run 2, reversed | median |
|---|---|---|---|
| holder, point probe, sequential | 1.45 1.55 1.48 | 1.42 1.47 1.42 | **1.460** `[derived]` |
| the route it replaces, sequential | 1.53 1.47 1.46 | 1.45 1.49 1.48 | **1.475** `[derived]` |
| holder, point probe, parallel | 1.03 0.97 1.01 | 0.98 1.00 0.99 | **0.995** `[derived]` |
| the route it replaces, parallel | 0.94 0.84 0.83 | 0.81 0.83 0.82 | **0.830** `[derived]` |
| master, sharing off | 1.45 1.33 1.37 | 1.41 1.27 1.28 | 1.350 `[derived]` |
| master, sharing on | 1.87 1.88 1.87 | 1.77 1.76 1.84 | 1.855 `[derived]` |

At N = 4 000 in the same run, all four are inside the machine's noise: holder-seq
0.19 0.17 0.20, tip-seq 0.19 0.19 0.21, master-off 0.19 0.19 0.16, master-on 0.24
0.23 0.20.

**The point probe does not deliver.** Sequentially it is a dead heat and still
0.11 s above master with sharing off; in parallel it is 0.165 s *worse* than the
route it replaces `[derived: 0.995 − 0.830]`.

### 4.3 The syscall account, which says why

`strace -f -c -w` of one whole first entry, N = 40 000, counts divided by 40 000:

| | `linkat` | `fcntl` | `pread64` | `pwrite64` | `newfstatat` | all calls |
|---|---|---|---|---|---|---|
| holder, point, sequential | 0 | **4.01** | 2.39 | 0.81 | 4.02 | **23.34** |
| holder, `txn`, sequential | 0 | **2.01** | 2.39 | 0.81 | 4.02 | **21.33** |
| holder, `prefetch`, sequential | 0 | **2.01** | 2.39 | 0.81 | 4.02 | **21.33–21.34** |
| the route it replaces, sequential | **1.01** | 2.01 | 2.14 | — | 4.03 | 21.72 |
| holder, `prefetch`, parallel | 0 | 2.01 | 2.39 | 0.82 | 4.02 | 21.44 |
| the route it replaces, parallel | 1.01 | 2.01 | 2.14 | — | 4.03 | 21.76–21.77 |
| master, sharing off | — | — | 2.00 | — | 7.02 | 27.11 |
| master, sharing on | 1.00 | — | 3.00 | — | 11.02 | 36.11 |

The `linkat` is gone exactly as designed. What replaced it is bigger: the +2.00
`fcntl` a file is SQLite's read-lock protocol, one read transaction per regular
file. The row *writes* are batched and nearly free — 0.81 `pwrite64` a file,
including the store's own registration. Under `txn` or `prefetch` the +2.00
`fcntl` is gone and the holder then issues **0.38 fewer syscalls a file** than the
route it replaces.

### 4.4 The batched probe on Linux: sequentially yes, in parallel no

Five rounds, orders alternated, three trials a label, N = 40 000. The builder was
busy throughout and two rounds ran at load 10–11, where every absolute doubles, so
**the reading is the within-round difference and not the absolutes**. Medians,
seconds; the load column is the two compared labels' own:

| round (load) | `prefetch` seq | tip seq | Δ | `prefetch` par | tip par | Δ | master off |
|---|---|---|---|---|---|---|---|
| p1 (8.4–8.7) | 1.44 | 1.53 | **−0.09** | 0.97 | 0.90 | +0.07 | 1.33 |
| p2 (6.9–7.1) | 1.56 | 1.75 | **−0.19** | 1.02 | 0.89 | +0.13 | 1.38 |
| q1 (8.6–9.9) | 1.59 | 1.86 | **−0.27** | 1.11 | 0.93 | +0.18 | 1.48 |
| q2 (9.6–10.0) | 3.44 | 3.61 | **−0.17** | 2.13 | 2.02 | +0.11 | 2.57 |
| q3 (10.0–11.0) | 3.26 | 3.36 | **−0.10** | 2.21 | 2.11 | +0.10 | 2.74 |

(All medians `[derived]` from the three trials of each label; the Δ columns
`[derived]` from those.)

**Sequentially the batched probe wins, by about the link: 0.09–0.27 s, five rounds
of five. In parallel it loses, by 0.07–0.18 s, five rounds of five** — and the
lock-free prefetch did not change that, so it is not contention on the set. **And
neither reaches master with sharing off**: the batched holder is above it by
0.11 s, 0.18 s and 0.11 s in the three rounds under load 10, and by 0.87 s and
0.52 s in the two above it, where every absolute doubled `[derived: the medians of
the table]`. So the premise "the entire difference is one `linkat`" is not quite
right either: removing the link closes about half the gap in the rounds where the
machine is quiet enough for the absolutes to mean anything.

### 4.5 The probe at a real store's size, matched on both sides

`runs/linux-holder-index-scale.c` fills a `Blobs` table and a sharded
`blobs/<xx>/<id>` directory of the same size on the same filesystem in the same
run, and times the probe for a miss (what a first entry does for every file) and
for a hit. aarch64-linux, ext4 over NVMe.

**At 10⁶ entries on each side** (load 2.79–3.84, three trials):

| per call | miss | hit |
|---|---|---|
| point query, id as text | **2.12 2.14 2.27 µs** | 3.53 3.56 3.60 µs |
| point query, id as a 32-byte blob | **2.09 2.13 2.15 µs** | 3.68 3.68 3.79 µs |
| batched `IN (…)`, 500 at a time | **1.73 1.75 1.83 µs** | 1.74 1.79 1.82 µs |
| filesystem `lstat` of `blobs/<xx>/<id>` | **3.17 3.17 3.46 µs** | **1.59 1.59 1.65 µs** |

**At 10⁷ entries on each side** (load 1.88–3.70, three trials, the two sides in two
runs on the same machine):

| per call | miss | hit |
|---|---|---|
| point query, id as text | **3.77 3.80 4.03 µs** | 6.27 6.29 6.31 µs |
| batched `IN (…)`, 500 at a time | **3.36 3.38 3.49 µs** | 3.40 3.41 3.44 µs |
| filesystem `lstat` of `blobs/<xx>/<id>` | **6.72 7.07 7.18 µs** | **2.00 2.01 2.01 µs** |

**This corrects section 4.3's reading.** At a real store's size the index's miss is
about 1.8 times cheaper than the directory's (3.8 against 7.0 µs at 10⁷) and
2.1 times cheaper batched; the hit is about three times dearer (6.3 against
2.0 µs). Section 4.3's "the filesystem directory is the cheaper index" was true of
the shape it measured — a *fresh* store, where every shard is empty and the `lstat`
is a negative dentry hit — and is not true at scale; what made the branch slower
there was WAL's per-statement lock protocol, which section 4.4's remedies remove.
The same ordering holds at 2·10⁶ on a busier machine (point miss 6.72–7.73 µs as
text, 5.22–5.93 µs as a blob; `lstat` miss at 2·10⁵ files 7.48–8.05 µs).

**What each map costs to have.** At 10⁶:

| | rows | files |
|---|---|---|
| building it | **7.8–8.0 s** as text, **4.3–4.4 s** as a blob | **9.3 s** |
| one path's 40 000 entries added | **0.281–0.435 s** | ≈0.37 s `[derived: 9.30 µs × 40 000]` |
| taking one path's 40 000 away | the cascade: **0.308–0.338 s** | part of the sweep |
| removing them all | not measured as one statement | `rm -rf`: **11.0 s** |
| bytes an entry | **196 B** as text, **115 B** as a blob | one inode and one dirent; not measured directly |
| the prefetch scan, per ingestion | **0.07 s** | — |

At 10⁷:

| | rows | files |
|---|---|---|
| building it | 182.4 s (**18.24 µs** an entry) | 144.1 s (**14.41 µs** an entry) |
| one path's 40 000 entries added | **0.679 0.728 0.787 s** | ≈0.58 s `[derived: 14.41 µs × 40 000]` |
| taking one path's 40 000 away | the cascade: **0.475 0.482 0.541 s** | part of the sweep |
| removing them all | not measured as one statement | 139.8 s (the sweep's floor) |
| bytes an entry | **199 B** as text; the database file is **1.85 GiB** | one inode and one dirent |
| the prefetch scan, per ingestion | **0.77 s** | — |

At 10⁸ the database is about 18 GiB as text and 11 GiB as a blob, and the prefetch
scan about 7 s `[extrapolated from the 10⁶ and 10⁷ ratios; not measured]`.

*Caveat on method, recorded once.* Two claims made here on a first pass were
measured wrong and are corrected above: that filling degrades badly as the table
grows (it is 7.8 µs a row at 10⁶ on a quiet machine and 18.2 µs at 10⁷ — 2.3× over
ten times the rows; the 28.5 µs seen at 2·10⁶ was measured while another job
contended for the same disk), and that one path's 40 000 rows would cost 1–4 s at
10⁷ (it costs 0.68–0.79 s, within about 30 % of the 40 000 links). A 10⁷ fill was
also once reported as "did not finish in three hours", implying SQLite was still
working; it was not — the build was **cancelled** (`error: Nix daemon disconnected
unexpectedly`), and the program's own progress was unknowable because its output
was block-buffered. Trimmed to one fill, it takes 182 s. **The two sides are
size-matched at 10⁷ and that is what the table above quotes.**

### 4.6 The registration's row write, in isolation

The isolated cost of the registration's row write, sorted by identifier as
`recordBlobs` sorts them, into an empty `Blobs` of the real shape with the store's
own pragmas (three trials, builder load 2.87):

| | trial 0 | trial 1 | trial 2 |
|---|---|---|---|
| one prepared statement, 40 000 binds | 0.060 | 0.042 | 0.044 |
| design (a), the part that would overlap: into a staging table | 0.011 | 0.012 | 0.012 |
| design (a), the part that stays serial: `INSERT … SELECT … ORDER BY id` | 0.023 | 0.023 | 0.025 |

**The whole insert is 0.042–0.060 s.** That is the arithmetic by which design (a)
was costed and not built: its floor is the ablation plus about 0.03 s.

### 4.7 `--verify` and `--gc`, at one path and at 10⁶ objects

At N = 40 000, one path, three trials, load 1.92–1.95, seconds:

| | holder | the route it replaces | master off | master on |
|---|---|---|---|---|
| `nix-store --verify --check-contents` | **0.37 0.37 0.37** | 0.56 0.55 0.54 | 0.19 0.19 0.20 | 0.35 0.34 0.34 |
| `nix-store --gc` of the 40 000-file path | **0.46 0.47 0.43** | 0.63 0.61 0.61 | 0.37 0.39 0.38 | 0.62 0.60 0.57 |

And the store afterwards, from the same run:

| binary | regular files under the store root | at two or more links | object files | index rows |
|---|---|---|---|---|
| holder | **40 041** | 0 | 41 (the trees) | 40 000 |
| the route it replaces | 80 041 | 80 000 | 40 041 | — |
| master, sharing off | 40 000 | 0 | 0 | — |
| master, sharing on | 80 000 | 80 000 | 0 | — |

At 10⁶ objects (a store made by one first entry of a 40 000-file tree, then
inflated: rows attached to that path for the holder, files under
`.objects/blobs/<xx>/` for the route it replaces; three trials, load 2.43–5.56):

| | holder | the route it replaces |
|---|---|---|
| inflating to 10⁶ | **10.1 10.3 11.9 s** | 20.9 21.8 23.0 s |
| `nix-store --verify --check-contents` | **0.69 0.71 0.82 s** | 30.85 31.09 33.46 s |
| `nix-store --gc` (the path and the 10⁶) | **9.42 9.42 11.17 s** | 20.60 26.83 27.65 s |
| the store's database | 187 MiB / 1 040 000 rows | — |

*Stated, because it matters:* the synthetic blob files of the route it replaces are
**empty**, so they do not hash to their names and its `--verify` also reports 10⁶
corrupt objects and exits 1. Its 31 s is therefore an over-estimate of a healthy
store's verification — but only the error reporting is spurious: the walk, the
`lstat` and the open of 10⁶ files are real, and the holder does none of them. The
collection figures carry no such caveat.

### 4.8 The parallel first entry at twenty trials a label

Three trials cannot separate an effect of 0.02–0.12 s on this builder: in the round
that comes closest to the gate a single label's twenty trials spanned 0.89–1.47 s,
and the first trial of a round is always an outlier. The rounds from **d** on are
therefore **ten trials a label in each of two orders**, all twenty kept, median of
the twenty; **a**, **b** and **c** are three and six, and are kept because they are
what located the cause rather than sized it. `hnr` is the ablation of section 3 —
the holder probing the index and writing no rows.

| round | what it measures | load | tip-par | hnr-par | holder-par |
|---|---|---|---|---|---|
| a | the first ablation, three trials only | 2.86–4.24 | 0.845 | 0.920 | — |
| b | the sharded pending set, six trials | 3.81–5.32 | 0.835 | — | 0.910 |
| c | locating what remains, six trials | 4.28–6.45 | 0.930 | 0.925 / 0.860 | 1.210 |
| d | the serial writer, twenty trials | 2.48–5.05 | **0.885** | **0.905** | **1.005** |
| e | design (b), a shard flushing at 64 unsent rows | 4.89–7.42 | 0.840 | 0.915 | **1.425** |
| f | design (b), the batch counted over every shard, 16 384 | 5.75–8.04 | 0.905 | 1.050 | **1.080** |
| g | design (b), the gate table | 4.14–6.92 | **0.840** | **0.850** | **0.925** |
| h | the continuous writer, threshold 1 024 | 2.75–5.07 | **0.930** | **0.995** | **1.250** |
| i | **the writer on a connection of its own** | **0.54–2.10** | **0.840** | **0.840** | **0.840** |

(Every median `[derived]` from the log's own trials; every load the range the log
recorded from the round's start to its end. The c-round's two `hnr-par` figures are
its two orders, which did not agree — the builder's load drifted from 4.3 to 6.5
*within* the round, and that is why the twenty-trial discipline replaced it.)

Spreads, for the three rounds the decision rests on: g — holder 0.89–1.47, hnr
0.79–0.93, tip 0.79–1.27; h — holder 1.12–1.79, hnr 0.86–1.16, tip 0.86–1.44;
**i — holder 0.79–1.48, hnr 0.80–1.06, tip 0.81–1.24, and excluding each label's
first trial in each round (always the outlier) every one of the sixty values
lies in 0.79–0.92**.

The i-round is the only one whose binaries were built *before* the builder was
polled quiet: the poller waited through 7.42, 4.68, 1.82 and 1.05 and the runs
then ran at load 0.54–2.10.  Every earlier round was measured on a machine its
own build had just loaded, which is why their absolutes drift and their deltas
do not.

What the ablation establishes, round by round:

- **The first attribution was wrong and the ablation says so.** Before the sharded
  pending set, `hnr-par` was *above* the tip in parallel (a-round: 0.920 against
  0.845) while being below it sequentially. So the index's write was not the whole
  of it.
- **The hold's single mutex was a real part of it.** `addRow` and `pendingHolder`
  took one lock per regular file from every worker. Sharded 256 ways, the parallel
  gap fell from about 0.10 s to about 0.075 s on the quiet pair.
- **With the sharded hold, everything except writing the rows is at the route it
  replaces.** In the d-round `hnr-par − tip-par` = **+0.020 s** and in the g-round
  **+0.010 s**; the whole of the remaining gap is `holder-par − hnr-par`, **0.100 s**
  with the serial writer (d) and **0.075 s** with the overlapping one (g).
- **The overlap works, and not reliably.** The f-round reads `holder-par − hnr-par`
  as 0.030 s and the g-round as 0.075 s; the two do not agree on how much, and the
  f-round ran at load 5.8–8.0 where every label inflated, so its absolutes cannot
  be compared with the g-round's. What is left unoverlapped in the g-round is
  arithmetic: two drains fire during a 40 000-row copy at a threshold of 16 384, so
  about 7 000 rows are still written serially by the ingestion's closing flush.
- **The gate is missed by 0.085 s** in the g-round `[derived: 0.925 − 0.840]`.
- **Making the write continuous makes it worse, not better**: `holder-par − hnr-par`
  goes from 0.075 s at a threshold of 16 384 to **0.255 s** at 1 024
  `[derived: 1.250 − 0.995]`, and `hnr-par − tip-par` from 0.010 s to 0.065 s.
  Section 5 is why.
- **And section 5's remedy closes it.** With the writer on a connection of its
  own, the i-round reads `holder-par − hnr-par` = **0.000 s** and
  `hnr-par − tip-par` = **0.000 s**: all three medians are 0.840
  `[derived from the round's twenty trials a label]`.  The index's write costs
  nothing measurable once it stops sharing a lock with the probe, which is what
  the h-round's negative result predicted.  **The gate — the parallel first
  entry at or under the route it replaces — is met.**

### 4.9 macOS, with the object-database read landed

`runs/macos-holder-first-entry.sh`, run outside the sandbox because the
measurement is of this machine: two binaries built here — **base** = the branch tip plus "a clean file's bytes are read from the object
database" (`01` section 13), **cand** = base plus the holder prototype with the
point probe — a clean, `git gc --aggressive`'d checkout so the object-database read
applies, a fresh store and fetcher cache each trial, binaries alternated, three
trials. `real` on this machine is not usable for a comparison on its own (the
agent's first-touch cost moved one binary's identical trial between 2.7 s and 25 s
at N = 10 000), so `sys` is the column and `user` is the control. Seconds:

| N | binary | real | user | **sys** |
|---|---|---|---|---|
| 4 000 | base | 1.351 1.333 | 0.771 0.765 | **5.825 6.147** |
| 4 000 | **cand** | **0.649 0.642** | 0.766 0.774 | **0.925 0.907** |
| 10 000 | base | 3.195 3.128 3.083 | 1.893 1.772 1.800 | **15.197 15.139 15.533** |
| 10 000 | **cand** | **1.415 1.375 1.376** | 1.828 1.841 1.841 | **2.219 2.187 2.158** |
| 40 000 | base | 12.768 12.816 12.823 | 7.327 7.534 7.360 | **58.814 59.348 58.172** |
| 40 000 | **cand** | **5.159 5.377 5.652** | 7.275 7.546 7.883 | **8.553 8.573 8.579** |

(At N = 4 000 the first trial of each binary is a warm-up on both and is not
quoted; it is 3.938 base and 3.046 cand.)

**The holder cuts the whole first entry on macOS by a factor of 2.0 to 2.4, on top
of everything that has landed** `[derived: the row pairs]`: 1.33 → 0.65 s at
4 000, 3.13 → 1.38 s at 10 000,
**12.8 → 5.4 s at 40 000**, and the kernel time falls from 58–59 s to 8.6 s at
40 000. `user` is identical, which is the control. The store afterwards at
N = 40 000: base 80 041 files and 40 041 object files, cand 40 041 and 41. The
earlier macOS run against the tip without the object-database read said the same
with larger absolutes (at 4 000, tip `sys` 5.977–6.714 s against holder
1.191–1.230 s; at 10 000 the copy phase alone, tip 13.766–16.006 s against holder
2.672–3.911 s), and needs no batched probe at all: SQLite's two `fcntl` cost
nothing beside a 248–436 µs `linkat`.

### 4.10 The edited copy: identical, and this is the boundary

The copy of an edited checkout, one file changed, three edits, on the same Mac and
the same two binaries. Seconds:

| N | binary | real | **sys** |
|---|---|---|---|
| 4 000 | base | 1.134 1.120 1.107 | 6.045 6.184 6.208 |
| 4 000 | cand | 1.120 1.077 1.111 | 6.210 6.125 6.179 |
| 10 000 | base | 2.498 2.452 2.482 | 15.194 15.287 15.377 |
| 10 000 | cand | 2.478 2.455 2.461 | 15.184 15.338 15.358 |
| 40 000 | base | 9.713 9.372 9.413 | 60.816 61.037 60.965 |
| 40 000 | cand | 9.604 9.587 9.454 | 61.058 61.312 60.501 |

**Identical, at every size, in both columns.** The edited copy goes through
`materialiseThroughObjects`, where every unchanged file is a `linkat` from
*something*: from the blob file on the route today, from the holder on the
candidate. The link is the sharing there, and both designs pay it. The holder
removes the link only where the branch makes one it need not — the *first* entry,
which writes a file and then links it into the object store as well. **The
operation the object store was built for gains nothing.**

## 5. The finding that decides the remaining gap

Section 4.8's last row is the one that locates what is left, and it locates it by
running the lever the wrong way. Making the index's write more continuous — a
writer thread draining at 1 024 rows instead of the copy's own workers draining at
16 384 — took `holder-par − hnr-par` from 0.075 s to **0.255 s**.

**The mechanism is the store's one database lock.** Every probe `link` makes takes
`_state`, once per regular file, from each of the copy's eight workers (`08`
section 1.1: every SQLite statement runs with `_state` locked). The writer takes
the same lock and holds it for a whole transaction. At 16 384 rows two such
transactions land inside a forty-thousand-file copy; at 1 024 there are forty, and
each one stops all eight workers' probes for its duration. **Making the write more
continuous does not overlap it with the copy; it interleaves it with the copy's
reads, which are on the same lock.**

So the index's write is not "serial work at the end that can be moved". It is work
that contends with the probe for one mutex, and moving it earlier trades a serial
tail for a contended middle. Neither of the two designs weighed in section 3
addresses that, because both keep writer and probe on the store's single
connection.

Two things would change it, and the measurements say what each is worth:

- **A second connection for the index.** SQLite in WAL admits one writer beside its
  readers, so a writer connection and a reader connection do not exclude each
  other; the writer's transactions would never take `_state`. This is the remedy
  the finding names. It is built and **its measurement is not in this document**;
  nothing below rests on it.
- **A per-directory batched `IN (…)` probe**, so that the probes stop touching the
  database per file. Measured as a probe cost it is the cheapest column of section
  4.5 — 1.73–1.83 µs an identifier at 10⁶ and 3.36–3.49 µs at 10⁷, cheaper than the
  `lstat` on both a miss and a hit — and it is **not built into the copy**. What
  argues against building it is section 4.8's own ablation: `hnr-par`, which probes
  per file and writes no rows, sits within 0.010 s of the route it replaces in the
  g-round, so per-file probing is not what costs the parallel copy anything, and
  batching it would be machinery against a cost that is not there. What argues for
  it is this section: the batched probe removes the probe from the lock, which is
  the other half of the contention the writer meets.

`prefetch` is not a candidate for either: 0.77 s of scan per ingestion at 10⁷ rows
(section 4.5). It stands only as the bound that says no batching scheme can win
more than it did.

## 6. The decision, taken

The decision was a condition, and the condition was the parallel first entry,
because that is the route the branch's best number comes from (`01` section 10,
*The copy walks a directory's subdirectories on a bounded pool*).

> **The holder representation is taken if and only if its parallel first entry of a
> 40 000-file tree is at or under the current route's, measured as twenty trials a
> label in each of two orders on a quiet builder. Otherwise the link stays.**

**The condition is met and the representation is taken.**  The i-round of section
4.8 — binaries built first, the builder then polled to 1.05, the runs at load
0.54–2.10, ten trials a label in each of two orders — reads all three medians at
**0.840 s**: `holder-par` at `tip-par`, with `hnr-par` between them at the same
figure.  Excluding each label's first trial in each round, always the outlier,
every one of the sixty values lies in 0.79–0.92.

What met it is one change, and the h-round is what named it: the index's writer
takes **a connection of its own**.  SQLite in WAL admits one writer beside its
readers, so the writer's transactions no longer take the lock the copy's probes
take; the h-round had shown that a writer sharing that lock makes the copy *worse*
the more continuous it is (`holder-par − hnr-par` 0.255 s at a threshold of 1 024
against 0.075 s at 16 384), and with the connection separated the same delta is
**0.000 s**.

Two things decided by the measurements and settled here:

- **The identifier is a 32-byte blob**, not 64-character hex, which halves the
  largest new artefact the design introduces (115 B a row against 196 B at 10⁶).
- **The per-directory batched probe is not built**, on the evidence of the
  ablation rather than by omission: `hnr-par` — which probes the index once per
  regular file and writes no rows — sits within 0.010 s of the route it replaces
  in the g-round and at **0.000 s** in the i-round.  Per-file probing costs the
  copy nothing, so batching it would be machinery against a cost that is not
  there.  Section 5's reasoning that the probe should leave the lock too was
  right about the *writer* and wrong about the probe, and the ablation is what
  distinguishes them.

The costs the representation carries, which the release note states: law 3's
fifth exception, K2's weaker repair, K8 falsified (the index is the witness, not
the link count), no cross-layer sharing on the overlay store, and a database of
1.85 GiB at 10⁷ blobs and about 18 GiB at 10⁸ `[extrapolated]`.

What the design buys, and what it costs, in one place:

| | Linux (ext4, NVMe) | macOS (APFS, with an endpoint-security agent) |
|---|---|---|
| first entry, parallel | **at the route it replaces**: 0.840 against 0.840, twenty trials a label, load 0.54–2.10 | not measured (the copy pool is the same code) |
| the whole first entry | — | **0.68 s faster** at 4 000 files, **1.75 s** at 10 000 and **7.4 s** at 40 000 `[derived: the medians]`, with 50 s of kernel time removed at 40 000 and `user` unchanged |
| the edited copy | gains nothing | **identical**, at 4 000, 10 000 and 40 000 |
| `nix-store --gc` | 0.15 s cheaper at one path (0.46 against 0.61) `[derived]`; **9.4 s against 20.6–27.7 s** over 10⁶ objects | — |
| `--verify --check-contents` | 0.18 s cheaper at one path (0.37 against 0.55) `[derived]`; **0.7 s against 31 s** over 10⁶ objects | — |
| files under the store root | **half** (40 041 against 80 041) | **half** (40 041 against 80 041) |
| the database | 1.85 GiB at 10⁷ blobs, opened by every command | the same |

The one measured statement that stands whatever else changes: **the `linkat`
cannot be removed by moving the blob-to-file map into the database unless the
writer leaves the store's one connection**, because the map is written while the
copy reads it, on the same lock, whether or not anything is shared.

## 7. What is left

The decision is taken (section 6) and the structural work is done; what remains is
the merge quality that follows it.  Items are struck from this list as they land,
so its length is the honest measure of what is left.

**Done, with the measurement or the mutation that pins each.**

- *The two structural fixes.*  The writer on a connection of its own is built and
  is what meets the gate (section 6).  The per-directory batched probe is **not**
  built, on the ablation's evidence, and section 6 records that as a decision
  rather than an omission.
- *The `norows` ablation is kept*, because it is what separates the writer's cost
  from everything else and what would catch a regression in either; it is a
  test-only switch and is named as one.
- *The two deterministic tests of the stale-row rule*, each with a mutation:
  `aCrashBeforeRegistrationLeavesNoRowThatMisleadsLink` (drop `and i.path is not
  null` from the lookup and it fails) and `theSweepJudgesARowByItsIngestionsPath`
  (drop the sweep's `pathExists(tmp / held)` test and it fails).
- *The two repair tests*, rewritten to pin what repair does to a *holder*: the row is forgotten, the freshly written file becomes the holder, and the corrupt file stays where it is, since removing it would damage a valid path.  Each keeps its discriminator in its own no-repair arm — the composite trusts the holder's size and links it, writing nothing — and `repairRelinksAFreshFileOverACorruptBlob` also loses a link, the holder being this path's own file with no second path sharing it yet (`08` K8).
- *The unit tests and functional scripts that pinned the old representation*, each rewritten to pin the statement that holds with the same discriminating power, none deleted without its replacement named, and one renamed where the rule's domain narrowed (`sweepKeepsASymlinkBlobALiveTreeNamesThoughNothingLinksIt`).  The list of scripts turned out to be **four** and not three: `object-store.sh`, `object-hash.sh`, `optimise-store.sh` and `git-hashing/object-store-fetch.sh`, the last of which the main suite's list had missed and the `git-hashing` gate found.
- *`nix-store --optimise`'s re-entry of a forgotten holder*, with its test (`optimiseReEntersAForgottenHolder`) and a functional block beside it.  It needed **no new code**: `optimiseStore` already opens an ingestion per path and names it at `endIngestion`, which was simply unpinned.  It is the repair law 3's fifth exception is bounded by.
- *The invariants, the laws and the tables.*  `08` K1–K4, K8, K9, K11, K13, K16, K17 and sections 1.3, 1.4, 1.5 and X2, each saying what became weaker, stronger or unchanged and citing the function; `01` section 9.10's *Where the objects live*, law 3's fifth exception, law 4 restated over the index, law 5's judgment of rows, and *One route*'s writer; `01` section 10's decision entry with the gate numbers of section 6; `04` section 1.7's composite — the index, the ingestion, the writer, the stale rule — and its verifier paragraph; `07`'s rows for the four defects this work found; `05` section 2's four scripts, section 4's mutation rows, section 5's counts, section 6's scale tables and section 7's `ENOSPC` entry, which the representation closes for a regular file; and the manual's release note and `nix-store --optimise` page.
- *The schema convention.*  The index owes no version bump — 11 was bumped because
  an existing column's *meaning* changed, and two new tables change no existing
  column — so it is an additive migration, and because a migration is recorded by
  name the migration drops and remakes the index's tables rather than altering
  them.  Dropping is sound because the index is a cache: every row is derivable
  from the store's own paths and `nix-store --optimise` rebuilds it.

**Not done.**

1. *The rest of the gate battery.*  What has run is in `05` section 5: the five
   unit binaries and the four functional suites on macOS, the `tests.run`
   derivations and the whole functional suite on Linux, and the mutation table's
   object-store rows, re-anchored (`runs/mutations.py check` reports
   `anchors bad: 0`) and re-run.  The **non-unity** Linux build of the five
   test packages passes at this tree, and **ThreadSanitizer** over the parallel
   copy and the store's materialise and restore routes -- which run the
   index's writer thread -- passes with no report (`05` section 5); the
   sanitised fetchers run passes as well, 6 of 6 with no report.
2. *The forced change run.*  `holderOrForget` removing the holder rather than
   forgetting its row fails four tests with `removing ".../a": Permission
   denied`: a store path's directory is read-only, so the removal a blob file
   of the store's own admitted is impossible for a holder, and K2's change of
   kind is pinned by the mutation and not only by the code (`05` section 4).
3. *The overlay run* (`runs/overlay-suite-on-host.sh`), which is the only thing
   that can say what this representation does to a merged mount: `08` K13's
   remaining limit, no cross-layer sharing of regular-file blobs, is by reading.
   The stale rows are no longer among the unknowns — the sweep's decomposition
   puts them outside the overridable half and a unit test pins it — but that
   the upper layer behaves as read is now measured: the suite runs ten of
   twelve, and `gc-objects` and `verify` fail -- the two that reach the
   representation through a merged mount; they are the next defects (`00`
   section 5).
