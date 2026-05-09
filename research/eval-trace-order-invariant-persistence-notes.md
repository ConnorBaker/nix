# Eval Trace Order-Invariant Concurrent Storage Engine

Date: 2026-04-08

## Executive Summary

This note replaces the earlier persistence plan with a stricter target:
the new storage architecture replaces SQLite as the semantic source of truth
for eval-trace state and removes the current attached-database / row-ID service
topology:

- `eval-trace-v14.sqlite`
- `attr-vocab.sqlite`
- `stat-hash-cache.sqlite`[^live-schema][^live-vocab][^live-stat-hash]

This is a rewrite, not a compatibility migration.
Breaking behavior changes are expected where the current implementation relies
on mutation history rather than semantic content.[^live-history-order]

The new architecture has five logical persistent namespaces:

1. `snapshot-v1`
2. `recovery-pack-v1`
3. `stat-hash-v1`
4. `attr-name-vocab-v1`
5. `attr-path-vocab-v1`

Portable semantic state is built from:

1. immutable content-addressed blocks;
2. fixed history-invariant ordered Merkle maps;
3. per-namespace root refs published by concrete file-lock + rename CAS;
4. a process-shared reader-pin registry synchronized with GC on pin
   transitions;
5. deterministic compiled vocab namespaces that are acceleration layers, not
   semantic truth.

Machine-local operational caches may use a narrower local backend where the
note later allows it.

This rewrite closes the main gaps from the previous adversarial review:

1. the atomic publication primitive is now concrete;
2. reader pinning and GC are concrete across processes;
3. subtree scans no longer depend on the broken `encode(P) || 0xFF` rule;
4. `NodeStamp` is replaced explicitly by pinned snapshot-root identity;
5. `trace_hash` is split from full trace-record object identity;
6. recovery winner selection is now specified as a deliberate semantic change;
7. the ordered-map wire format is fixed rather than left as a family of
   implementations;
8. the service boundary rewrite is first-class rather than deferred cleanup;
9. stat-hash revalidation is explicit instead of inheriting the current
   TOCTOU weakness;
10. recovery updates are incremental, not "rebuild the whole pack on every
   insert".

The design goal remains the same:
roots must be functions of current contents, not of insertion order or replay
history, in the same broad sense as uniquely represented search trees,
ATProto's deterministic MST, and other Prolly-style ordered Merkle maps.[^btreap]
[^atproto-repo][^dolt-block-store]

## Current Implementation Constraints

### Live Store Shape

The current eval-trace store persists:

- `Results`
- `DepKeySets`
- `Traces`
- `Sessions`
- `History`
- `SessionRuntimeRoots`
- `DirSets`[^live-schema]

The verifier uses them in this shape:

1. look up the current binding in `Sessions`;
2. on a miss, bootstrap from `History`;
3. verify the selected trace;
4. on failure, try GitIdentity recovery, direct-hash recovery, then
   structural-variant recovery.[^live-orchestrator][^live-recovery]

The storage engine must preserve that recovery strength even though it replaces
the row store completely.

### Live Concurrency Limits

The current persistence path is explicitly incompatible with a concurrent
evaluator:

- `TraceStore` is "not thread-safe";[^live-threading]
- serialization used to go through a `TraceStoreService` / `sqlite_strand`
  pair; that wrapper has been removed.  Serialization now goes through the
  `ExclusiveTraceStoreAccess` capability (minted only by
  `TraceStore::withExclusiveAccess`, holds the store-wide `storeMutex_`
  for the callback).  The semantics described below still apply: one
  exclusive-access holder at a time per store;[^live-threading]
- `AttrVocabStore` is a shared mutable dense-ID trie;[^live-vocab]
- `StatHashStore` was removed as dead code (zero production read-path
  hits); earlier prose that referenced it as a mutable global no longer
  applies.[^live-stat-hash]

Those are not peripheral implementation details.
They are exactly what this rewrite removes.

### Live Portability Failures

The current persistent bytes still embed local runtime identifiers:

- dep-key blobs contain local source, key, file-path, data-path, and attr-path
  IDs in native-endian packed structs;[^live-trace-serialize]
- result payloads encode local attr-name IDs and raw identity stamps;[^live-result-codec]
- materialization restores those identity stamps because runtime behavior
  depends on them;[^live-materialize][^live-identity]
- `AttrVocabStore::hashPath()` feeds host-endian `uint32_t` lengths into the
  hash stream.[^live-vocab]

The new persistent bytes must contain none of those local IDs.

### Live History Bias

The live recovery path is recency-biased:

- `scanHistoryForAttr` orders by `trace_id DESC`;[^live-history-order]
- bootstrap on a `Sessions` miss takes `history.front()`;[^live-history-order]
- GitIdentity recovery does `ORDER BY trace_id DESC LIMIT 1`.[^live-history-order]

That ordering is not history-invariant.
Any deterministic portable design must either preserve that bias with an
explicit semantic clock, or reject it.
This note rejects it.

The live direct-hash path also implicitly collapses duplicate `trace_hash`
matches to the first loaded history entry, which today means the most recent
one because history is loaded in descending `trace_id` order.[^live-history-order]
[^live-recovery]

### Live `NodeStamp` Use

The live current-binding layer uses `NodeStamp` because the store is mutable in
place:

- `Sessions` stores `node_stamp`;[^live-schema]
- `publishStateChange()` allocates a fresh `NodeStamp`;[^live-node-stamp]
- `resolveTraceContextHash()` memoizes on `(AttrPathId, NodeStamp)` to detect
  current-binding replacement.[^live-node-stamp]

In an immutable-root engine, the generation token is the pinned root hash
itself.
That lets `NodeStamp` disappear from persistent state completely.

## Design Goals

The new storage engine must satisfy all of the following.

1. Replace SQLite as the semantic source of truth for eval-trace state.
2. Keep persistent attr vocab only as a deterministic acceleration layer.
3. Make every persistent root invariant under insertion and deletion history.
4. Make reads safe under concurrent threads and processes.
5. Make writes safe under concurrent threads and processes.
6. Make crash consistency explicit and mechanically checkable.
7. Preserve exact-hit reuse and make exported eval artifacts substitutable
   through the existing Nix store/substituter path.
8. Preserve constructive recovery strength.
9. Preserve current identity-sensitive warm-hit behavior where the evaluator
   already depends on it.
10. Preserve a local stat-hash cache without reintroducing mutable singletons.
11. Specify honest complexity bounds, especially for structural recovery.
12. Avoid backward-compat shims, dual writes, and runtime bridges from
    semantic truth back to the current attached-SQLite topology.

## Architecture Alternatives

The earlier note argued against the current SQLite implementation, but it did
not clearly separate "SQLite is the wrong authority for semantic truth" from
"SQLite must not appear anywhere in the final system".

Three architecture families were considered:

1. SQLite for everything:
   - keep semantic current state, history membership, local caches, and remote
     import/export staging all in SQLite tables;
   - use file export/import only at the boundary.
2. hybrid:
   - semantic current state and history use immutable blocks plus published
     Merkle roots;
   - machine-local operational caches may still use SQLite or another local DB,
     as long as they are non-authoritative and rebuildable.
3. one file-based engine for everything:
   - semantic state and machine-local caches all use the same immutable
     block/ref/pin/GC machinery.

V1 chooses the second architecture.

That choice is deliberate:

- the note's strongest objections are to the current semantic topology:
  attached databases, row-ID service boundaries, one `sqlite_strand`, and
  mutable singleton state; they are not objections to SQLite's transaction
  model in the abstract;[^sqlite-atomic]
- semantic current state, recovery history, and portable evaluator artifacts
  need content-derived identity and root-level export/substitution semantics
  that fit immutable blocks plus published roots more directly than mutable SQL
  rows;
- machine-local operational caches such as stat-hash and tree-hash do not need
  portable Merkle identity just because snapshot/recovery state does.

Consequences:

- the design is no longer "anti-SQLite everywhere"; it is anti "SQLite as the
  semantic source of truth for order-invariant eval state";
- semantic namespaces (`snapshot-v1`, `semantic-recovery-v1`, `eval-view-v1`,
  portable packages) remain on immutable blocks plus explicit refs;
- machine-local, rebuildable caches may use SQLite-backed or memory-fronted
  implementations so long as they do not define semantic identity, portable
  export, or evaluator coherence;
- the document's harder protocol machinery must justify itself only where
  semantic roots, GC reachability, and portable transport actually need it.

### Memory-Only Authority Alternative

The live trace store already keeps large mutable indexes in memory and flushes
them to SQLite periodically.[^live-periodic-flush]
The rewrite could try to preserve that broad shape while replacing SQLite with a
memory-first metadata layer.

Three choices were considered:

1. memory-first authority with periodic durable flush:
   - keep current heads, history membership, or other semantic metadata in an
     in-memory store and periodically checkpoint them to disk;
2. centralized shared-memory or daemon authority:
   - move that in-memory authority behind one long-lived coordinator and treat
     disk as a checkpoint or replication target;
3. durable on-disk authority plus per-process hot mirrors only:
   - keep semantic truth in the same durable block/ref/pin authority that
     readers and writers validate directly, and allow only rebuildable
     process-local in-memory acceleration on top.

V1 chooses the third model.

That choice is deliberate:

- `DummyStore` is a fresh in-memory `Store` instance per open, backed by
  process-local concurrent maps; it is useful for tests and ephemeral data, but
  it is not a shared multi-process authority and it does not provide the
  ref/pin/GC crash semantics this note requires;[^dummy-store]
- exact coherent-head publication in this design depends on one durable success
  boundary, not a memory-accept point followed later by a disk-flush point;
- if memory-side publication succeeds but the process crashes before the durable
  flush, semantic refs, pins, and GC reachability can diverge;
- if multiple evaluator processes need one shared memory authority, the design
  would need a new daemon/shared-memory protocol anyway, which would simply move
  the hard concurrency and crash-consistency problem rather than removing it;
- periodic flush is appropriate for rebuildable acceleration layers, not for
  semantic snapshot/recovery/eval heads.

Consequences:

- v1 does not use `DummyStore`, in-memory SQLite, or any other memory-only
  store as the semantic authority for snapshot/recovery/eval refs, pins, or GC
  metadata;
- process-local verified-block caches, session registries, dynamic runtime-root
  mount overlays, and similar drop-on-exit accelerators remain allowed;
- machine-local operational caches may still use SQLite-backed or memory-fronted
  implementations so long as those caches are non-authoritative and rebuildable
  from durable state;
- `DummyStore` remains useful for unit tests, fuzzing, and ephemeral import /
  export assembly, but not for live multi-process coordination or periodic
  checkpointing of semantic truth.

## Chosen Storage Model

### 1. Immutable Block Store

The block store is content-addressed.
Each block is one canonical byte string whose hash is:

- `block_hash = BLAKE3-256(domain || kind_tag || payload_bytes)`

The block API is:

- `put(payload_bytes) -> block_hash`; idempotent[^git-hash-object][^ipfs-block-put]
- `get(block_hash) -> payload_bytes`

Writers may attempt the same block concurrently.
That is safe because the block identity is content-addressed.

Content addressability is also a correctness rule, not just a naming scheme:

- `put(payload_bytes)` computes `block_hash` from exactly the bytes it writes;
- `get(block_hash)` must not return bytes unless those bytes are verified to hash
  to `block_hash`;
- if the block file exists at the expected path but its bytes do not hash to the
  requested `block_hash`, that is hard corruption.

To keep reads fast, the implementation may keep a process-local verified-block
cache keyed by:

- `block_hash`
- file fingerprint of the opened block file (`dev`, `ino`, `size`, `mtime_sec`,
  `mtime_nsec`)

On a verified-cache hit for the same fingerprint, the implementation may reuse
the mapped bytes without rehashing.
On a miss, it must hash the bytes before returning them.

#### Engine Root Placement

The block/ref/pin engine also needs one concrete filesystem authority.

Three placement choices were considered:

1. daemon-owned store state:
   - place `<engine-root>` under the local store / daemon state directory and
     require daemon or worker-protocol extensions for every evaluator-visible
     ref/pin/GC operation;
2. evaluator-local cache root:
   - place `<engine-root>` under the existing eval-trace cache area
     (`getCacheDir()` today or an equivalent successor cache root) and let
     evaluator processes coordinate directly with file locks inside that root;
3. per-evaluation ephemeral temp roots only:
   - rebuild semantic state from scratch per evaluation and export it only at
     explicit boundaries.

V1 chooses the second placement.

That choice is deliberate:

- the current eval-trace implementation already persists under the evaluator's
  cache directory rather than the daemon-owned local-store state directory;[^live-cache-dir]
- the direct file-lock + rename + pin-file protocol specified in this note maps
  naturally onto a shared cache root visible to cooperating evaluator
  processes;
- it avoids making v1 correctness depend on extending the generic nix-daemon /
  worker protocol with new eval-trace-specific RPCs before the storage model is
  proven locally.

Consequences:

- `<engine-root>` is not a `StorePath`, not part of `/nix/store`, and not
  governed by the local-store GC or temp-root protocol;
- `<engine-root>` is scoped to one cache owner, not one whole machine:
  with today's `getCacheDir()` placement that normally means one user / one
  `NIX_CACHE_HOME` or `XDG_CACHE_HOME` domain, not a daemon-global shared
  authority;[^users-cache-dir]
- the eval-trace engine has its own reachability and stale-pin cleanup rules
  under `<engine-root>/gc.lock`;
- substituters and store closures remain transport for exported artifacts only;
  they do not become the live storage authority for current snapshot/recovery
  state;
- two different cache owners on the same host do not implicitly share live
  current snapshot/recovery/eval heads through v1; sharing happens only through
  explicit export/import or future daemon-mediated protocol extensions;
- a future daemon-mediated or multi-host design may wrap this engine behind
  protocol extensions, but that is explicitly out of v1 scope.

#### Concrete V1 File Layout

The v1 implementation should use one block file per hash:

```text
<engine-root>/
  blocks/
    aa/
      bb/
        aabbcc...<64 hex chars>.blk
  staging/
    <writer-id>/
      writer.lock
      <txid>/
        tx.lock
        aa/
          bb/
            aabbcc...<64 hex chars>.blk
```

During transaction construction, `staging/<writer-id>/` may also contain hidden
`.pending-<txid>/` directories that are not yet visible publication candidates.

Writers do not install newly created blocks directly into `blocks/` while those
blocks are still unpublished.
Doing so would recreate the classic concurrent-GC race that Git documents for
unreferenced objects: a collector can delete objects another process is using
but has not yet referenced.[^git-gc-race]

Instead, the concrete write path is:

1. create `staging/<writer-id>/writer.lock`, open it, and hold an exclusive lock
   on that file descriptor for the lifetime of the publication task;
2. create a hidden transaction directory
   `staging/<writer-id>/.pending-<txid>/`;
3. create `.pending-<txid>/tx.lock`, open it, and hold an exclusive lock on that
   file descriptor for the lifetime of the staged transaction;
4. write the full block bytes to a temporary file in the hidden transaction
   directory;
5. `fsync()` the file;
6. rename the temp file to the staged block path;
7. `fsync()` the staged parent directory;
8. rename `.pending-<txid>/` to visible `staging/<writer-id>/<txid>/`;
9. `fsync()` `staging/<writer-id>/`.[^in-tree-publish]

`writer-id` is a publication-task-local opaque identifier used only to partition
staging directories.
`txid` is a unique staged-publication attempt within one writer.
Neither value is semantic.

This is intentionally quarantine-shaped rather than age-based:

- staged blocks are invisible to published refs until they are migrated into
  `blocks/`;
- failed transactions can be discarded wholesale by removing their quarantine
  directory, matching the same basic pattern Git uses for quarantined incoming
  objects in `receive-pack`.[^git-receive-pack]
- the design does not rely on prune-age or mtime grace periods for correctness;
  Git documents those as mitigations, not as a complete concurrency solution.[^git-gc-race]

That is intentionally simple.
Packfile-like compaction can be added later as an internal optimization, and
the repo already has relevant pack-writing code in `git-utils.cc`, but the
semantic API remains `put/get` on immutable blocks.[^in-tree-pack]

#### Compaction Boundary

The earlier note mentioned packed-block storage as a future optimization without
stating whether v1 actually depends on it.

Three choices were considered:

1. loose blocks only in v1:
   - keep one durable loose file per block and let ordinary root-based GC
     reclaim unreachable loose blocks;
2. background live compactor in v1:
   - add a repacker immediately and allow it to rewrite physical block layout
     while readers, writers, and GC are all active;
3. offline-only packed storage:
   - require quiescence or explicit maintenance mode before any packed-layout
     rewrite.

V1 chooses the first rule.

That choice is deliberate:

- the correctness-critical problems in v1 are already refs, pins, GC, exact
  publication, and crash recovery;
- a live repacker would introduce a second publication problem at the physical
  storage layer unless it came with its own reachability, crash, and reader
  handoff protocol;
- one-file-per-block keeps the first implementation mechanically checkable and
  lets ordinary root-based GC reclaim orphaned loose blocks created by crashed
  or abandoned publication attempts.

Consequences:

- v1 does not require or assume a background block compactor;
- physical storage in v1 is loose-block only under `blocks/`;
- orphaned installed loose blocks that were migrated into `blocks/` before a
  crash but never became ref-reachable are reclaimed by ordinary root-based GC,
  not by a special repacker;
- any later packed-block implementation must remain a physical-layout
  optimization only:
  - `block_hash -> payload_bytes` remains the logical API;
  - refs, pins, and GC continue to talk about logical blocks and roots, not
    packfile membership;
  - pack indexes or manifests must not become a second semantic root space;
  - packed storage must not rewrite live packfiles in place beneath readers;
  - reclaiming old packed or loose physical containers must still be governed
    by root-and-pin reachability or an equivalent protocol with the same safety
    guarantees.

`staging/` is not scanned by normal GC.
Staged blocks become GC-visible only during the publish step below.

Staging cleanup is a separate liveness protocol:

1. opportunistic cleanup may enumerate `staging/<writer-id>/`;
2. it opens `writer.lock` and attempts a non-blocking exclusive lock;
3. if that lock attempt succeeds, the publication task is dead and the cleaner may remove
   the entire `staging/<writer-id>/` tree, including hidden pending
   transactions;
4. if that lock attempt fails, the writer is live; the cleaner must ignore any
   hidden `.pending-*` directories and may scan only visible `<txid>/`
   directories;
5. for one visible `<txid>/`, it opens `tx.lock` and attempts a non-blocking
   exclusive lock;
6. if that lock attempt fails, the transaction is live and must be left alone;
7. if that lock attempt succeeds, no live publisher owns that transaction, so
   the cleaner may remove the entire `<txid>/` directory tree and the now-stale
   `tx.lock` file.

This mirrors the same "lock file proves liveness; lock acquisition proves
staleness" pattern already used for temp roots and pin files in-tree.[^in-tree-gc]
[^in-tree-locks]

### 2. Per-Namespace Root Refs

There is no single mutable database file.
Semantic namespaces have one root-ref file each:

```text
<engine-root>/
  refs/
    eval/<eval-ref>.ref
    snapshot/<session-ref>.ref
    recovery-semantic/<recovery-ref>.ref
    recovery-compiled/<compiled-ref>.ref
    attr-name-vocab/current.ref
    attr-path-vocab/current.ref
```

Machine-local operational caches such as `stat-hash-v1` and
`tree-hash-cache-v1` are logical namespaces too, but they do not have to use
this physical ref-file layout.
If they use the semantic block/ref backend, corresponding `stat-hash` /
`tree-hash` refs are valid.
If they use a SQLite-backed, process-local memory-fronted, or equivalent local
cache backend, those logical namespaces are realized through that backend
instead.
Pure memory-only cache implementations are process-local accelerators, not the
cross-process semantic authority for those namespaces.

`<eval-ref>`, `<session-ref>`, `<recovery-ref>`, and `<compiled-ref>` are stable
hex digests of the serialized namespace key, used only to name the ref file.

For the coherent evaluator view, the namespace key is the tuple:

- `semantic_session_key`
- `stable_recovery_key`

For compiled recovery, the namespace key is the tuple:

- `stable_recovery_key`
- `base_semantic_root_hash`

The ref file payload stores the real serialized namespace key plus the current
root hash.

#### Concrete CAS Primitive

`compare_and_swap(ref, expected_root, new_root)` is implemented as:

1. acquire shared `gc.lock`;
2. acquire an exclusive lock on `ref.lock`;
3. read `ref` if it exists;
4. if `ref` exists, verify that its `root-ref-v1` payload matches the expected
   namespace kind, serialized namespace key, and supported format version;
5. if that verification fails, release `ref.lock`, release shared `gc.lock`,
   and fail with hard corruption;
6. compare its root hash to `expected_root`;
7. if it does not match, release `ref.lock`, release shared `gc.lock`, and
   fail;
8. install the staged blocks for this publication into `blocks/`:
   - staged blocks are already verified before publication because `put()`
     writes canonical payload bytes whose `block_hash` was computed from the same
     bytes;
   - if the final block path is absent, rename the staged block into place and
     `fsync()` the touched parent directory;
   - if the final block path already exists, discard the staged duplicate;
   - readers enforce block integrity through `get(block_hash)` verification and
     the verified-block cache above rather than by rehashing large block files
     inside the publish critical section.
9. write a temp ref file with the new `root-ref-v1` payload, deriving
   `namespace kind`, `serialized namespace key`, and `format version` from the
   publication context rather than trusting caller-supplied bytes;
10. `fsync()` the temp file;
11. rename the temp ref file over `ref`;
12. `fsync()` the parent directory;
13. release `ref.lock`;
14. release shared `gc.lock`;
15. remove the now-empty staging transaction directory.[^in-tree-locks]
    [^in-tree-publish][^in-tree-gc]
    [^git-update-ref]

This is the publication mechanism.
It is no longer "an implementation detail".

Operations that promise to return the exact root or coherent root pair that won
or matched a successful CAS use one fused exact-pin family:

- `compare_and_swap_and_pin_exact(ref, expected_root, new_root, exact_roots)`
- `confirm_current_ref_and_pin_exact(ref, expected_root, exact_roots)`

Two ordering choices were considered for the exact-pin family:

1. make roots visible in the caller's local refcount table first, then rewrite
   `pins/<pid>.pin`; or
2. keep the same durable-marker-first rule as the ordinary `0 -> 1` pin
   transition and rewrite `pins/<pid>.pin` before making those roots locally
   visible as pinned.

V1 chooses the second rule.

Consequences:

- the exact-pin helpers follow the same mental model as the ordinary pin
  transition protocol;
- the GC-trusted durable liveness marker is updated before the process treats
  the roots as locally pinned;
- exact-pin success updates the full process pin set by union rather than
  replacing it with one per-call scratch subset;
- the note no longer relies on an undocumented "special case while shared
  `gc.lock` is held" exception to explain exact-pin success paths.

Accordingly, before releasing shared `gc.lock`, the helper must:

1. rewrite `pins/<pid>.pin` to the canonical union of:
   - the process's already-pinned roots; and
   - `exact_roots`,
   so those exact roots become process-pinned without dropping unrelated roots
   the process already owns;
2. increment the caller's local pin refcount for each root in `exact_roots`;
3. only then release shared `gc.lock`.

This exact-root pin handoff must not reread the moving namespace head.
Its purpose is to close the gap between "CAS succeeded for root `R`" and
"process has pinned root `R`", even if another writer advances the same ref
immediately afterward.

`confirm_current_ref_and_pin_exact(...)` is the no-op-success companion:

1. acquire shared `gc.lock`;
2. read `ref`;
3. validate the existing `root-ref-v1` payload against the expected namespace
   kind, serialized namespace key, and supported format version for that ref;
4. verify that the current ref still points at `expected_root`;
5. if it does, rewrite `pins/<pid>.pin` to the canonical union of the
   process's already-pinned roots and `exact_roots`, increment the caller's
   local pin refcount for each root in `exact_roots`, and only then release
   shared `gc.lock`;
6. if it does not, release shared `gc.lock` and report mismatch without
   pinning.

This closes the dual gap between:

- "CAS succeeded for root `R`" and "process has pinned root `R`"; and
- "CAS lost, but the current head already equals `R`" and
  "process has pinned that exact current root under GC synchronization".

Replacement-view ownership for unchanged roots follows one additional rule.
Two choices were considered:

1. whenever an operation returns a replacement `EvalReadView`, re-pin every
   unchanged root for that replacement view and then drop the prior view; or
2. when the returned view keeps the same `snapshot_root_hash` and
   `semantic_root_hash` as the input view, transfer the caller's in-process
   ownership of those unchanged base roots from the input view to the
   replacement view without a `1 -> 0` / `0 -> 1` gap, and exact-pin only the
   changed roots.

V1 chooses the second rule for compiled-recovery preflight view replacement.

Consequences:

- replacement views stay GC-safe without gratuitous base-root pin churn;
- compiled-preflight helpers only need an exact-pin handoff for the compiled
  root they add or change;
- the returned replacement view still satisfies the `EvalReadView` invariant of
  being a pinned coherent view, because the unchanged base-root ownership is
  transferred rather than dropped.

If an implementation cannot transfer that unchanged base-root ownership
directly, it must reacquire those base roots under shared `gc.lock` before
releasing the prior view's ownership.

`recovery-semantic/<recovery-ref>.ref` and
`recovery-compiled/<compiled-ref>.ref` are separate refs with separate CAS
lifecycles.
That split is intentional:

- semantic recovery identity must not change when compiled acceleration is
  rebuilt locally;
- compiled recovery publication must not contend on the semantic recovery ref
  unnecessarily;
- compiled recovery state is bound to one semantic recovery root by namespace
  key, so `openEvalView()` cannot accidentally combine semantic root `S2` with
  compiled root `C1` that was built for older semantic root `S1`.

#### Why Per-Namespace Refs

The earlier note left open whether there was one global root catalog.
This note chooses per-namespace refs because they minimize unrelated write
contention:

- two writers updating different semantic sessions do not contend on one shared
  `snapshot` catalog lock;
- two writers updating different recovery namespaces do not contend on one
  shared `History` root;
- only writers to the same namespace key conflict.

This is closer to `git update-ref` than to a monolithic SQL transaction log:
small mutable pointers on top of immutable content.[^git-update-ref]

### 3. Reader Pins And GC

The previous note was underspecified here.
This note makes the process boundary explicit.

#### Process Pin Files

Each process owns one pin payload file and one liveness lock file:

```text
<engine-root>/
  pins/<pid>.pin
  pins/<pid>.lock
```

The process:

1. creates `pins/<pid>.lock`;
2. opens it;
3. holds an exclusive lock on that file descriptor for the life of the process;
4. rewrites `pins/<pid>.pin` by temp-write + rename whenever the pinned-root
   set changes.

The file contents are a canonical sorted list of currently pinned roots:

- namespace kind
- namespace key digest
- root hash

For recovery namespaces, semantic and compiled roots are pinned separately under
distinct namespace kinds:

- `recovery-semantic`
- `recovery-compiled`

Within a process, readers keep ordinary in-memory reference counts.
Pin transitions are synchronized with GC:

1. on the first local pin of a root (`0 -> 1`), acquire a shared `gc.lock`,
   publish the updated pin payload, then make the root visible as pinned in the
   local refcount table;
2. on the last local unpin (`1 -> 0`), acquire a shared `gc.lock`, publish the
   updated pin payload, then drop the local pin;
3. intermediate refcount changes that do not cross `0` or `1` do not touch
   `gc.lock`.

To reduce churn, `1 -> 0` unpins may be delayed or batched for a short interval.
That only postpones reclamation; it does not compromise correctness.

This is deliberately modeled on Nix temp-root files:
process-local ownership via a locked file, with GC consulting those live
markers.[^in-tree-gc]

#### Root Acquisition Protocol

Opening a read view must also synchronize with GC.

The low-level coherent read operation is:

- `openEvalView(semantic_session_key, stable_recovery_key)`

It is defined as:

1. acquire shared `gc.lock`;
2. read `refs/eval/<eval-ref>.ref` for the requested
   `(semantic_session_key, stable_recovery_key)`;
3. if that ref is absent, synthesize the canonical empty evaluator view for the
   requested keys:
   - empty `snapshot-v1` with empty `bindings_root`, empty `runtime_roots_root`,
     and `snapshot-meta-v1` keyed to `semantic_session_key`;
   - empty `semantic-recovery-v1` with empty `members_root`, empty
     `candidate_objects_root`, and `recovery-meta-v1` keyed to
     `stable_recovery_key`;
   - empty `eval-view-v1` that references those empty roots and an
     `eval-view-meta-v1` keyed to both requested namespace keys;
   - no compiled recovery root;
   - implementations may `put()` those canonical empty immutable objects before
     pinning them, but they must not publish any namespace ref as part of this
     absent-namespace read path;
   - do not probe `refs/recovery-compiled/<compiled-ref>.ref` for this
     synthesized-empty case;
4. if the eval ref exists:
   - verify that its `root-ref-v1` payload matches the expected namespace kind,
     serialized namespace key, and supported format version;
   - load the referenced `eval-view-v1` object;
   - load its `eval-view-meta-v1` object and verify that both embedded namespace
     keys match the requested `semantic_session_key` and `stable_recovery_key`;
   - record `snapshot_root_hash` and `semantic_root_hash` from that
     `eval-view-v1` object;
   - load the referenced `snapshot-v1` object and its `snapshot-meta-v1`, and
     verify that the embedded `semantic_session_key` matches the requested
     namespace key;
   - load the referenced `semantic-recovery-v1` object and its
     `recovery-meta-v1`, and verify that the embedded `stable_recovery_key`
     matches the requested namespace key;
5. derive the compiled recovery namespace key
   `(stable_recovery_key, semantic_root_hash)`;
6. read `refs/recovery-compiled/<compiled-ref>.ref` for that exact namespace
   key only;
7. if a compiled ref is present:
   - verify that its `root-ref-v1` payload matches the expected compiled
     namespace kind, serialized namespace key, and supported format version;
   - load its `compiled-recovery-v1` root object;
   - load its `compiled-recovery-meta-v1` object and verify that:
     - `compiled-recovery-meta-v1.stable_recovery_key == stable_recovery_key`;
     - `compiled-recovery-meta-v1.base_semantic_root_hash ==
       semantic_root_hash`;
   - pin the compiled recovery root only if that verification succeeds;
8. if the compiled ref is absent, invalid, or encoded in an unsupported
   compiled format/version, continue with a semantic-only recovery view;
9. pin the snapshot root and semantic recovery root;
10. publish the updated `pins/<pid>.pin` payload while still holding shared
   `gc.lock`;
11. increment the local refcount for each pinned root;
12. release shared `gc.lock`;
13. return the read view containing the pinned root hash or hashes.

This rule is intentional:

- if raw snapshot or semantic recovery heads exist without a matching
  `refs/eval/<eval-ref>.ref`, `openEvalView()` still returns the canonical empty
  evaluator view for that key pair;
- separate raw-head installs are therefore maintenance/import helpers, not an
  alternate evaluator-visible state path.

Reading the root ref first and pinning later without the shared `gc.lock` is
not allowed.
That gap would let GC reclaim the just-opened root.

Raw snapshot or semantic-recovery opens may exist for maintenance and export,
but they must apply the same root-object meta validation rules and they are not
the supported coherence token for evaluator reads.
If their ref is absent, they interpret that namespace as the canonical empty
root for the requested key rather than as a hard "not found" error.
As above, they may materialize the corresponding empty immutable blocks via
idempotent `put()` before pinning, but they must not publish a new namespace
ref just because a reader opened an absent namespace.

#### GC Protocol

GC uses a global lock file:

```text
<engine-root>/gc.lock
```

The collector:

1. acquires `gc.lock`;
2. reads every published root ref;
3. enumerates every `pins/*.lock`;
4. if a pin lock can be exclusively locked, its owner is gone and the matching
   `pins/<pid>.pin` payload is stale; remove both files;
5. otherwise read `pins/<pid>.pin` and include those roots in the mark set;
6. mark from published refs plus live pin files;
7. sweep unreachable blocks;
8. release `gc.lock`.

The collector never relies on "age" or ad hoc row cleanup.
It is purely root-based reachability, plus live reader pins.[^in-tree-gc]

#### Startup Hygiene And Corruption Handling

The earlier note described steady-state GC and open semantics, but not what a
fresh process should do before normal traffic begins.

Three choices were considered:

1. eager full-engine scrub at every startup:
   - walk every block, every ref, every pin file, and every staged
     transaction before serving any reads or writes;
2. bounded startup hygiene plus fail-closed validation:
   - clean up only liveness-shaped debris cheaply at startup and rely on
     explicit validation at open / publish / GC mark time for semantic roots;
3. lazy-only:
   - do no startup cleanup and let ordinary opens / GC eventually notice dead
     staging trees or stale pin files.

V1 chooses the second rule.

That choice is deliberate:

- a full startup fsck is too expensive to make part of the required hot path
  for every evaluator process;
- doing nothing leaves stale staging trees and dead pin files around longer
  than necessary even though their liveness can be checked cheaply by lock
  acquisition;
- semantic safety already depends on validating refs and root objects at the
  point of use, so startup does not need to duplicate that whole walk.

Consequences:

- v1 startup may opportunistically reclaim:
  - stale `pins/<pid>.lock` + `pins/<pid>.pin` pairs; and
  - stale `staging/<writer-id>/` trees or visible `<txid>/` directories using
    the existing `writer.lock` / `tx.lock` liveness protocol;
- v1 startup does not require a full block-by-block scan or full closure walk
  before serving reads or writes;
- semantic refs are still validated at open, CAS, import, and GC mark time;
- there is no automatic "repair by dropping suspicious roots" rule in v1.

Published-ref corruption also needs one explicit fail-closed rule.

If GC or any other maintenance walk encounters a published ref whose
`root-ref-v1` payload is malformed, namespace-mismatched, format-invalid, or
points at a root object that fails required meta validation, that is hard
corruption.

V1 chooses:

- evaluator opens fail that namespace / view as corruption, as already required
  above; and
- GC must fail closed rather than silently ignoring or deleting the corrupt
  published ref.

That choice is deliberate:

- silently dropping a corrupt published ref could incorrectly unroot still-live
  semantic state;
- silently ignoring it could let GC reclaim blocks that the operator still
  expects the published namespace to protect;
- ref repair is therefore an explicit maintenance action, not an automatic side
  effect of ordinary startup or GC.

If an operator wants repair later, that should be a separate maintenance tool
that:

1. validates refs and root-object meta out of band;
2. reports or quarantines corrupt published refs explicitly; and
3. only then allows a subsequent GC pass to proceed against the repaired ref
   set.

#### Maintenance Tool Scope

The earlier note now says "use a separate maintenance tool", but that still
leaves too much room for accidental semantic repair-by-default.

Three maintenance shapes were considered:

1. one mutating `repairEngine` command by default:
   - scan, decide, and rewrite published refs or blocks in one pass;
2. split verify vs quarantine/repair:
   - default to a read-only verification walk, and require a second explicit
     maintenance mode before any published ref is rewritten or quarantined;
3. no maintenance tool at all:
   - rely only on ordinary open/publish/GC failures and manual filesystem
     surgery.

V1 chooses the second rule.

That choice is deliberate:

- the engine's fail-closed behavior should surface corruption without also
  performing irreversible state changes;
- a read-only verification pass is safe to run diagnostically, while quarantine
  or repair changes the published root set and therefore deserves an explicit
  operator action;
- manual filesystem surgery is too error-prone given that refs, pins, and
  logical roots are coupled by protocol rather than by one SQL catalog.

Consequences:

- the maintenance surface in v1 is conceptually split into:
  - `verifyEngine`: read-only structural validation of published refs, pin
    payloads, and optionally reachable block closure;
  - `quarantineRef` / `repairPublishedRefs`: explicit mutating maintenance that
    rewrites or relocates published refs after operator intent is clear;
- `verifyEngine` must not silently rewrite, delete, or quarantine published
  refs as a side effect of reporting corruption;
- mutating maintenance must run under the same exclusivity boundary that GC
  uses for whole-engine maintenance, i.e. exclusive `gc.lock`, so no concurrent
  open/publish/GC can observe a partially quarantined ref set;
- quarantined refs are not published roots:
  they move out of `refs/` into a maintenance/quarantine area for inspection
  and are ignored by ordinary open and GC root discovery;
- v1 does not define a generic "rebuild missing semantic state from surviving
  blocks" command;
  repair in v1 is limited to explicit ref quarantine/removal plus ordinary
  re-import / recomputation above the engine.

This also keeps v1 aligned with the rest of the note's rewrite stance:

- semantic truth is immutable blocks plus published refs;
- corruption in published refs is an operator-visible fault;
- repair is an explicit maintenance transition, not a hidden branch in normal
  evaluator traffic.

### 4. Publication Units

Cross-namespace atomicity is intentionally not promised in general.
The engine guarantees atomicity per namespace ref, not across arbitrary groups
of refs.

One coupled publication unit is special in v1:

- the current snapshot root and semantic recovery root for one
  `(semantic_session_key, stable_recovery_key)` pair must become visible to the
  evaluator together, matching the live SQLite behavior where `Sessions` and
  `History` move together inside one transaction.[^live-schema]
  [^sqlite-atomic]

V1 preserves that property through one explicit coherence object:

- `eval-view-v1`

The evaluator reads only through `refs/eval/<eval-ref>.ref`, not by opening
snapshot and semantic recovery refs independently.
Atomic multi-ref publication alone would not be enough, because separate opens
could still mix old snapshot state with newer recovery state between calls even
if the writes themselves were coordinated.[^git-update-ref]

For a fresh local evaluation record, the supported publication protocol is:

1. write all newly needed immutable blocks;
2. from the pinned `EvalReadView` owned by the caller's current
   `EvalSessionHandle`, derive the target semantic recovery root, target
   snapshot root, and target `eval-view-v1` root for this record;
3. publish the target `eval-view-v1` root under the shared-`gc.lock`
   `compare_and_swap_and_pin_exact(...)` protocol, keyed by
   `(semantic_session_key, stable_recovery_key)`, with `exact_roots` equal to
   the roots the returned view/handle will depend on, so success pins that
   exact committed root set before shared `gc.lock` is released;
4. if that coherent-head CAS loses:
   - call `confirm_current_ref_and_pin_exact(...)` against the same target
     `eval-view-v1` root and the same `exact_roots`;
   - if the current coherent head still points at the same target under shared
     `gc.lock`, treat the publication as a successful no-op around that exact
     current root set;
   - otherwise reopen the latest `EvalReadView`, recompute the target roots
     against that newer pinned view, and retry or return a retryable contention
     error;
5. after coherent-head success, raw maintenance-head refresh is scoped by what
   the successful evaluator-facing write actually changed:
   - raw snapshot refs may be refreshed to the committed snapshot root as
     maintenance-only no-op/replace CAS operations after either
     `publishRecord(...)` or `publishRuntimeRoot(...)`;
   - raw semantic-recovery refs may be refreshed to the committed semantic
     recovery root only after a write that actually changed
     `semantic_root_hash`, i.e. the `publishRecord(...)` path;
   - snapshot-only runtime-root publication must not move a raw
     semantic-recovery head as a maintenance side effect.
6. evaluator-facing local publication must not publish or replace any
   `recovery-compiled` ref, even as cache warming.
   Three choices were considered:
   - allow `publishRecord(...)` to publish a new compiled recovery ref for the
     changed semantic root as a best-effort maintenance side effect;
   - allow both `publishRecord(...)` and `publishRuntimeRoot(...)` to publish
     or refresh compiled recovery refs opportunistically; or
   - keep compiled ref publication inside the explicit validation boundary of
     `installCompiledRecoveryForRoot(...)`,
     `installCompiledRecovery(...)`, and `ensureCompiledRecovery(...)` only.
   V1 chooses the third rule.
   Consequences:
   - the note now has one authoritative compiled-ref publication boundary;
   - evaluator-facing local publication cannot accidentally bypass compiled
     validation or weaken the "validated-on-publication" rule;
   - any compiled recovery attachment for a newly changed semantic root remains
     an explicit later preflight step rather than a hidden side effect of local
     current-state writes.
7. optionally publish updated compiled vocab roots or other compiled-only
   immutable acceleration blocks that are not named by `recovery-compiled`
   refs.

Returned `compiled_root_hash` after evaluator-facing local publication follows
one additional rule.
Three choices were considered:

1. preserve the caller's prior `compiled_root_hash` unconditionally across
   every successful local write;
2. clear `compiled_root_hash` after every successful local write and force all
   later acceleration attachment through explicit compiled-preflight helpers; or
3. preserve `compiled_root_hash` only when the returned
   `semantic_root_hash` is unchanged, and clear it when the returned semantic
   root differs from the caller's prior view, even if a later explicit
   compiled-preflight helper succeeds for that new semantic root.

V1 chooses the third rule.

Consequences:

- `publishRuntimeRoot(...)` keeps a valid compiled attachment when it updates
  only snapshot state and leaves `semantic_root_hash` unchanged;
- `publishRecord(...)` cannot return a stale compiled attachment bound to an
  older semantic recovery root after a semantic-history update;
- evaluator-facing local publication does not need to probe or rebuild compiled
  refs on the success path just to preserve correctness;
- compiled acceleration for a newly changed semantic root remains an explicit
  follow-up through `installCompiledRecoveryForRoot(...)`,
  `installCompiledRecovery(...)`, or `ensureCompiledRecovery(...)`, not an
  implicit side effect of local current state publication or
  recording-handle replacement.

For evaluator-facing writes, this protocol is an internal publication subroutine
under `publishRecord(...)` and `publishRuntimeRoot(...)`.
Callers do not drive publication from a bare `EvalReadView`;
they invoke one of those bound-session operations, which uses the pinned view
inside the caller's `EvalSessionHandle` and, on success, returns a replacement
`EvalSessionHandle` rebound around the exact committed `eval-view-v1` root that
won or matched the successful coherent-head CAS.
It must not satisfy success by reopening the namespace head and accepting a
later `eval-view-v1` published by another writer after that successful CAS.
For `publishRuntimeRoot(...)`, ordinary coherent-head contention is not an
observable API outcome:

- once a newly fetched locked runtime root has been accepted into the current
  cold-eval path, the operation must absorb same-namespace CAS contention
  internally until it either commits and returns the replacement handle or
  fails with a non-retryable storage/corruption error;
- it must not return retryable contention that would force callers to perform a
  second out-of-band registry update just to preserve same-session provenance
  visibility.

This is a deliberate strengthening relative to the live implementation:

- the current store serializes publication on one `sqlite_strand` and relies on
  SQLite transaction atomicity for the row updates it performs; it does not have
  an optimistic whole-publish retry loop above immutable roots and per-namespace
  refs;[^live-threading][^live-publish]
- v1 therefore treats the coherent `eval-view-v1` CAS as the only success
  token for concurrent local publication and makes raw-head refresh
  explicitly maintenance-only.

This ordering guarantees:

- a visible root never points at missing blocks;
- GC cannot miss a newly published root, because staged blocks are installed and
  refs are published while both operations are linearized against `gc.lock`;
- GC cannot sweep unpublished blocks out from under a writer, because ordinary
  GC does not scan `staging/`;
- evaluator-visible state is coherent: `openEvalView()` sees either the old
  `(snapshot_root, semantic_recovery_root)` pair or the new pair, never a mix;
- raw snapshot and semantic recovery refs are maintenance heads, not the
  evaluator coherence token;
- they may lead or lag the committed coherent `eval-view-v1` head after either
  local publication or coherent import;
- tools that require evaluator-visible "current state" must read
  `refs/eval/<eval-ref>.ref` or export from one pinned `EvalReadView`, not from
  raw heads alone;
- coherent imports through `installEvalPackage(...)` never move raw heads, and
  local publication may refresh raw heads only as best-effort maintenance after
  coherent-head success;
- a crash may therefore leave raw heads stale, but never a published
  `eval-view-v1` that mixes roots from different evaluation records;
- compiled recovery may lag the latest semantic recovery ref without affecting
  semantic correctness, because `openEvalView()` only consults compiled roots
  keyed to the pinned semantic root and ignores absent or invalid compiled
  state;
- compiled vocab roots may lag semantic roots without affecting correctness.

That is a deliberate simplification.
The current system uses one SQLite transaction to update multiple tables, but
the new engine is not trying to preserve that exact internal publication shape.

## Deterministic Attr Vocabulary

Persistent attr vocab is allowed.
The earlier "no persistent attr-vocab namespace at all" rule was too strict.

What is not allowed is discovery-order global ordinals as semantic identity.

The key design decision is:

- semantic objects are self-describing from canonical bytes and semantic hashes;
- persistent vocab is a compiled acceleration layer for compact runtime
  encodings, recursive path representation, and numeric comparison;
- semantic publication does not wait on vocab publication.

Two storage choices were considered for persistent vocab:

1. machine-local SQLite-backed vocab cache only;
2. deterministic persistent vocab namespaces on the same immutable block/ref
   substrate as other portable compiled namespaces.

V1 chooses the second rule.

That split from stat-hash / tree-hash is intentional:

- attr vocab is not semantic truth, but it is also not just a machine-local
  observation cache;
- its contents are deterministic functions of canonical attr-name/path bytes
  and therefore can be rebuilt, compared, shipped, or pinned across sessions
  without depending on host-local file fingerprints or transfer side effects;
- keeping vocab on the deterministic namespace side lets compiled
  accelerators, and any future exported helper artifact that wants compact
  ordinal metadata, refer to one reproducible vocab root without turning
  discovery-order IDs into semantic identity.

Consequences:

- stat-hash/tree-hash may remain local DB-backed without affecting semantic
  identity;
- attr vocab remains a deterministic compiled namespace rather than a mutable
  machine-local cache;
- vocab publication may still lag semantic publication, because semantic bytes
  do not depend on vocab ordinals.

### `attr-name-vocab-v1`

One ordered map:

- key: `attr-name-key-v1 = uvarint(len) || raw name bytes`
- value: `attr-name-record-v1`

`attr-name-record-v1` contains:

- `name_digest = BLAKE3-256("nix.eval-trace/attr-name-v1" || attr-name-key-v1)`
- optional compiled ordinal metadata for one pinned vocab root

### `attr-path-vocab-v1`

One ordered map:

- key: `path-key-v1`
- value: `attr-path-record-v1`

`attr-path-record-v1` contains:

- `path_digest = BLAKE3-256("nix.eval-trace/path-key-v1" || path-key-v1)`
- `parent_path_digest`, or null for the root path
- `leaf_name_digest`
- optional compiled `(parent_ordinal, leaf_name_ordinal)` metadata for one
  pinned vocab root

### Deterministic Indices

If a caller wants integer indices for compact local arrays, the mapping is:

- `attr-name-index = lexicographic rank of attr-name-key-v1 in the pinned name
  vocab root`
- `attr-path-index = lexicographic rank of path-key-v1 in the pinned path vocab
  root`

Those indices are deterministic for a pinned vocab root.
They are not stable global identities across future vocab growth.

That distinction matters:

- stable semantic references use digests or canonical bytes;
- dense indices are root-local accelerators only.

This directly addresses the user's clarified requirement:
the persistent mapping may exist, but it must be deterministic.

### Publication Model

Compiled vocab roots are advisory:

1. writers may publish semantic roots without updating vocab roots;
2. any process may rebuild or advance vocab roots by scanning published semantic
   roots;
3. readers may use vocab ordinals when available and fall back to semantic bytes
   or digests when they are not.

This keeps the global vocab out of the semantic correctness path and avoids
turning it into the main write-contended hotspot.

### `path-key-v1`

`path-key-v1` is the canonical rooted attr-path representation.

Encoding:

- zero or more attr-name segments;
- each segment is `uvarint(len) || raw bytes`;
- no mutable vocab IDs appear in the path key.

### Subtree Scans

The previous note's subtree upper bound,
`< encode(P) || 0xFF`,
was wrong.

This note removes that rule entirely.

Subtree scans are defined as:

1. seek to the first key `>= encode(P)`;
2. decode each encountered `path-key-v1`;
3. stop when the decoded path no longer has `P` as a segment prefix.

This is still:

- `O(log N)` to seek;
- plus `O(k)` for the `k` matching descendants.

It does not depend on a bogus byte-range shortcut.

## Core Semantic Objects

### `dir-set-v1`

Canonical replacement for the live `DirSets` table.

It stores:

- an ordered canonical list of directory descriptors;
- normalized bytes only;
- no local IDs.[^live-schema][^live-trace-serialize]

`dep-key-set-v1` refers to it by hash when a dep uses a directory-set summary.

### `runtime-root-map-v1`

Canonical replacement for `SessionRuntimeRoots`.

One ordered map:

- key: source-id bytes
- value: runtime-root record with `locked_url`, `nar_hash`, and `store_path`

This is attached to `snapshot-v1`, because runtime roots are part of current
session state.[^live-schema]

Runtime-root publication is snapshot-local:

- adding or updating one runtime-root record rewrites `runtime_roots_root`,
  publishes a new `snapshot-v1`, and then publishes the matching `eval-view-v1`;
- it does not rewrite semantic recovery state;
- this matches the live role of `SessionRuntimeRoots`: current-session metadata
  used to seed and extend the semantic registry, not historical recovery
  membership.[^live-schema]

The runtime behavior must preserve the live session-open and cold-eval paths:

- when a trace/eval session opens, it must load `runtime-root-map-v1`,
  verify each persisted runtime root against its `locked_url`, `nar_hash`, and
  `store_path`, and seed the in-memory semantic registry from the verified
  entries, matching the live `loadAndVerifyRuntimeRoots()` behavior; in v1,
  rebinding a session handle must rebuild a fresh session-local registry from
  the immutable graph-derived registry seed and then reconstruct only the
  forward runtime-root entries from the persisted runtime-root map, rather than
  mutating a previously live registry in place.[^live-runtime-roots]
- during cold eval, when `fetchTree` produces a new locked runtime root, the
  evaluator must make that root visible to the current session immediately and
  persist it for future sessions, matching the current
  `TraceSession::registerRuntimeRootMount(...)` plus
  `TraceBackend::recordRuntimeRoot(...)` pair (historically a
  `TraceRuntime`-level `addRegistryMountPoint(...)` plus
  `recordRuntimeRoot(...)` path); in v1, the supported evaluator-facing
  way to do that is one bound-session `publishRuntimeRoot(...)`
  operation that returns a replacement session handle whose in-memory
  semantic registry already includes the verified runtime root and the
  carried-forward dynamic runtime-root mount overlay plus the newly
  added mount point for that cold-eval fetch;
- unlike the live split mutation path, v1 does not expose a partially updated
  "visible in this session but not yet reflected in the returned handle"
  state: `publishRuntimeRoot(...)` must absorb ordinary coherent-head
  contention internally and return only once that returned replacement handle
  carries both the immediate session-local visibility and the persisted
  snapshot/eval-view update, or fail the operation as unrecoverable.[^live-runtime-roots]

### `unit-v1`

`unit-v1` is the canonical empty set value used for map-backed sets such as
`candidate-set-v1`.

Its payload is empty bytes and its hash is:

- `BLAKE3-256("nix.eval-trace/unit-v1")`

### `result-v1`

Canonical replacement for live `Results`.

It contains:

- result kind;
- canonical kind-specific payload;
- attr names by bytes or stable name digests, never `AttrNameId`;
- deterministic identity classes where the evaluator's warm-hit behavior
  depends on identity;
- explicit encoding rules for integers, floats, `-0`, and NaN.

It does not contain:

- local `AttrNameId` values;
- local `AttrPathId` values;
- raw runtime `ValueIdentityStamp` numbers.[^live-result-codec]

### `dep-key-set-v1`

Canonical replacement for live `DepKeySets`.

It contains ordered dep keys encoded from semantic fields only:

- dep kind;
- dep source bytes;
- dep key bytes;
- structured file-path bytes;
- structured data-path bytes;
- trace-context path bytes;
- dir-set hash references where needed.

It does not contain local `StringId`, `FilePathId`, `DataPathId`, or
`AttrPathId` values.[^live-trace-serialize]

### `trace_hash`

`trace_hash` keeps the live meaning:

- it is the canonical hash of one ordered dep observation vector;
- it is computed from canonical dep keys plus canonical dep values;
- it does not include result payload bytes.[^live-recovery]

This preserves the current direct-hash recovery contract.[^live-recovery]

### `trace-v1`

Canonical replacement for live `Traces`.

It contains:

- `trace_hash`
- `result_hash`
- `dep_key_set_hash`
- dep values in dep-key order
- semantic flags only

The full block hash of `trace-v1` is `trace_record_hash`.
`trace_record_hash` and `trace_hash` are intentionally different concepts:

- `trace_hash` is the recovery key;
- `trace_record_hash` is the immutable object identity.

### `binding-v1`

Snapshot value for one rooted attr path.

It contains:

- `candidate_id`
- `trace_hash`
- `result_hash`
- `dep_key_set_hash`

The extra summary fields are allowed because they are pure functions of the
trace object.

In v1, `binding-v1` is also the explicit semantic edge set for snapshot
closure:

- `candidate_id` references `trace_record_hash`;
- `result_hash` references `result-v1`;
- `dep_key_set_hash` references `dep-key-set-v1`.

Unlike recovery membership, those references already live in the map value, so
snapshot closure does not need a second object-root like
`candidate_objects_root`.

`binding-v1` is not an independent semantic summary.
Its fields are strict derivations of the referenced `trace-v1`:

1. `candidate_id == trace_record_hash`
2. `trace_hash == trace-v1.trace_hash`
3. `result_hash == trace-v1.result_hash`
4. `dep_key_set_hash == trace-v1.dep_key_set_hash`

Any mismatch is hard corruption.

### `candidate_id`

In v1, `candidate_id` is exactly `trace_record_hash`.

This closes the earlier ambiguity about which trace fields define candidate
identity:

- every semantically relevant field in `trace-v1`, including semantic flags, is
  already covered by `trace_record_hash`;
- recovery hints in `candidate-summary-v1` do not change candidate identity.

### `candidate-summary-v1`

Recovery candidate summary object.

It contains:

- `candidate_id`
- `trace_hash`
- `result_hash`
- `dep_key_set_hash`
- `encoded_result_payload-v1`
- optional `git_identity_hash`
- optional `git_repo_root_digest`
- `git_recoverable`
- small recoverability flags

`encoded_result_payload-v1` is the exact data needed to call the existing
result decoder:

- result kind
- encoding version
- payload bytes
- aux-context bytes

This is the fast-recovery summary object.
It is deliberately wide enough to preserve the current per-candidate hot path:

- bootstrap, direct-hash, and GitIdentity recovery can accept a candidate
  without an extra `result-v1` lookup, because the summary already carries the
  encoded result payload the live SQL path preloads today;[^live-fast-history]
- GitIdentity recovery does not need to load the full historical trace just to
  re-check repo-root consistency and `allDepsGitRecoverable()`, because those
  facts are denormalized into `git_repo_root_digest` and `git_recoverable`.[^live-git-summary]

`candidate-summary-v1` is not semantic truth.
It is a deterministic acceleration object derived from:

- `trace-v1`
- `result-v1`
- `dep-key-set-v1`

The rules are:

1. writers must construct `candidate-summary-v1` only from those canonical core
   objects;
2. import must either validate every summary against the referenced core
   objects or discard and rebuild summaries;
3. any summary mismatch is a hard corruption error, not a soft cache miss.

### `history-member-v1`

Recovery member value for one `(path_digest, candidate_id)` pair.

`history-member-v1` is a canonical unit payload with no fields.

The key already carries the entire semantic identity of one history member:

- `path_digest`
- `candidate_id`

The value exists only because `prolly-map-v1` is map-shaped.
This closes the remaining ambiguity about duplicate summary state:

- semantic history membership lives only in the base member map;
- `candidate-summary-v1` and `candidate-set-v1` are deterministic acceleration
  objects derived from the core trace/result/dep objects and rebuilt or
  validated as needed;
- there is no second summary pointer in the semantic source of truth.

### `candidate-set-v1`

`candidate-set-v1` is a packed ordered immutable sequence optimized for the
recovery hot path.

Each entry stores:

- `candidate_id`
- the canonical `candidate-summary-v1` payload inline

Entries are sorted by `candidate_id`.
Candidate iteration order is therefore the lexicographic order of
`candidate_id` bytes.

This is intentionally not encoded as a generic
`candidate_id -> candidate_summary_hash` map or as a second layer of summary
objects loaded one at a time.
That indirection would force an extra block lookup per candidate and would miss
the performance target set by the current SQL hot path.[^live-fast-history]

`candidate-set-v1` is also derived acceleration state.
It is not semantic truth.
Import may keep persisted candidate sets only if it validates them, or it may
discard and rebuild them from:

- the semantic base member map;
- `trace-v1`
- `result-v1`
- `dep-key-set-v1`

The required properties are:

1. entry membership equals the candidate IDs selected by the corresponding
   secondary index key;
2. iteration order is canonical `candidate_id` order;
3. every inlined `candidate-summary-v1` payload matches the canonical rebuilt
   summary for that `candidate_id`.

## Ordered Map Primitive

The earlier note was too vague.
This note chooses one exact primitive:
`prolly-map-v1`.

The construction is informed by uniquely represented trees and deterministic
Merkle search trees, but the rules below are normative, not illustrative.[^btreap]
[^atproto-repo]

### Key Semantics

- keys are raw byte strings;
- key ordering is lexicographic by raw bytes;
- duplicate keys are invalid input;
- map mutation is defined over a logical set of unique key/value pairs.

### Values

Map values are always referenced by hash:

- each map entry stores `value_hash`;
- the real value bytes live in a separate immutable block.

This keeps node encodings uniform and avoids a second inline-value format.

### Chunking Constants

`prolly-map-v1` uses fixed constants:

- `TARGET = 64`
- `MIN = 32`
- `MAX = 128`

These are format constants, not implementation tunables.

### Leaf Entry Boundary Hash

For each sorted leaf entry:

- `boundary_hash = BLAKE3-256("nix.eval-trace/prolly-boundary-v1" || key || value_hash)`

Interpret the first 8 bytes of `boundary_hash` as big-endian `u64`.

### Leaf Chunking Rule

Scan sorted logical entries in order and build chunks as follows:

1. a chunk always starts at the first remaining entry;
2. extend the chunk until one of the following becomes true:
   - the chunk already has at least `MIN` entries and the current entry's
     boundary value is `< floor(2^64 / TARGET)`;
   - the chunk has reached `MAX` entries;
   - input is exhausted.

This rule is deterministic from sorted current contents alone.

### Leaf Node Encoding

Canonical payload:

```text
u8    node_kind = 0x01
uvarint entry_count
repeat entry_count times:
  uvarint key_len
  bytes   key
  bytes32 value_hash
```

Leaf hash:

- `leaf_hash = BLAKE3-256("nix.eval-trace/prolly-leaf-v1" || canonical_leaf_payload)`

### Internal Chunking Rule

Each child contributes:

- `first_key`
- `child_hash`

Its boundary hash is:

- `BLAKE3-256("nix.eval-trace/prolly-internal-boundary-v1" || first_key || child_hash)`

Chunking uses the same `TARGET`, `MIN`, and `MAX` rule.

### Internal Node Encoding

Canonical payload:

```text
u8    node_kind = 0x02
uvarint child_count
repeat child_count times:
  uvarint first_key_len
  bytes   first_key
  bytes32 child_hash
```

Internal hash:

- `internal_hash = BLAKE3-256("nix.eval-trace/prolly-internal-v1" || canonical_internal_payload)`

### Empty Map

The empty map is the hash of the canonical empty leaf:

```text
node_kind = 0x01
entry_count = 0
```

There is no special "null root" encoding for empty maps.

### Resulting Property

Because chunk boundaries depend only on sorted current contents,
the root hash is a pure function of the logical key/value set, not of mutation
history.[^atproto-repo][^btreap]

## Root Ref Object

### `root-ref-v1`

Each ref file stores one canonical `root-ref-v1` payload:

- namespace kind
- serialized namespace key
- root hash
- format version

For compiled recovery refs, `serialized namespace key` is the canonical tuple:

- `stable_recovery_key`
- `base_semantic_root_hash`

The lock file is not part of the semantic payload.
It is only the coordination mechanism for CAS publication.

Readers must not trust the ref pathname alone.
When opening any ref, the implementation must verify that:

- `namespace kind` matches the expected kind;
- `serialized namespace key` matches the expected namespace key;
- `format version` is supported.

Any mismatch is hard corruption.

## Live Eval Coherence Namespace

### `eval-view-v1`

`eval-view-v1` is the live coherence object for one
`(semantic_session_key, stable_recovery_key)` pair.

Each instance consists of:

1. `eval-view-v1`
   - `snapshot_root_hash`
   - `semantic_recovery_root_hash`
   - `eval_view_meta_hash`
2. `eval-view-meta-v1`
   - serialized `semantic_session_key`
   - serialized `stable_recovery_key`
   - format version

This object is not an export artifact.
It is the live evaluator read token that preserves the current "current state
and history move together" property even though snapshot and semantic recovery
remain independently addressable namespace roots.[^live-schema]

Live eval-view closure is walked from explicit object edges:

1. `eval-view-v1` references `snapshot_root_hash`, `semantic_recovery_root_hash`,
   and `eval_view_meta_hash`;
2. the snapshot and semantic recovery roots then contribute their own closure as
   defined below.

## Snapshot Namespace

### What A Snapshot Is

This ambiguity is now closed.

One `snapshot-v1` root is the entire current binding map for one
`semantic_session_key`.
It is not a partial export and not a subtree slice.

If partial export is ever wanted later, that should be a different object type,
not a redefinition of `snapshot-v1`.

### Shape

Each snapshot namespace instance consists of:

1. `snapshot-v1`
   - `bindings_root`
   - `runtime_roots_root`
   - `snapshot_meta_hash`
2. `bindings_root`
   - map key: `path-key-v1`
   - map value: `binding-v1`
3. `runtime_roots_root`
   - map key: source-id bytes
   - map value: runtime-root record
4. `snapshot-meta-v1`
   - serialized `semantic_session_key`
   - feature flags
   - format version

This replaces the current `Sessions` and `SessionRuntimeRoots` tables.[^live-schema]

For portable export, one `snapshot-pack-v1` object for one
`semantic_session_key` is the semantic snapshot artifact.

Its canonical payload is:

- serialized `semantic_session_key`
- `snapshot_root_hash`
- format version

`snapshot-pack-v1` is a transport descriptor, not a live retained namespace
root.
Install flows may validate it directly from supplied bytes and then discard it.

Snapshot closure is walked from explicit object edges:

1. `snapshot-v1` references `bindings_root`, `runtime_roots_root`, and
   `snapshot_meta_hash`;
2. each `binding-v1` references `candidate_id`, `result_hash`, and
   `dep_key_set_hash`;
3. in v1, `candidate_id` references `trace_record_hash`;
4. `trace-v1` references `result_hash` and `dep_key_set_hash`.

GC and export closure follow those explicit fields.
They do not infer reachability from path keys or runtime-local caches.

### Update Complexity

Updating one path rewrites:

- `O(log B)` snapshot nodes, where `B` is the number of current bindings for
  that semantic session;
- plus the newly added immutable trace/result/dep blocks if any.

There is no whole-snapshot rewrite.

### `NodeStamp` Replacement

The current store needs `NodeStamp` because it mutates current bindings in
place.[^live-node-stamp]

The new engine does not.

The replacement rule is:

1. a verification session opens one `EvalReadView` and reads current bindings
   from its pinned snapshot root;
2. all current-binding lookups in that verification session read from that
   pinned root;
3. trace-context memoization keys on `(snapshot_root_hash, path-key-v1)`, not
   `(AttrPathId, NodeStamp)`;
4. if the evaluator wants to see later writes, it opens a new `EvalReadView`
   and gets a different snapshot root hash.

Inside a pinned snapshot root, no current binding can change.
That makes the root hash the generation token.

## Recovery Namespace

### What A Recovery Pack Is

This ambiguity is also closed.

One `recovery-pack-v1` export object for one `stable_recovery_key` is the
semantic recovery artifact.

Its canonical payload is:

- serialized `stable_recovery_key`
- `semantic_recovery_root_hash`
- format version

If prebuilt compiled acceleration is shipped, it is emitted as a separate
optional `compiled-recovery-package-v1` companion object whose canonical payload
is:

- serialized `stable_recovery_key`
- `base_semantic_root_hash`
- `compiled_recovery_root_hash`
- format version

That companion object is valid only if:

- `base_semantic_root_hash = semantic_recovery_root_hash` of the paired
  `recovery-pack-v1`; and
- the referenced `compiled-recovery-v1` object declares the same
  `base_semantic_root_hash`.

This follows the same general pattern used by descriptor-based Merkle object
systems: a higher-level object can name multiple independently content-addressed
roots without collapsing them into one mutable pointer.[^oci-descriptor]
[^oci-image-index]

These export objects are not the same thing as the live published refs:

- live coherent evaluator publication is keyed by
  `(semantic_session_key, stable_recovery_key)` through `refs/eval/<eval-ref>.ref`;
- live snapshot publication is keyed only by `semantic_session_key`;
- live semantic recovery publication is keyed only by `stable_recovery_key`;
- live compiled recovery publication is keyed by
  `(stable_recovery_key, base_semantic_root_hash)`.

The semantic recovery root defines portable recovery identity.
It includes:

1. the set of historical members;
2. explicit semantic references to the core trace objects needed to execute
   recovery;
3. through those trace objects, the full closure of referenced result and dep
   objects needed to execute recovery.

The compiled recovery root is deterministic acceleration state derived from the
semantic recovery root.
It may be shipped alongside the semantic recovery artifact for fast import,
omitted entirely, or discarded and rebuilt locally on import.

This closes the earlier ambiguity:

- semantic recovery identity does not depend on compiled indexes;
- fast recovery may still ship prebuilt compiled indexes when desired.

`recovery-pack-v1` and `compiled-recovery-package-v1` are transport descriptors,
not live retained namespace roots.
Install flows may validate them directly from supplied bytes and then discard
them once the referenced semantic or compiled roots have been published.

For coherent portable evaluator state, one `eval-package-v1` object for one
`(semantic_session_key, stable_recovery_key)` pair is the portable coherence
manifest.

Its canonical payload is:

- serialized `semantic_session_key`
- serialized `stable_recovery_key`
- `snapshot_root_hash`
- `semantic_recovery_root_hash`
- format version

`eval-package-v1` is valid only if:

- the supplied `snapshot-pack-v1` payload embeds the same
  `semantic_session_key` and `snapshot_root_hash`;
- the supplied `recovery-pack-v1` payload embeds the same
  `stable_recovery_key` and `semantic_recovery_root_hash`.

This is the portable transport analogue of `eval-view-v1`:

- `eval-view-v1` is the live local coherence token and the semantic identity of
  one coherent evaluator state;
- `eval-package-v1` is the exported transport manifest used to ship that same
  coherent `(snapshot_root_hash, semantic_recovery_root_hash)` pair together.

That distinction is intentional.
`eval-package-v1` is not a second semantic identity layer above
`eval-view-v1`.
Changing transport wrappers around `snapshot-pack-v1` or `recovery-pack-v1`
does not change semantic coherent evaluator state, because `eval-package-v1`
names semantic roots directly rather than naming package-descriptor hashes.

Exporting `snapshot-pack-v1` or `recovery-pack-v1` alone remains valid for
narrower use cases, but that is not a coherent "current evaluator state" export.
Any export/import flow that wants the live "snapshot and history move together"
property must anchor on `eval-package-v1`.

`eval-package-v1` is also a transport descriptor, not a live retained root.
`installEvalPackage(...)` must not depend on that descriptor remaining reachable
from the engine's GC roots after installation completes.

### Integration With Nix Substitution

The package formats above do not by themselves choose a remote transport.

Three remote-integration choices were considered:

1. bespoke eval-trace transport:
   - define a new HTTP/SSH/S3-facing protocol for `eval-package-v1` and
     `block-archive-v1`, parallel to binary caches and substituters;
2. existing substituter path for portable artifacts:
   - treat package payloads and block archives as content formats that are
     carried inside ordinary content-addressed store objects and closures, then
     fetched through existing Nix substituters / binary caches;
3. direct remote eval-query short-circuit:
   - define a new query protocol keyed by deterministic eval-query bytes so a
     remote cache can answer "exact hit / accepted recovery candidate" without
     first naming or fetching a full artifact closure.

V1 chooses the second rule and explicitly does not require the third.

Consequences:

- `eval-package-v1`, `snapshot-pack-v1`, `recovery-pack-v1`,
  `compiled-recovery-package-v1`, and `block-archive-v1` are content formats,
  not a second transport stack;
- remote distribution should reuse existing Nix substituter/binary-cache
  machinery, signatures, priorities, auth, and transport implementations by
  wrapping these payloads in ordinary content-addressed store objects;
- substituted store paths are transport locators and authentication carriers,
  not semantic identity: semantic acceptance still comes from validating the
  inner package/archive bytes against their declared roots and closure rules;
- `install*Package(...)` consumes validated bytes after fetch; those bytes may
  come from local disk, `nix copy`, `fetchClosure`, `ensurePath`, or any other
  existing store/substituter path;
- v1 therefore preserves remote artifact substitution without inventing a new
  cache protocol alongside narinfo / store-object substitution;
- direct remote "query short-circuit" remains a future extension, not part of
  v1 semantic correctness.

For the artifact-substitution path itself, three wrapping choices were
considered:

1. one Nix store object per immutable eval block;
2. one Nix store closure per exported artifact set, with one root store object
   for the manifest payload and sibling store objects for the package/archive
   payloads it needs;
3. no store wrapping at all, i.e. require a bespoke downloader even when Nix
   substituters already exist.

V1 chooses the second wrapping.

Consequences:

- the existing binary-cache/substituter path moves a small number of coarse
  export payloads, not millions of tiny engine blocks;
- the semantic engine keeps `block-archive-v1` as its portable closure format,
  while Nix transport remains ordinary store-object substitution;
- a coherent export can therefore be fetched by naming one root store path in a
  closure and then handed to `installEvalPackage(...)` after reading the
  payload bytes from that closure locally.

This split suggests one thin integration layer rather than a second storage
engine:

- `exportEvalPackageToStoreClosure(view) -> StorePath` should materialize one
  content-addressed store closure whose root path identifies the coherent
  export and whose closure carries the package/archive payload objects needed by
  `installEvalPackage(...)`;
- `installEvalPackageFromStoreClosure(root_store_path) -> EvalReadView` should
  fetch or ensure that closure into the host's local store through the ordinary
  store/substituter path, read the payload bytes from that local store, then
  delegate to the existing byte-oriented `installEvalPackage(...)`.

`root_store_path` is therefore a transport handle for one coherent export
closure, not the semantic query key for evaluator state.

The outer store-wrapper layout also needs to fit current Nix store APIs.
Three wrapper layouts were considered:

1. one monolithic store object whose bytes inline every package/archive payload;
2. one content-addressed root manifest object that references sibling
   content-addressed payload objects;
3. one input-addressed wrapper closure whose meaning depends on narinfo
   signatures and cache trust.

V1 chooses the second layout.

Consequences:

- `root_store_path` can be treated as an ordinary `StorePath` whose
  `ValidPathInfo.references` are the sibling payload objects of the coherent
  export closure;
- `installEvalPackageFromStoreClosure(root_store_path)` can therefore be
  implemented with existing store primitives:
  - fetch or ensure the root wrapper object through the ordinary
    store/substituter path;
  - `queryPathInfo(root_store_path)` to discover the wrapper closure;
  - ensure the sibling payload store objects named by that wrapper manifest and
    reference set before attempting to read them locally;
  - read one small wrapper manifest from the root object;
  - verify that the manifest-declared sibling payload paths exactly match the
    root object's reference set;
  - read the referenced payload bytes locally and delegate to the existing
    byte-oriented validators;
- wrapper authenticity stays inside existing CA-store / substituter semantics,
  while semantic acceptance still comes from validating the referenced inner
  package/archive bytes.

This helper is intentionally a local-store integration layer.
Remote stores and substituters remain sources for the wrapper closure, but the
engine-side install step runs after those store objects have been ensured into
the host's local store.
V1 does not require a fully generic `Store`-level implementation that can
decode package/archive bytes from arbitrary remote store backends without first
materializing them locally.

Concretely, the wrapper root should be a small content-addressed NAR directory
object containing one deterministic manifest file that names:

- wrapper format version;
- the coherent semantic roots named by the export;
- the sibling store paths that hold the exact bytes of:
  - `eval-package-v1`
  - `snapshot-pack-v1`
  - `recovery-pack-v1`
  - semantic `block-archive-v1`
  - optional compiled companion payloads.

The sibling payload objects may use any ordinary content-addressed store-object
encoding that preserves exact bytes, but the wrapper manifest and reference set
must identify them unambiguously.

For implementation feasibility against current store APIs, v1 narrows that
further:

- each sibling payload object should be one content-addressed regular-file
  store object whose file contents are exactly the package/archive bytes named
  by the manifest;
- each sibling payload object should have an empty reference set, so the
  wrapper closure is shallow and the install helper only needs the root object
  plus its direct sibling payload objects;
- descriptor payloads may use any exact-byte file encoding appropriate for
  canonical text payloads;
- archive payloads should use one exact-byte binary file encoding.

This avoids requiring `installEvalPackageFromStoreClosure(...)` to reconstruct
package/archive bytes from arbitrary directory layouts or nested NAR trees.

Analogous standalone helpers may exist for snapshot-only, recovery-only, or
compiled-only transport, but they should stay wrappers around the same
byte-level package/archive validators rather than defining a parallel semantic
import path.

### If Remote Eval Query Short-Circuit Is Added Later

The existing artifact-substitution story is enough for v1, but it is not the
same as "ask a remote cache for this eval query and short-circuit locally".

If a later version wants that stronger behavior, three query-scope choices
exist:

1. exact-hit only:
   - remote queries cover only deterministic exact-hit lookup, e.g.
     `(path_digest, trace_hash)`;
2. full remote recovery:
   - remote queries also cover bootstrap, GitIdentity, direct-hash, and
     structural recovery classes against one named semantic recovery root;
3. opaque server-defined heuristics:
   - the client asks one vague "do you have anything for this path?" question
     and trusts the server's choice of answer.

V1 does not choose among them because remote query short-circuit is not part of
the correctness boundary.
But any future design should reject the third option.
The client must be able to name exactly what recovery universe and what query
class it is asking about.

At minimum, a future query protocol would need:

- one anchor identifying the remote semantic universe being queried, e.g.
  `semantic_recovery_root_hash` for recovery-only queries, or the coherent
  `(snapshot_root_hash, semantic_recovery_root_hash)` pair / corresponding
  `eval-view-v1` root for evaluator-state queries;
- if a substituted store closure is used to distribute the relevant artifacts,
  it may reveal or prove that semantic anchor after fetch, but the closure path
  itself is transport state, not the query key;
- one explicit query class:
  - candidate-addressed
  - exact/direct-hit
  - bootstrap
  - GitIdentity
  - structural-variant;
- one deterministic query payload for that class, such as:
  - `(path_digest, candidate_id)` for candidate-addressed lookup;
  - `(path_digest, trace_hash)` for direct-hit;
  - `(path_digest)` for bootstrap;
  - `(path_digest, git_identity_hash)` for GitIdentity;
  - `(path_digest, dep_key_set_hash, structural_override_vector)` or an
    equivalent deterministic structural-recovery key for structural lookup.

The response cannot just be "here is a result payload".
It would need to carry enough structure for the client to validate local reuse,
for example:

- the queried semantic anchor (`semantic_recovery_root_hash` or equivalent);
- accepted `candidate_id` / `trace_record_hash`;
- `result_hash`;
- `dep_key_set_hash`;
- `encoded_result_payload-v1`; and
- either:
  - a proof bundle from the semantic recovery root to the accepted candidate;
    or
  - references to already substitutable artifact closures from which the client
    can reconstruct and validate that proof locally.

Negative-answer semantics also need to be explicit.

Three future choices exist:

1. authoritative miss:
   - the server may answer "no match exists for this query under this anchor"
     and the client may stop without local fallback;
2. positive-only remote hint:
   - the server may answer with a validated hit/proof or "unknown", but absence
     is not authoritative and the client must still fall back to local lookup /
     recovery;
3. authority-certificate miss:
   - the server may return an authoritative miss only together with an explicit
     completeness proof or capability binding that says it is authoritative for
     the queried semantic anchor and query class.

V1 does not choose a remote query protocol at all, but any future design should
start from the second rule and only claim authoritative negative answers if it
also defines the third rule rigorously.

That choice is deliberate:

- ordinary binary caches and substituters are not globally complete, so simple
  absence is not a proof of semantic absence;
- a false negative is much more dangerous than a missed positive hit because it
  can incorrectly suppress local exact-hit or constructive recovery;
- positive-only remote short-circuit still captures the main performance win:
  skip local work on proven remote hits while leaving local recovery as the
  safety net for partial or lagging remote caches.

Future transport shape also needs one explicit choice.

Three options exist:

1. overload existing substituter / realisation queries:
   - try to encode eval-result existence into `querySubstitutablePathInfos`,
     `.narinfo`, or `queryRealisation`-style responses;
2. define one dedicated eval-query operation:
   - a separate request/response type keyed by semantic anchor plus
     deterministic eval-query bytes;
3. make store-closure substitution the only remote protocol forever:
   - require full artifact fetch before any remote exact-hit or recovery
     decision is visible.

V1 still chooses the third rule for required behavior, but if a future
short-circuit protocol is added, it should choose the second rule rather than
the first.[^live-substituter-query]

That choice is deliberate:

- current substituter queries are keyed by `StorePath` or `DrvOutput`, while
  future eval-query keys are semantic-engine objects such as
  `(semantic_recovery_root_hash, query_class, query_payload)`;
- existing substituter responses describe transport availability, references,
  and download sizes, not proof-carrying semantic acceptance of one recovery
  candidate;
- bolting eval-query meaning onto existing substituter negative responses would
  make cache-authority semantics even harder to reason about.

If a future eval-query protocol is added, one concrete envelope is the right
starting point:

- request:
  - protocol version
  - semantic anchor kind:
    - `semantic_recovery_root_hash`
    - or coherent `eval-view-v1` / `(snapshot_root_hash, semantic_root_hash)`
  - query class
  - canonical query payload bytes
  - requested answer mode:
    - proof bundle
    - substitutable artifact closure handle
    - either
- response:
  - echoed protocol version
  - echoed semantic anchor
  - echoed query class
  - query payload digest
  - one outcome tag:
    - `hit-proof`
    - `hit-closure`
    - `unknown`
    - `unsupported-anchor`
    - `unsupported-query-class`

Positive responses must be self-describing:

- `hit-proof` carries:
  - accepted `candidate_id`
  - `trace_record_hash`
  - `result_hash`
  - `dep_key_set_hash`
  - `encoded_result_payload-v1`
  - proof bytes sufficient to validate inclusion under the echoed semantic
    anchor;
- `hit-closure` carries:
  - one substitutable store-closure handle for an artifact whose validated
    bytes are sufficient to reconstruct the same proof locally.

`unknown` means only "this responder is not asserting a hit".
It is not an authoritative miss unless the future protocol separately defines
the stronger authority-certificate mode above.

Proof shape also needs a stricter semantic boundary.

The current design makes `candidate-set-v1` and the secondary recovery indexes
derived acceleration, not semantic truth.
That means a remote proof cannot safely say "this is the canonical recovery
answer" merely by pointing at one persisted candidate set, because that set is
not itself committed by `semantic-recovery-v1`.

Three future proof strategies exist:

1. proof against derived candidate sets:
   - return one `candidate-set-v1` witness and trust it as the answer set for
     the query key;
2. proof against semantic objects only:
   - return witnesses rooted only in `semantic-recovery-v1`,
     `members_root`, `candidate_objects_root`, `trace-v1`, `result-v1`, and
     `dep-key-set-v1`, plus query-class-specific checks the client can rerun;
3. closure-assisted canonical answer:
   - return a substitutable artifact closure handle and let the client
     reconstruct or validate the full canonical candidate universe locally.

Any future design should reject the first strategy.

That choice is deliberate:

- `candidate-set-v1` is explicitly rebuildable acceleration and may be omitted
  or discarded on import;
- a proof rooted only in derived candidate sets would not be stable under the
  semantic identity boundary this note chose;
- the client must be able to validate the answer from semantic commitments, not
  from server-chosen cache material.

Consequences:

- `hit-proof` is naturally suited to proving "candidate `C` is semantically
  valid under anchor `A`" by carrying:
  - the root object bytes needed to hash and validate the echoed semantic
    anchor;
  - a `members_root` inclusion witness for the relevant `(path_digest,
    candidate_id)` membership;
  - a `candidate_objects_root` inclusion witness for the same `candidate_id`;
  - the referenced `trace-v1`, `result-v1`, and `dep-key-set-v1` objects;
  - any query-class-specific witness needed to rerun the predicate locally,
    such as `trace_hash`, `git_identity_hash`, or structural override data
    derived from those semantic objects;
- `hit-proof` by itself is not enough to claim "this is the canonical winner"
  for bootstrap / GitIdentity / direct-hash / structural-variant recovery
  unless a future protocol also defines how the client proves completeness over
  all lower-ordered candidate IDs for that query;
- therefore, if a future short-circuit protocol wants to preserve canonical
  winner semantics without expanding the semantic anchor, the safer starting
  point is:
  - proof-only responses for candidate-addressed or otherwise uniquely
    determined queries; and
  - `hit-closure` for general recovery classes, so the client can rebuild or
    validate the ordered candidate universe locally before short-circuiting.

Future canonical-winner claims need one more explicit choice.

Three strategies exist:

1. proof-only canonical winner from the current semantic anchor:
   - let the server answer "this is the winner" from one candidate-validity
     proof plus server assertion about lower candidates;
2. closure-first canonical winner:
   - allow proof-only short-circuit only for candidate-addressed or otherwise
     uniquely determined queries, and require `hit-closure` for bootstrap,
     GitIdentity, direct-hash, and structural-variant winner claims until the
     client can reconstruct the ordered candidate universe locally;
3. future proof-only canonical winner with an explicit completeness witness:
   - allow proof-only winner claims for the general recovery classes only after
     the protocol defines how the client verifies that no lower-ordered
     candidate under the same semantic anchor satisfies the same query
     predicate.

Any future design should reject the first strategy.
The second strategy is the required starting point.

That choice is deliberate:

- a cache-authority certificate or capability binding may justify why one server
  is allowed to answer for one semantic anchor, but it does not by itself prove
  canonical winner order within that anchor;
- the current semantic anchor commits to historical members and candidate
  objects, not to one pre-sorted query-specific winner table;
- requiring `hit-closure` for the general recovery classes keeps the first
  remote short-circuit design aligned with the current semantic commitments
  instead of quietly trusting server-side ordering.

Consequences:

- before a later protocol defines a real completeness witness, proof-only
  remote short-circuit may claim only:
  - candidate validity; or
  - winner semantics for candidate-addressed or otherwise uniquely determined
    queries;
- bootstrap, GitIdentity, direct-hash, and structural-variant winner
  short-circuit must use `hit-closure`, not `hit-proof`, if the client is going
  to skip local recovery work;
- a future authority-certificate miss is a statement about negative-answer
  authority, not a substitute for canonical-winner completeness.

If a future protocol does want proof-only canonical winner claims for the
general recovery classes without expanding the semantic anchor, it needs one
more explicit completeness-witness choice.

Three shapes exist:

1. server assertion of ordered completeness:
   - the server says "no lower candidate matches" without transporting enough
     semantic evidence for the client to verify that claim;
2. path-local complete-prefix witness over the current semantic anchor:
   - the proof transports enough `members_root` witness material to show the
     complete ordered set of candidate memberships for one `path_digest` up to
     and including the claimed winner candidate;
   - for every lower candidate in that witnessed prefix, the proof also carries
     enough semantic object material for the client to rerun the query-class
     predicate and confirm that the candidate does not satisfy it;
   - for the accepted candidate, the proof carries the ordinary candidate-
     validity witness showing that it does satisfy the predicate;
3. semantic-anchor expansion:
   - change the semantic design itself so canonical winner summaries become
     committed semantic objects rather than derived acceleration.

Any future design should reject the first shape.
If it wants proof-only canonical winner claims without changing the semantic
anchor, it should choose the second.

That choice is deliberate:

- the current semantic anchor already commits to ordered membership by
  `(path_digest, candidate_id)` in `members_root`, so a sound completeness
  witness has to be derived from that committed order rather than from
  `candidate-set-v1` or another derived index;
- the third shape is a real semantic-model change, not a transport-only future
  extension;
- the second shape is expensive but honest: it proves canonical winner order
  from the current semantic commitments instead of quietly trusting remote cache
  policy.

Consequences:

- a future proof-only canonical-winner witness must extend the single-path
  inclusion witness above into a path-scoped range witness over `members_root`,
  not just another single-candidate proof;
- that range witness must be sufficient for the client to verify there are no
  omitted `(path_digest, candidate_id)` entries with `candidate_id` lower than
  the accepted candidate inside the witnessed range;
- for each candidate enumerated in that witnessed range, the proof must either:
  - show the query predicate fails for that candidate from semantic objects; or
  - for the accepted candidate, show the predicate succeeds;
- witness size is therefore proportional to the number of lower-ranked
  candidates that must be ruled out for that path/query pair, which is exactly
  why the closure-first rule above is the required starting point rather than an
  arbitrary conservatism.

This is the key limit on future proof-only remote recovery:
the current semantic anchor commits to historical members and candidate
objects, not to server-maintained secondary query indexes.

Witness encoding also needs one explicit starting point.

Because the semantic objects above are already content-addressed, a future
`hit-proof` response should not invent a second proof tree format.

The better default is:

1. object bytes are carried exactly as canonical engine payload bytes;
2. map witnesses are carried as exact canonical `prolly-map-v1` node payload
   bytes along one search path;
3. the client recomputes every referenced hash locally from those bytes.

That choice is deliberate:

- the note already defines canonical hashing for `prolly-map-v1`, root objects,
  and value blocks;
- sending exact node payload bytes makes the proof format a transport of
  already-defined semantic objects rather than a second serialization scheme;
- local recomputation keeps the trust boundary aligned with the rest of the
  design: bytes first, hashes second, server claims never trusted directly.

One concrete future witness bundle for a candidate-validity proof should
therefore contain:

- `semantic-recovery-v1` object bytes;
- `recovery-meta-v1` object bytes;
- for `members_root`:
  - every canonical `prolly-map-v1` node payload on the search path from the
    root to the leaf that contains the queried `(path_digest, candidate_id)`
    entry;
  - the `history-member-v1` value object bytes referenced by that leaf entry's
    `value_hash`;
- for `candidate_objects_root`:
  - every canonical `prolly-map-v1` node payload on the search path from the
    root to the leaf that contains the queried `candidate_id`;
  - the `candidate-object-ref-v1` value object bytes referenced by that leaf
    entry's `value_hash`;
- the referenced `trace-v1`, `result-v1`, and `dep-key-set-v1` object bytes;
- any query-class-specific semantic object bytes needed to rerun the query
  predicate locally.

The client-side verification algorithm for that future proof is then:

1. hash the supplied `semantic-recovery-v1` bytes and verify that the result is
   the echoed semantic anchor;
2. hash the supplied `recovery-meta-v1` bytes and verify that the
   `semantic-recovery-v1` payload references that hash and names the echoed
   recovery key;
3. for each supplied `prolly-map-v1` witness path:
   - hash every leaf/internal node payload according to the already-defined
     `prolly-map-v1` rules;
   - verify parent `child_hash` links bottom-up until the claimed map root is
     reached;
   - verify that the terminal leaf actually contains the queried key and the
     expected `value_hash`;
4. hash the supplied `history-member-v1` / `candidate-object-ref-v1` value
   bytes and verify they match the `value_hash` referenced by those leaf
   entries;
5. hash the supplied `trace-v1`, `result-v1`, and `dep-key-set-v1` bytes and
   verify they match the object references reached from the witness and from
   each other;
6. rerun the query-class-specific predicate locally from those semantic
   objects;
7. only then accept the remote candidate as semantically valid.

This future witness format still proves only candidate validity, not canonical
winner completeness, for the general recovery classes discussed above.

That requirement is why v1 keeps remote substitution at the artifact level.
Without either proof-carrying responses or a fetched artifact closure, a remote
server could answer from a different semantic recovery universe than the client
intended and the client would have no principled way to detect it.

### Portable Block Transport

Package descriptors are not enough to install state by themselves.
An install flow also needs the referenced immutable block graph.

V1 therefore uses one explicit transport envelope:

- `block-archive-v1`

Its job is narrow:

- carry the immutable blocks referenced by one or more package descriptors;
- allow `install*Package(...)` to ingest those blocks via `put(payload_bytes) ->
  block_hash`;
- provide an optional archive-local index for fast block lookup during import;
- remain transport-only rather than becoming a live retained namespace or a
  bespoke network protocol.

The design options considered were:

- one Nix store closure containing a directory / NAR of exported engine blocks,
  using Nix's store-object transport format itself as the only archive shape;
- CARv2, which provides a content-addressed archive with an optional index for
  fast random block lookup, but is CID/multihash-centered and still marked
  draft;[^carv2]
- OCI image layout, which provides an index plus opaque content-addressed blobs
  and explicitly supports multiple transport mechanisms, but adds layout and
  descriptor machinery that is broader than the engine needs for this one
  archive format;[^oci-image-layout]
- Git bundles, which transport packfiles plus refs/prerequisites, but are tied
  to Git object and ref semantics rather than arbitrary immutable block
  closure.[^git-bundle]

V1 therefore chooses a custom archive:

- one `block-archive-v1` is a self-contained deterministic sequence of
  canonical block records plus a deterministic sorted index trailer;
- its header contains:
  - format version;
  - a sorted list of advertised root hashes whose closure the archive claims to
    carry;
  - block count;
  - index offset;
- each block record contains the semantic `block_hash`, payload length, and raw
  payload bytes;
- canonical export emits block records in strict lexicographic `block_hash`
  order;
- each `block_hash` may appear at most once;
- import must derive `computed_block_hash = put(payload_bytes)` or an
  equivalent prepublication hash from each record's raw payload bytes and reject
  the archive unless `computed_block_hash == record.block_hash`;
- the archive must contain exactly the transitive closure of the advertised root
  set:
  - missing referenced blocks are invalid;
  - any record whose declared `block_hash` disagrees with its payload bytes is
    invalid;
  - duplicate or out-of-order block records are invalid;
  - extra blocks not reachable from the advertised root set are invalid;
- the sorted index trailer maps `block_hash -> byte offset` in the same strict
  order as the block records, and every index entry must match the validated
  record sequence exactly;
- install must verify both archive structure and exact closure before publishing
  any live refs.

That choice is intentional even though transport may happen through ordinary
Nix store closures.

Consequences:

- Nix store-object substitution remains the outer transport, but
  `block-archive-v1` remains the engine's portable closure format;
- the engine does not have to treat NAR directory layout, store references, or
  store-object chunking as part of its semantic import contract;
- one exported closure can therefore be fetched through existing substituters,
  while the engine still validates one deterministic block-level archive after
  fetch;
- the "what bytes constitute the semantic block closure?" question is answered
  by `block-archive-v1`, not by incidental properties of a directory tree
  inside a substituted NAR.

This keeps the live block model unchanged:

- semantic identity stays in the blocks and roots, not in the archive wrapper;
- archive format evolution is transport-only;
- install is executable because the referenced blocks are supplied directly with
  the package descriptors.

These stricter rules are intentional:

- package install now depends on `block-archive-v1` as an executable transport
  surface, not just as a conceptual envelope;
- exact-closure export makes archive bytes reproducible for the same advertised
  root set;
- rejecting extra or duplicate blocks keeps install mechanically checkable
  rather than turning the archive into an underspecified bag of blocks.[^carv2]
  [^oci-image-layout]

The recovery refs follow the same split:

- `refs/recovery-semantic/<recovery-ref>.ref` publishes the semantic recovery
  root for one `stable_recovery_key`;
- `refs/recovery-compiled/<compiled-ref>.ref` publishes one optional compiled
  recovery root for one `(stable_recovery_key, base_semantic_root_hash)` pair.

Compiled refs are local acceleration only.
Implementations may delete compiled refs for older semantic roots
opportunistically once those refs are no longer the current compiled view for
that `stable_recovery_key`.
Removing such a compiled ref does not change semantic recovery identity.

### Shape

Each semantic recovery namespace instance consists of:

1. `semantic-recovery-v1`
   - `members_root`
   - `candidate_objects_root`
   - `recovery_meta_hash`
2. `recovery-meta-v1`
   - serialized `stable_recovery_key`
   - feature flags
   - format version

Each compiled recovery namespace instance consists of:

1. `compiled-recovery-v1`
   - `base_semantic_root_hash`
   - `bootstrap_index_root`
   - `git_index_root`
   - `direct_index_root`
   - `struct_profile_index_root`
   - `compiled_recovery_meta_hash`
2. `compiled-recovery-meta-v1`
   - serialized `stable_recovery_key`
   - `base_semantic_root_hash`
   - feature flags
   - format version

These root objects make the recovery boundary mechanically checkable:

- a semantic recovery root hash always names one `semantic-recovery-v1` object;
- a compiled recovery root hash always names one `compiled-recovery-v1` object;
- a compiled recovery root is valid only if both its namespace key and its
  `compiled-recovery-meta-v1.base_semantic_root_hash` match the semantic
  recovery root it is being paired with.

### Base Member Map

One map:

- key: `path_digest || candidate_id`
- value: `history-member-v1`

This is the semantic source of truth for history membership.
It is a set, not an append log.

### Candidate Object Map

One map:

- key: `candidate_id`
- value: `candidate-object-ref-v1`

`candidate-object-ref-v1` contains:

- `trace_record_hash`

In v1, `candidate_id == trace_record_hash`, so this duplicates the key bytes.
That duplication is intentional:

- GC and export closure must not depend on special-case parsing of map keys;
- the value is the explicit semantic edge from the recovery root into the core
  immutable object graph;
- if candidate identity is ever widened in a future version, this root shape
  still works without redefining semantic reachability.

This map is part of the semantic recovery root.
It is not compiled acceleration state.

`candidate_objects_root` is not an independent semantic set.
It is a canonical derived view of `members_root`:

1. collect the distinct `candidate_id` values that appear in `members_root`;
2. emit exactly one `candidate-object-ref-v1` entry for each such
   `candidate_id`;
3. emit no extra entries;
4. in v1, require `candidate-object-ref-v1.trace_record_hash == candidate_id`.

Extra entries, missing entries, or mismatched `trace_record_hash` values are
hard corruption.
This exact-image rule is part of semantic recovery canonicalization.

Semantic recovery closure is walked from explicit object edges:

1. `semantic-recovery-v1` references `members_root` and `candidate_objects_root`;
2. `semantic-recovery-v1` references `recovery_meta_hash`;
3. `candidate-object-ref-v1` references `trace_record_hash`;
4. `trace-v1` references `result_hash` and `dep_key_set_hash`.

GC and export closure follow those edges.
They do not infer reachability from secondary indexes or from special-case
interpretation of map keys.

Compiled recovery closure is walked from explicit object edges too:

1. `compiled-recovery-v1` references `bootstrap_index_root`,
   `git_index_root`, `direct_index_root`, `struct_profile_index_root`, and
   `compiled_recovery_meta_hash`;
2. every compiled index root references only compiled acceleration objects;
3. deleting compiled recovery refs and compiled-only blocks must not affect
   semantic recovery closure.

### Compiled Recovery Indexes

If a compiled recovery root is present, its `compiled-recovery-v1` object points
at these additional roots.

The compiled recovery root is validated-on-publication acceleration state:

Three publication-boundary choices were considered:

1. allow only the view-level recovery-preflight helpers
   `installCompiledRecovery(...)` and `ensureCompiledRecovery(...)` to publish
   `recovery-compiled` refs;
2. allow those view-level helpers plus the standalone shipped-package installer
   `installCompiledRecoveryForRoot(...)` to publish `recovery-compiled` refs;
   or
3. allow evaluator-facing local publication to warm or replace compiled refs
   opportunistically after semantic writes.

V1 chooses the second rule.

Consequences:

- one authoritative validation boundary still exists for compiled recovery
  publication;
- shipped compiled artifacts remain installable without first opening a pinned
  eval view;
- evaluator-facing local publication remains outside the compiled publication
  path and cannot weaken the validation boundary by opportunistic cache
  warming.

Accordingly:

- `installCompiledRecoveryForRoot(...)`, `installCompiledRecovery(...)`, and
  `ensureCompiledRecovery(...)` are the only operations allowed to publish
  `recovery-compiled` refs;
- those operations must fully validate or rebuild the compiled root against the
  semantic recovery root before publication;
- `openEvalView()` may use a published compiled ref after namespace-key and
  `base_semantic_root_hash` checks, because publication itself is the
  validation boundary;
- if the published compiled root or its metadata use an unsupported compiled
  format/version, `openEvalView()` must treat that compiled root as absent and
  fall back to a semantic-only view;
- if an implementation bypasses those publication operations, any resulting
  compiled ref is out of spec;
- any mismatch found during explicit validation is hard corruption in the
  compiled layer, not a semantic member-set mismatch.

For compiled-recovery preflight return semantics, three plausible contracts were
considered:

1. exact-return semantics for both `installCompiledRecovery(...)` and
   `ensureCompiledRecovery(...)`;
2. "any currently valid compiled root for this semantic root" semantics for
   both helpers;
3. exact-return for package-directed install, but current-valid semantics for
   local "ensure" preflight.

V1 chooses the third contract:

- package-directed `installCompiledRecovery(...)` must return a view pinned to
  the exact compiled root that won or matched that install path; and
- local `ensureCompiledRecovery(...)` may adopt a concurrently published,
  validated compiled root for the same semantic recovery root rather than
  forcing its locally built compiled root to win.

That split is intentional:

- shipped compiled artifacts remain auditable and deterministic at the service
  boundary;
- local ensure/preflight remains a best-effort acceleration path and therefore
  tolerates concurrent compiled-cache publication without unnecessary
  determinism failures or replacement churn;
- both helpers still must pin the exact compiled root they actually return
  before exposing the replacement view to the caller.

### Portable Install And Export Flow

There are five distinct operations:

1. install coherent evaluator package:
   - accept one `eval-package-v1` payload plus one `snapshot-pack-v1` payload
     and one `recovery-pack-v1` payload as install inputs, plus one
     `block-archive-v1` carrying the referenced semantic block closure;
   - validate that the supplied `snapshot-pack-v1` and `recovery-pack-v1`
     payloads match the keys and root hashes named by `eval-package-v1`;
   - validate that `block-archive-v1.advertised_root_hashes` is exactly the
     sorted pair `{ snapshot_root_hash, semantic_recovery_root_hash }` named by
     the supplied descriptors;
   - ingest the supplied semantic archive into the local block store and verify
     that it contains the full closure of the referenced `snapshot-v1` and
     `semantic-recovery-v1` roots;
   - validate the referenced `snapshot-v1` and `semantic-recovery-v1` roots and
     their semantic closure directly from the supplied package payloads;
   - construct the corresponding `eval-view-v1` root from the validated keys and
     root hashes;
   - define coherent install success by the `refs/eval/<eval-ref>.ref` update,
     alone;
   - if the current `refs/eval/<eval-ref>.ref` is absent, publish it;
   - if it already points at the same `eval-view-v1` root, treat install as a
     successful no-op through `confirm_current_ref_and_pin_exact(...)`;
   - if it points at a different `eval-view-v1` root, atomically replace that
     coherent evaluator head;
   - if the final coherent-head CAS loses to another writer, re-read the current
     coherent head:
     - if it now points at the same `eval-view-v1` root, treat install as a
       successful no-op through `confirm_current_ref_and_pin_exact(...)`;
     - otherwise retry or return a retryable contention error;
   - two success-return choices were considered:
     - reopen by `(semantic_session_key, stable_recovery_key)` through the
       normal `openEvalView(...)` protocol; or
     - return the exact committed pinned `EvalReadView` for the `eval-view-v1`
       root that won or matched that install path;
   - v1 chooses the second rule;
   - consequence: coherent install cannot accidentally bind a later evaluator
     head published after the successful install path, and this transport/install
     flow now matches the service and validation sections;
   - on success, return that exact committed pinned `EvalReadView`, not a later
     namespace reopen by key;
   - coherent install must not publish or replace raw snapshot or semantic
     recovery heads as a side effect.
2. install raw semantic snapshot head:
   - deserialize `snapshot-pack-v1`;
   - validate that `block-archive-v1.advertised_root_hashes` is exactly the one
     `snapshot_root_hash` named by `snapshot-pack-v1`;
   - ingest a supplied `block-archive-v1` that contains the referenced snapshot
     closure;
   - validate `snapshot-v1` and its semantic closure;
   - if the current raw snapshot ref for that `semantic_session_key` is absent,
     publish it;
   - if it already points at the same `snapshot_root_hash`, treat install as a
     no-op;
   - if it points at a different `snapshot_root_hash`, replace that raw head
     atomically.
3. install raw semantic recovery head:
   - deserialize `recovery-pack-v1`;
   - validate that `block-archive-v1.advertised_root_hashes` is exactly the one
     `semantic_recovery_root_hash` named by `recovery-pack-v1`;
   - ingest a supplied `block-archive-v1` that contains the referenced semantic
     recovery closure;
   - validate `semantic-recovery-v1` and its semantic closure;
   - if the current raw semantic recovery ref for that `stable_recovery_key` is
     absent, publish it;
   - if it already points at the same `semantic_recovery_root_hash`, treat
     install as a no-op;
   - if it points at a different `semantic_recovery_root_hash`, replace that raw
     head atomically.
4. install shipped compiled acceleration:
   - deserialize `compiled-recovery-package-v1`;
   - validate that `block-archive-v1.advertised_root_hashes` is exactly the one
     `compiled_recovery_root_hash` named by `compiled-recovery-package-v1`;
   - ingest a supplied `block-archive-v1` that contains the referenced compiled
     recovery closure;
   - verify its `stable_recovery_key` and `base_semantic_root_hash` against the
     caller-provided semantic recovery root or pinned `EvalReadView`, not
     against any raw semantic-recovery head;
   - validate the referenced `compiled-recovery-v1` object and its derived
     indexes against that semantic recovery root;
   - two contention policies were considered for this standalone install path:
     - silently adopt whatever different compiled root becomes current after a
       lost CAS; or
     - treat same-format divergence as hard corruption and permit retry only
       when a different compiled format/version won the race;
   - v1 chooses the second rule so that standalone package-directed install
     remains deterministic for one semantic root and one compiled
     format/version;
   - define standalone compiled-install success by the
     `refs/recovery-compiled/<compiled-ref>.ref` update alone;
   - if no compiled ref exists for that namespace key, attempt to publish
     `refs/recovery-compiled/<compiled-ref>.ref`;
   - if a compiled ref already exists for that namespace key:
     - if it points at the same `compiled_recovery_root_hash`, treat install as
       a no-op;
     - otherwise load the existing `compiled-recovery-meta-v1`;
     - if the existing compiled format/version equals the incoming compiled
       format/version, fail with hard corruption because determinism for one
       semantic root and one format/version was violated;
     - if the existing compiled format/version differs, attempt to replace the
       old local compiled ref atomically with the newly validated one.
   - if the compiled-ref CAS loses to another writer, re-read the current
     compiled ref for that exact namespace key:
     - if it now points at the same `compiled_recovery_root_hash`, treat
       install as a successful no-op;
     - otherwise load the current `compiled-recovery-meta-v1`;
     - if the current compiled format/version equals the incoming compiled
       format/version, fail with hard corruption because determinism for one
       semantic root and one format/version was violated;
     - if the current compiled format/version differs, retry or return
       retryable contention without claiming success for a different compiled
       root.
   - this standalone install path is keyed by the caller-provided semantic
     recovery root and is the install counterpart to standalone compiled
     recovery export.
5. ensure local compiled acceleration:
   - if no valid compiled ref exists for the pinned semantic root, rebuild and
     publish one locally.

Portable transport and live maintenance are intentionally separate:

- coherent import through `installEvalPackage(...)` publishes only the coherent
  evaluator head plus any explicitly requested compiled acceleration;
- it does not move raw snapshot or raw semantic-recovery heads;
- raw snapshot and raw semantic-recovery refs remain local maintenance heads,
  updated only by local publication and the raw-head install helpers above.

This split is intentional:

- `openEvalView()` stays read-only;
- coherent evaluator import is explicit and single-step rather than inferred
  from separate snapshot and recovery installs;
- raw semantic snapshot/recovery installs are maintenance-only helpers, not the
  normal evaluator import path;
- installing shipped compiled acceleration is explicit and verifiable;
- local rebuild remains distinct from package install.

Coherent export is also explicit:

1. open one pinned `EvalReadView`;
2. emit `snapshot-pack-v1` from `view.snapshot_root_hash`;
3. emit `recovery-pack-v1` from `view.semantic_root_hash`;
4. emit `eval-package-v1` that names exactly those two semantic roots;
5. optionally emit `compiled-recovery-package-v1` for the same
   `view.semantic_root_hash`.

Export tools must not synthesize a coherent evaluator export by separately
reading the current snapshot and semantic recovery refs outside one pinned
`EvalReadView`.

The coherent `semantic_archive` emitted by `exportEvalPackage(view)` is a
two-root archive for coherent install only.
It is not a valid raw-install archive for `installSnapshotPackage(...)` or
`installRecoveryPackage(...)`, because those helpers require exact single-root
archives.

Standalone semantic export is explicit too:

1. `exportSnapshotPackage(snapshot_root_hash)` emits one `snapshot-pack-v1` plus
   one `block-archive-v1` whose advertised root set is exactly
   `{ snapshot_root_hash }`;
2. `exportRecoveryPackage(semantic_root_hash)` emits one `recovery-pack-v1` plus
   one semantic `block-archive-v1` whose advertised root set is exactly
   `{ semantic_root_hash }`;
3. if compiled recovery is also exported for that semantic root, its
   `compiled-recovery-package-v1` companion uses a separate compiled archive
   whose advertised root set is exactly `{ compiled_recovery_root_hash }`.

This closes the transport gap created by exact-root-set archives:

- `snapshot-pack-v1` and `recovery-pack-v1` remain valid standalone semantic
  export forms for narrower use cases;
- they are now executable because their install helpers have matching
  single-root export helpers, rather than relying on callers to carve those
  roots out of the coherent semantic archive;
- standalone compiled recovery export is likewise executable because the
  service boundary exposes a matching standalone compiled install helper keyed
  by the semantic recovery root.

#### `bootstrap-index-v1`

- key: `path_digest`
- value: `candidate_set_root_hash`

This replaces the current "use `history.front()`" miss bootstrap path.[^live-history-order]

#### `git-index-v1`

- key: `path_digest || git_identity_hash`
- value: `candidate_set_root_hash`

This preserves the live GitIdentity fast path while removing dependence on row
recency.[^live-recovery]

#### `direct-index-v1`

- key: `path_digest || trace_hash`
- value: `candidate_set_root_hash`

This preserves direct-hash recovery even if multiple candidates share one
`trace_hash`.

#### `struct-profile-index-v1`

- key: `path_digest || dep_key_set_hash || struct_profile_hash`
- value: `struct-profile-summary-v1`

`struct-profile-summary-v1` contains:

- `dep_key_set_hash`
- `struct_profile_hash`
- `structural_override_vector`
- `profile_candidate_count`
- recoverability flags derived from the profile

`structural_override_vector` is the ordered list of:

- dep position in dep-key order
- historical stored hash value

for exactly those dep positions whose stored historical hash may be reused by
the `StructVariantDep` path rather than recomputed from current state.

This replaces the earlier "one representative candidate per dep-key-set group"
design.
That older design was too weak: the live algorithm already relies on a single
representative only as a heuristic, and it does not prove that one historical
trace is enough when multiple candidates share a dep-key set but differ in the
stored hashes that structural subsumption may reuse.[^live-struct-profile]

The new rule is:

- structural recovery indexes every distinct structural-recovery profile, not
  just one representative candidate;
- candidates that differ only in fields irrelevant to structural recovery may
  collapse to one profile;
- candidates whose stored structural hashes differ must remain in distinct
  profiles.

`struct-profile-summary-v1` is also derived acceleration state.
It is not semantic truth.
Import may keep persisted structural-profile summaries only if it validates
them, or it may discard and rebuild them from:

- the semantic base member map;
- `trace-v1`
- `dep-key-set-v1`

Any mismatch between a persisted profile summary and the canonical rebuilt
profile summary is hard corruption.

### Candidate Ordering Is A Deliberate Semantic Change

The live system picks "latest inserted" in several places.[^live-history-order]
That cannot be both portable and history-invariant.

This note therefore makes the breaking change explicit:

- recovery candidate order is the lexicographic order of `candidate_id`;
- bootstrap, GitIdentity, direct-hash ties, and structural-profile ties all use
  that same canonical order;
- if multiple candidates verify, the first verifying candidate in canonical
  order wins.

This is not presented as "preserving current behavior".
It is an intentional semantic rewrite made necessary by the history-invariance
requirement.

### Recovery Algorithms

The algorithms below assume the `EvalReadView` already has a compiled
recovery root.
If only the semantic recovery root is present, the caller should first ensure a
compiled recovery root exists via the service operation below.

#### Snapshot Miss Bootstrap

1. look up `bootstrap-index-v1[path_digest]`;
2. iterate candidate summaries in canonical `candidate_id` order;
3. run the same verification predicate as the live system;
4. accept the first candidate that verifies.

#### GitIdentity Recovery

1. extract the current repo root from the current dep set;
2. reject if the current dep set is not Git-recoverable for that repo root;
3. compute the current GitIdentity hash;
4. look up `git-index-v1[(path_digest, git_identity_hash)]`;
5. iterate candidate summaries in canonical `candidate_id` order;
6. require `git_recoverable = true` and `git_repo_root_digest` to match the
   current repo-root digest;
7. accept the first matching candidate using the inlined
   `encoded_result_payload-v1`.

#### Direct-Hash Recovery

1. recompute current dep hashes;
2. compute the candidate `trace_hash`;
3. look up `direct-index-v1[(path_digest, trace_hash)]`;
4. iterate candidate summaries in canonical `candidate_id` order, using the
   inlined `encoded_result_payload-v1` on acceptance.

#### Structural-Variant Recovery

1. prefix-scan `struct-profile-index-v1` for the path digest;
2. for each profile, load `dep_key_set_hash` and
   `structural_override_vector`;
3. resolve current dep values from `dep-key-set-v1`, reusing the stored hashes
   from `structural_override_vector` only for dep kinds that the
   `StructVariantDep` path is allowed to subsume;
4. compute the candidate `trace_hash`;
5. look up `direct-index-v1[(path_digest, trace_hash)]`;
6. iterate candidate summaries in canonical `candidate_id` order, using the
   inlined `encoded_result_payload-v1` on acceptance.

### Honest Complexity Statement

The earlier note overstated this.

The bounds are:

- exact snapshot lookup: `O(log B)`
- bootstrap lookup: `O(log P + v)`
- GitIdentity lookup: `O(log G + v)`
- direct-hash recovery: `O(dep_resolution + log D + v)`
- structural-variant recovery: `O(log S + p * (profile_dep_resolution + log D + v))`

where:

- `B` = bindings in one snapshot namespace
- `P` = paths with bootstrap candidate sets
- `G` = GitIdentity index size
- `H` = history member count
- `D` = direct-hash index size
- `S` = structural-profile index size
- `p` = number of structural-recovery profiles for the requested path
- `v` = number of candidates actually tried before success or exhaustion

Structural-variant recovery is still linear in the number of structural
profiles for that path.
The gain is that it is no longer linear in raw append history.

### Incremental Update Model

The earlier "rebuild deterministic indexes" phrasing was too vague and too
expensive-sounding.

Adding one new history member updates only:

1. one `history-members` entry;
2. one bootstrap candidate set;
3. zero or one GitIdentity candidate set;
4. one direct-hash candidate set;
5. zero or one structural-profile summary;
6. zero or one structural-profile index entry.

All of those are persistent map or set updates with `O(log N)` node rewrites.
There is no full recovery-pack rebuild on each insertion.

## Stat-Hash Namespace

### Base Shape

One map:

- key: `stat-hash-key-v1`
- value: `stat-hash-value-v1`

`stat-hash-key-v1` contains:

- physical path bytes
- dep kind
- file fingerprint:
  `dev`, `ino`, `mtime_sec`, `mtime_nsec`, `size`

`stat-hash-value-v1` contains:

- dep hash bytes

This replaces the current `(path, dep_type) -> (fingerprint, hash)` mutable row
store.[^live-stat-hash]

### Supported Query Classes

The stat-hash namespace is only sound for query classes whose full input is
proven by the cached fingerprint.

In v1:

- regular-file byte hashes are allowed;
- single-directory entry-listing hashes are allowed only if the implementation
  hashes one opened directory handle and validates the same directory handle
  before and after hashing;
- recursive tree hashes such as `NarIdentity` are not stored in the stat-hash
  namespace.

Caching recursive tree hashes by root-path `stat` data alone is not sound,
because subtree changes can invalidate the tree hash without giving the storage
engine a trustworthy tree-stability proof.

### `tree-hash-cache-v1`

Recursive tree hashing is handled by a separate machine-local namespace:
`tree-hash-cache-v1`.

The key is `tree-hash-key-v1`.
The key is not root-path `stat`.
The cache has two entry families:

1. leaf entries
   - key: validated leaf fingerprint
   - value: the NAR-form leaf digest for one regular file or symlink
2. tree entries
   - key: canonical ordered vector of child name, child kind, and child digest
   - value: the recursive tree hash used for `NarIdentity`

For regular-file leaves, the validated leaf fingerprint includes:

- the opened-file descriptor fingerprint
- the mode bits that affect NAR output

For symlink leaves, it includes:

- canonical target bytes

Directory nodes are keyed by child digests, not by root-path `stat`.

This keeps recursive tree hashing performant without pretending root `stat`
proves subtree stability:

- repeated hashing of the same large tree still walks directory structure, but
  it can reuse validated NAR-form leaf digests and cached subtree hashes rather
  than rereading every file and rerunning full NAR serialization;
- the cache composes from the same child ordering and serialization semantics
  already used by NAR dumping and directory hashing in-tree.[^in-tree-nar]
  [^live-nar-identity]

This is a real design choice, not a deferred optimization.
The rewrite should not regress recursive tree hashing all the way back to
"always stream the whole tree again".

### Correctness Rule

The live store has a TOCTOU gap:

1. `lstat`
2. maybe reuse fingerprint
3. compute hash
4. store the hash against the earlier fingerprint[^live-stat-hash]

The new engine must not freeze that weakness into the design.

For regular files, cached publication is:

1. open the file;
2. `fstat` it to obtain `fingerprint_before`;
3. hash bytes from that open file descriptor;
4. `fstat` the same file descriptor again to obtain `fingerprint_after`;
5. publish only if `fingerprint_before == fingerprint_after`.

For directory-entry hashes:

1. open the directory;
2. `fstat` it before hashing;
3. hash the entry vector from that opened directory;
4. `fstat` the same directory again;
5. publish only if the directory fingerprint is unchanged.

If the fingerprints disagree, return the computed hash to the caller but do not
publish it to the stat-hash namespace.

This is the same general defense Git applies to racily-clean cache entries:
fall back to a stronger content check when stat information alone is not enough
to prove stability.[^git-racy]

### Scope

The `stat-hash-v1` and `tree-hash-cache-v1` namespaces remain machine-local and
operational.
They are not part of a portable snapshot or recovery export.
They do not need to share the semantic block/ref/pin/GC engine merely for
uniformity.
V1 allows them to use SQLite-backed, process-local memory-fronted, or
equivalent machine-local cache
implementations, provided that:

- they remain non-authoritative and rebuildable;
- they are not named by portable snapshot, recovery, or eval-package roots;
- evaluator coherence does not depend on their transaction boundaries; and
- cache misses or invalidations fall back to semantic recomputation rather than
  to "trust the local DB".

## Identity Preservation

The plan still preserves current identity-sensitive warm-hit behavior.

The live system depends on persisted identity in three places:

1. result payloads encode publication identity for strings and paths;[^live-result-codec]
2. result payloads encode container identity stamps for lists and attrsets;[^live-result-codec]
3. materialization restores that identity and `sameValueIdentity()` consults
   it at runtime.[^live-materialize][^live-identity]

So the new rule is:

1. persist deterministic identity classes in `result-v1`;
2. on materialization, allocate fresh runtime stamps from those classes;
3. never persist raw `nextValueIdentityStamp++` values as semantics.

That preserves current warm-hit behavior without leaking session-local stamp
numbers into persistent bytes.

## Service Boundary Rewrite

The previous note understated this.
The service rewrite is not optional.

The current async boundary is built around row IDs:

- `AttrPathId`
- `TraceId`
- `ResultId`
- `DepKeySetId`[^live-threading][^live-service-ids]

Those persistent identities disappear in the new engine.

### New Boundary

The replacement service should traffic in:

- `EvalReadView { semantic_session_key, stable_recovery_key, snapshot_root_hash, semantic_root_hash, compiled_root_hash? }`
- `EvalSessionHandle`
- `TraceRef { trace_hash }`
- `TraceRecordRef { trace_record_hash }`
- `ResultRef { result_hash }`
- `DepKeySetRef { dep_key_set_hash }`
- `PathKeyRef { path-key-v1 bytes }`

`EvalSessionHandle` is the bound evaluator capability, not just a struct alias
for `EvalReadView`.
It owns:

- one pinned coherent `EvalReadView`;
- one immutable registry seed containing the graph-derived entries and mount
  points that existed before session-local runtime-root discovery;
- one deduplicated session-local runtime-root mount overlay accumulated during
  the current session's cold-eval path;
- the verified runtime-root result merged into the session's in-memory semantic
  registry;
- the bound per-session evaluator state needed before verification or cold eval
  may proceed.

This mirrors the live `bindSession()` rule:

- low-level storage open and high-level evaluator bind are distinct phases in
  the current implementation;[^live-session-open]
- the rewrite should preserve that separation by making evaluator-facing
  verification/cold-eval entry points require `EvalSessionHandle`, not bare
  `EvalReadView`.

`EvalReadView` has these required invariants:

- it is derived from exactly one validated `eval-view-v1`;
- if `refs/eval/<eval-ref>.ref` exists, that `eval-view-v1` comes from the
  published ref after namespace-kind, namespace-key, format-version, and
  embedded-meta validation;
- if `refs/eval/<eval-ref>.ref` is absent, that `eval-view-v1` is the canonical
  empty `eval-view-v1` synthesized for the requested
  `(semantic_session_key, stable_recovery_key)` pair;
- `snapshot_root_hash` comes from that validated `eval-view-v1` and names a
  `snapshot-v1` whose `snapshot-meta-v1.semantic_session_key` equals the
  requested `semantic_session_key`;
- `semantic_root_hash` comes from that same validated `eval-view-v1` and names
  a `semantic-recovery-v1` whose `recovery-meta-v1.stable_recovery_key` equals
  the requested `stable_recovery_key`;
- if `compiled_root_hash` is present, it names a `compiled-recovery-v1` whose
  `compiled-recovery-meta-v1.stable_recovery_key == stable_recovery_key` and
  whose `compiled-recovery-meta-v1.base_semantic_root_hash ==
  semantic_root_hash`;
- if the view was synthesized from an absent eval ref, `compiled_root_hash` is
  absent.

The API discipline is mandatory:

- evaluator code must not stop at `openEvalView()` when runtime-root-backed
  provenance resolution is in play;
- the supported evaluator/session entry point is one bound session-open
  operation that opens the pinned view, hydrates verified runtime roots into the
  in-memory semantic registry, and only then exposes the session for
  verification or cold eval, matching the live `TraceSession` setup order;[^live-session-open]
- if evaluator code must bind one already-opened or newly imported coherent
  view exactly, it must do so through an explicit bind-from-view operation
  rather than by reopening namespace refs by key;
- verification- and cold-eval-facing operations must require
  `EvalSessionHandle`; they must not accept bare `EvalReadView`;
- one verification session opens at most one `EvalReadView`;
- one recovery session may replace its initially opened `EvalReadView` at most
  once during compiled-recovery preflight via `installCompiledRecovery(...)` or
  `ensureCompiledRecovery(...)`; after that optional preflight replacement, the
  resulting view is reused for the full recovery pipeline;
- a recording session owns one `EvalSessionHandle`;
- after a successful evaluator-facing `publishRuntimeRoot(...)` or
  `publishRecord(...)`, that recording session must replace its current
  `EvalSessionHandle` with the returned replacement handle and use that handle
  for subsequent current-state reads and evaluator operations;
- opening views per lookup is not supported in v1.

### Evaluator Integration Seam

The storage/API rewrite still has to fit the live evaluator control flow.

Today that seam is not "call storage directly from `TracedExpr`".
It is:

- `TraceSession` owns one polymorphic `TraceBackend`;
- `TracedExpr` calls `verify(...)`, `record(...)`, `loadFullTrace(...)`,
  `getCurrentTraceHash(...)`, `recordRuntimeRoot(...)`, and
  `loadAndVerifyRuntimeRoots(...)` through that backend; and
- `StoreTraceBackend` hides the async/store/orchestrator details behind that
  narrower interface.[^live-backend-interface]

Three migration shapes were considered:

1. delete `TraceBackend` and thread `EvalSessionHandle` / `EvalReadView`
   directly through `TracedExpr`, `TraceSession`, and materialization code;
2. keep `TraceBackend` as the evaluator-facing seam, but rewrite
   `StoreTraceBackend` so it owns the new bound-session/read-view state and
   translates between the legacy evaluator control flow and the new engine
   primitives;
3. keep the existing row-ID-oriented seam (historically
   `TraceStoreService` + `VerificationOrchestrator`; `TraceStoreService`
   has since been removed, so this shape now means the
   `VerificationOrchestrator` seam alone) and only swap persistence
   under it.

V1 chooses the second shape.

That choice is deliberate:

- the note already rejects preserving row-ID / `NodeStamp` / sqlite-strand
  identity as the long-term semantic boundary;
- directly rethreading `EvalSessionHandle` through every `TracedExpr`,
  replay, materialization, and backend call would turn the storage rewrite into
  a larger evaluator-control-flow rewrite than necessary for v1;
- the current `TraceBackend` seam is already the place where binding, runtime
  root persistence, verification, recording, and flush are abstracted away from
  `TraceSession`.

Consequences:

- `TraceBackend` remains the evaluator-facing abstraction in v1;
- `StoreTraceBackend` becomes the adapter from evaluator operations to the new
  engine service boundary;
- keeping that abstraction does not preserve row-ID method signatures:
  any `TraceBackend` operation whose current type mentions `TraceId`,
  `ResultId`, or `DepKeySetId` must be rewritten to use hash-derived refs
  (`TraceRef`, `TraceRecordRef`, `ResultRef`, `DepKeySetRef`) or to keep such
  identities fully internal to the adapter; persistent row IDs must not remain
  visible across the evaluator/backend seam;
- `TraceSession` continues to own one backend object, but that backend now owns
  the current bound `EvalSessionHandle` and any lower-level `EvalReadView`
  preflight state it needs;
- evaluator code such as `TracedExpr::eval()` and the existing session-open
  path do not call `openEvalView(...)` or `publishRecord(...)` directly.
  They continue to call backend methods that update or replace the backend's
  internally owned session handle.
- the existing backend-lifetime rule must survive the rewrite:
  `TraceSession::releaseBackend()` / `NullTraceBackend` (or an equivalent
  mechanism) must still let old GC-managed `TracedExpr` thunks fall back to
  direct evaluation after session teardown instead of retaining a dead engine
  backend or keeping its files pinned/open.[^live-backend-interface]

In particular, live shapes such as:

- `loadFullTrace(traceId)`
- `VerifyResult { ..., traceId }`
- `RecordResult { traceId }`

must migrate to hash-derived refs or backend-private ephemeral tokens rather
than preserving persistent row IDs as part of the rewritten seam.

Concretely, the migration contract is:

- `TraceBackend::bindSession(...)` maps to `openBoundEvalSession(...)` or
  `bindEvalView(...)` and stores the returned `EvalSessionHandle` inside the
  backend adapter;
- `TraceBackend::verify(...)` verifies against the backend's currently bound
  session handle;
  it must not remain an adapter over a row-ID-centered store API equivalent to
  `lookupCurrentNode(pathId) -> traceId -> verifyTrace(traceId, ...)`;
  the rewritten backend seam must drive verification from the pinned
  snapshot/current-binding view plus hash-derived candidate identity;
- `TraceBackend::record(...)` delegates to evaluator-facing
  `publishRecord(session, ...)`, then replaces the backend's stored
  `EvalSessionHandle` with the returned replacement handle before returning to
  `TracedExpr`;
  if evaluator code still needs a "trace identity" result for replay or memo
  bookkeeping, that result must be a hash-derived trace reference rather than a
  persistent row ID;
- `TraceBackend::recordRuntimeRoot(...)` delegates to
  `publishRuntimeRoot(session, ...)` and likewise replaces the backend's stored
  handle on success;
- `TraceBackend::loadAndVerifyRuntimeRoots(...)` becomes a thin compatibility
  entry for maintenance/tests rather than remaining an independent second
  source of truth for normal session open;
- the live constructor split:
  - `loadAndVerifyRuntimeRoots(...)`
  - merge verified roots into `registry_`
  - `bindSession(...)`
  must collapse in v1 into one adapter-owned setup path centered on
  `openBoundEvalSession(...)` / `bindEvalView(...)`;
  normal evaluator session construction must not keep a second pre-bind runtime
  root merge path alongside bind-owned hydration;[^live-backend-interface]
- any new engine-facing async service introduced for the file-based engine must
  be hidden behind `StoreTraceBackend` rather than exposed directly to
  `TracedExpr`.
- backend shutdown must still support the current session-cache lifetime model:
  releasing a session backend must drop the engine-backed adapter and clear any
  traced-root state that would otherwise keep old thunks bound to stale
  evaluator/session handles.

Required operations:

1. `openBoundEvalSession(state, registry, session_key, recovery_key) -> EvalSessionHandle`
   This is the supported evaluator-facing session-open path.
   It opens the pinned coherent view via `openEvalView(...)`, then binds that
   exact view through `bindEvalView(state, registry, view)`.
   `openEvalView(...)` by itself is a lower-level storage primitive for
   maintenance, export, import, and tests; it is not the supported evaluator
   session entry point when runtime-root-backed provenance resolution is
   required.[^live-session-open]
2. `bindEvalView(state, registry, view) -> EvalSessionHandle`
   This is the supported evaluator-facing bind step for one already-opened
   coherent view.
   `registry` is the immutable session registry seed, not a previously mutated
   live session registry.
   The bind step must construct a fresh session-local semantic registry from
   that seed, then call `loadAndVerifyRuntimeRoots(view)` and merge the
   verified persisted runtime roots into that fresh registry before binding the
   per-session evaluator state.
   In v1, each verified persisted runtime root contributes:
   - a forward entry `DepSource::fromRuntimeRoot(locked_url) -> store_path`;
   - no reverse mount-point entry is synthesized from persisted runtime-root
     metadata alone.
   Reverse runtime-root mount points remain session-local dynamic state,
   matching the live behavior where they are added during cold eval at mount
   time rather than restored from persisted runtime-root metadata.
   It then returns the bound session handle for exactly that pinned view.
   This is the path evaluator code must use after `installEvalPackage(...)`
   when it needs to operate on the exact imported coherent view rather than
   reopening by namespace key.
3. `openEvalView(session_key, recovery_key) -> EvalReadView`
   This is read-only.
   It opens one coherent pair of `snapshot_root_hash` and
   `semantic_recovery_root_hash` through `eval-view-v1`, then optionally pairs a
   compiled recovery root keyed by `(stable_recovery_key, semantic_root_hash)`.
   If no eval ref exists, it synthesizes the canonical empty `eval-view-v1` for
   the requested keys and returns the corresponding empty snapshot and semantic
   recovery view.
4. `installEvalPackage(eval_package, snapshot_package, recovery_package, semantic_archive) -> EvalReadView`
   This is the high-level coherent import path.
   It validates the supplied package payloads directly, publishes the matching
   coherent `eval-view-v1` head using the same-root/no-op and
   different-root/atomic-replace rules above through
   `compare_and_swap_and_pin_exact(...)`, then returns the committed pinned view
   for the exact committed `eval-view-v1` root that won or matched that
   successful CAS, not by reopening the moving namespace head.
   Evaluator code that needs to operate on that exact imported coherent state
   must then call `bindEvalView(state, registry, returned_view)` rather than
   reopening by key.
5. `installSnapshotPackage(snapshot_package, snapshot_archive) -> snapshot_root_hash`
   Maintenance-only helper for raw snapshot-head installation.
6. `installRecoveryPackage(recovery_package, recovery_archive) -> semantic_recovery_root_hash`
   Maintenance-only helper for raw semantic-recovery-head installation.
7. `installCompiledRecoveryForRoot(semantic_root_hash, compiled_package, compiled_archive) -> compiled_recovery_root_hash`
   Standalone helper for publishing shipped compiled acceleration against one
   caller-provided semantic recovery root.
   It validates the compiled package against that semantic recovery root and
   publishes the matching compiled ref without requiring a pinned eval view.
   It follows the same standalone compiled-install rules in the transport
   section above:
   - same root is a no-op;
   - same semantic root plus same compiled format/version but different
     compiled root is hard corruption;
   - different compiled format/version may replace atomically;
   - CAS loss may retry or return retryable contention, but must not claim
     success for a different compiled root.
8. `installCompiledRecovery(view, compiled_package, compiled_archive) -> EvalReadView`
   Validate `compiled_package` against `view.stable_recovery_key` and
   `view.semantic_root_hash`, publish the matching compiled recovery ref, then
   return a replacement view whose `compiled_root_hash` equals the exact
   compiled root that won or matched that install path.
   This is the evaluator/view convenience wrapper around
   `installCompiledRecoveryForRoot(...)`.
   The returned replacement view must preserve ownership of the input view's
   unchanged `snapshot_root_hash` and `semantic_root_hash` according to the
   replacement-view transfer rule above; it must not transiently drop those
   base-root pins while swapping in the compiled root.
   On a winning publish path, it must exact-pin the installed compiled root via
   `compare_and_swap_and_pin_exact(...)` before exposing the replacement view.
   The input view's unchanged base-root ownership is then transferred to that
   replacement view without a `1 -> 0` / `0 -> 1` gap.
   If the compiled install CAS loses, it must either:
   - confirm under shared `gc.lock` that the current compiled ref already
     equals the same target compiled root and pin that exact compiled root via
     `confirm_current_ref_and_pin_exact(...)`; or
   - retry / fail according to the same standalone compiled-install conflict
     rules, without satisfying success by reopening the compiled namespace and
     accepting a later different compiled root.
   It is a recovery-preflight helper, not a mid-pipeline mutation step.
9. `ensureCompiledRecovery(view) -> EvalReadView`
   If the compiled recovery root is absent or invalid for
   `view.semantic_root_hash`, build and publish a fresh local compiled recovery
   root keyed by `(stable_recovery_key, semantic_root_hash)`, then return a
   replacement view pinned to one validated compiled root current for that
   semantic recovery root at successful preflight completion.
   The returned replacement view must preserve ownership of the input view's
   unchanged `snapshot_root_hash` and `semantic_root_hash` according to the
   replacement-view transfer rule above.
   In particular:
   - if a valid compiled root is already current, the helper must pin and
     return that exact current compiled root through
     `confirm_current_ref_and_pin_exact(...)`, then transfer unchanged
     base-root ownership into the replacement view rather than reopening later;
   - if the helper races with another writer and a different validated compiled
     root becomes current for the same semantic recovery root, it may adopt and
     return that compiled root after validation instead of forcing its locally
     built root to win; whichever compiled root it returns must itself have
     been exact-pinned through `compare_and_swap_and_pin_exact(...)` or
     `confirm_current_ref_and_pin_exact(...)` before the replacement view is
     exposed.
   It is allowed only before candidate iteration begins; after it returns, the
   replacement view becomes the one reused for the rest of that recovery
   session.
10. `loadAndVerifyRuntimeRoots(view) -> RuntimeRootResult`
   Low-level bind/helper operation, not an evaluator-facing substitute for
   `bindEvalView(...)`.
   Load `runtime-root-map-v1` from the pinned snapshot root, verify each entry
   against `locked_url`, `nar_hash`, and `store_path`, and return the verified
   roots for `bindEvalView(...)`, `openBoundEvalSession(...)`, and
   maintenance/test code that needs the verified result set explicitly.
   Evaluator-facing code must not bypass `bindEvalView(...)` by merging these
   results into a live session registry on its own.
11. `exportEvalPackage(view) -> { snapshot_package, recovery_package, eval_package, semantic_archive, compiled_package?, compiled_archive? }`
12. `exportSnapshotPackage(snapshot_root_hash) -> { snapshot_package, snapshot_archive }`
    Maintenance/export helper for standalone semantic snapshot transport.
    The emitted `block-archive-v1` must advertise exactly `{ snapshot_root_hash
    }`.
13. `exportRecoveryPackage(semantic_root_hash) -> { recovery_package, recovery_archive, compiled_package?, compiled_archive? }`
    Maintenance/export helper for standalone semantic recovery transport.
    The emitted semantic `block-archive-v1` must advertise exactly
    `{ semantic_root_hash }`.
14. `lookupCurrentBinding(view, path_key) -> optional<binding-v1>`
15. `loadRuntimeRoots(view) -> runtime-root-map-v1`
16. `publishRuntimeRoot(session, source_id, locked_url, nar_hash, store_path) -> EvalSessionHandle`
   Evaluator-facing write operation on a bound session.
   It updates only the current snapshot state and the matching coherent eval
   view; it does not rewrite semantic recovery.
   It may refresh the raw snapshot maintenance head after coherent-head
   success, but it must not move any raw semantic-recovery head.
   It follows the same coherent-head publication rule as
   `installEvalPackage(...)`:
   success is defined by the exact committed `eval-view-v1` root that won or
   matched the successful coherent-head CAS and the returned replacement handle
   rebound around that exact committed view, not by raw-head movement.
   This exact-root success path must use `compare_and_swap_and_pin_exact(...)`,
   not a later namespace reopen.
   On coherent-head CAS loss, it must either:
   - observe that the committed coherent head already equals its target and
     succeed as a no-op through `confirm_current_ref_and_pin_exact(...)`; or
   - reopen the latest view, recompute, and retry internally until it either
     succeeds or fails with a non-retryable storage/corruption error.
   Because this operation leaves `semantic_root_hash` unchanged, the returned
   replacement handle must carry forward the prior handle's `compiled_root_hash`
   unchanged if one was already present and valid for that semantic root.
   It must not probe, rebuild, or replace compiled recovery state as part of
   this snapshot-only publication path.
   The operation itself must carry forward the prior handle's deduplicated
   session-local runtime-root mount overlay, add the newly mounted runtime
   root's `(CanonPath(store_path), DepSource::fromRuntimeRoot(locked_url), "")`
   entry to that overlay, and merge both the verified forward runtime-root
   entry and the updated overlay into the returned replacement handle before
   returning.
   Callers must not perform a second out-of-band registry mutation after a
   successful call.
   The operation must not expose ordinary same-namespace contention as a
   retryable result, because the current cold-eval path has already accepted
   the new locked runtime root as part of same-session provenance.
   If the operation fails before committing a replacement handle, the caller's
   prior `EvalSessionHandle` remains authoritative and the cold-eval step must
   treat that failure as unrecoverable.
   The returned value is the replacement bound session handle for subsequent
   same-session reads and evaluator operations.
17. `loadTraceRecord(trace_record_hash) -> trace-v1`
18. `loadResult(result_hash) -> result-v1`
19. `loadDepKeySet(dep_key_set_hash) -> dep-key-set-v1`
20. `loadCandidateSet(candidate_set_root_hash) -> candidate-set-v1`
21. `publishRecord(session, ...) -> EvalSessionHandle`
   Evaluator-facing write operation on a bound session.
   This publishes the replacement current binding state and returns the
   committed replacement bound session handle for subsequent same-session reads
   and evaluator operations.
   It follows the same coherent-head publication rule:
   success is defined by the exact committed `eval-view-v1` root that won or
   matched the final coherent-head CAS and committed-handle rebind against
   that exact view, while raw snapshot and raw semantic-recovery refs may be
   refreshed afterward only as maintenance.
   This exact-root success path must use `compare_and_swap_and_pin_exact(...)`,
   not a later namespace reopen.
   On coherent-head CAS loss, it must either:
   - observe that the committed coherent head already equals its target and
     succeed as a no-op through `confirm_current_ref_and_pin_exact(...)`; or
   - reopen the latest view, recompute, and retry / fail with retryable
     contention.
   The returned replacement handle must carry forward the prior handle's
   session-local runtime-root mount overlay unchanged.
   If the returned `semantic_root_hash` equals the input handle's prior
   `semantic_root_hash`, the returned replacement handle may carry forward the
   same valid `compiled_root_hash`.
   If the returned `semantic_root_hash` differs, the returned replacement
   handle must set `compiled_root_hash` absent.
   Evaluator-facing `publishRecord(...)` does not itself publish, probe, or
   attach a new compiled recovery root for the changed semantic root on the
   success path.
22. `publishStatHash(...) -> new_stat_hash_root`
23. `publishTreeHash(...) -> new_tree_hash_root`

The existing recovery logic in `trace-store-verify.cc` can be ported above this
new interface, but the row-ID API itself is not reusable as-is.[^live-recovery]

## In-Tree Reuse

This rewrite does not start from zero.
Several in-tree mechanisms are directly reusable.

### Reuse As-Is

1. `PathLocks` and `FdLock`
   - for cross-process lock files and sorted lock acquisition.[^in-tree-locks]
2. `writeFile(..., FsSync::Yes)`, `syncParent`, and `replaceSymlink`
   - for durable temp-write + rename publication.[^in-tree-publish]
3. temp-root / GC-lock patterns in `gc.cc`
   - as the model for process pin files and stale-pin cleanup.[^in-tree-gc]
4. file/tree hashing helpers in `file-content-address.cc`
   - as the basis for canonical hashing utilities.[^in-tree-hash]
5. `boost::iostreams::mapped_file_source`
   - for mmap-backed immutable block reads on the hot path.[^in-tree-mmap]
6. `SharedSync` and `boost::unordered_flat_*`
   - for in-process hot caches and concurrent read-mostly state.[^in-tree-sync]

### Reuse With Adaptation

1. recovery logic in `trace-store-verify.cc`
   - the algorithms are reusable; the storage API is not.[^live-recovery]
2. `AttrVocabStore`
   - path construction logic is conceptually useful, but dense mutable IDs and
     host-endian hashing are not.[^live-vocab]
3. `StatHashStore` (removed as dead code)
   - historical note: fingerprint-keyed memoization was conceptually useful,
     but the singleton and SQLite persistence were not.[^live-stat-hash]
   - the whole store was removed from the tree (zero production read-path
     hits); this bullet survives only as an archaeological pointer.
4. `archive.cc`, `dep-hash-fns.cc`, and `nar-accessor.cc`
   - reusable for `tree-hash-cache-v1`; the new cache composes from NAR/tree
     semantics already implemented in-tree.[^in-tree-nar][^live-nar-identity]
5. `git-utils.cc` pack writing
   - useful later if block compaction moves from one-file-per-block to a packed
     block format, but libgit2 packfiles are not the primary eval-trace block
     store.[^in-tree-pack]

### Do Not Reuse

1. the attached-SQLite store topology as the semantic source of truth;
2. the row-ID async service boundary;
3. persistent mutable global attr or stat-hash singletons;[^live-threading]
   [^live-vocab][^live-stat-hash]
4. a memory-first semantic authority that accepts writes in RAM and only
   checkpoint-flushes semantic roots, refs, or GC metadata later.[^live-periodic-flush]

## Rewrite Strategy

This remains a rewrite with no compatibility layer.

The plan explicitly excludes:

- backward compatibility shims
- SQLite fallback reads for semantic snapshot/recovery/eval state
- dual-write phases
- runtime row-bridge adapters that preserve persistent `TraceId` / `ResultId` /
  `DepKeySetId` semantics across the new storage boundary
- long-lived coexistence with the legacy stores

What is still allowed is one evaluator-facing backend adapter at the existing
`TraceBackend` seam, so long as it translates to the new hash/root-based engine
boundary rather than re-exposing legacy row-ID semantics.

### Step 1: Canonical Encoders

Implement canonical encoders and decoders for:

- `attr-name-key-v1`
- `path-key-v1`
- `dir-set-v1`
- `runtime-root-map-v1`
- `eval-view-v1`
- `eval-view-meta-v1`
- `snapshot-v1`
- `snapshot-pack-v1`
- `snapshot-meta-v1`
- `recovery-meta-v1`
- `compiled-recovery-meta-v1`
- `result-v1`
- `dep-key-set-v1`
- `trace-v1`
- `candidate-summary-v1`
- `candidate-set-v1`
- `binding-v1`
- `history-member-v1`
- `candidate-object-ref-v1`
- `semantic-recovery-v1`
- `recovery-pack-v1`
- `eval-package-v1`
- `compiled-recovery-v1`
- `compiled-recovery-package-v1`
- `struct-profile-summary-v1`
- `stat-hash-key-v1`
- `tree-hash-key-v1`
- `root-ref-v1`
- `prolly-map-v1`

### Step 2: Block Store

Implement:

- content-addressed block paths
- writer-local staging directories
- per-writer `writer.lock` liveness files
- per-transaction `tx.lock` liveness files
- `put/get`
- verified-block read cache
- mmap-backed read path
- temp-write + `fsync` + rename
- staged-block install during publish
- opportunistic stale-transaction cleanup
- root-based GC reachability

### Step 3: Root Refs

Implement:

- root-ref file encoding
- eval-view namespace keys derived from
  `(semantic_session_key, stable_recovery_key)`
- compiled recovery namespace keys derived from
  `(stable_recovery_key, semantic_root_hash)`
- ref-payload namespace-key validation on read
- ref-payload namespace-key validation on CAS update
- per-ref lock files
- CAS publish under shared `gc.lock`
- read views that pin roots

### Step 4: Process Pin Registry

Implement:

- one `pins/<pid>.lock` liveness file plus one `pins/<pid>.pin` payload file
  per process
- local refcounts
- shared-`gc.lock` synchronization on `0 -> 1` and `1 -> 0` transitions
- shared-`gc.lock` synchronization for root publication itself
- temp-write + rename pin payload updates
- global `gc.lock`

### Step 5: Snapshot Namespace

Replace:

- `Sessions`
- `SessionRuntimeRoots`

with:

- `snapshot-v1`
- `snapshot-pack-v1`
- `runtime-root-map-v1`
- `eval-view-v1`

Delete the semantic snapshot/current-state SQLite path rather than bridging it.
Runtime-root map load/update operations are part of this replacement, not a
later add-on.

### Step 6: Recovery Namespace

Replace:

- `History`
- its implicit dependence on `Traces`, `DepKeySets`, and `Results`

with:

- `recovery-pack-v1`
- `history-member-v1`
- `candidate-object-ref-v1`
- one semantic recovery root
- one optional compiled recovery root
- `semantic-recovery-v1`
- `compiled-recovery-v1`
- `recovery-meta-v1`
- `compiled-recovery-meta-v1`
- `eval-package-v1`
- optional `compiled-recovery-package-v1` export companion
- canonical candidate sets
- `candidate-summary-v1`
- `direct-index-v1`
- `struct-profile-index-v1`
- deterministic secondary indexes
- explicit compiled-package install path

Delete the SQLite history path rather than bridging it.

### Step 7: Attr Vocabulary

Replace:

- `attr-vocab.sqlite`
- discovery-order `AttrNameId`
- discovery-order `AttrPathId`

with:

- `attr-name-vocab-v1`
- `attr-path-vocab-v1`
- stable digests
- root-local deterministic ordinal derivation where dense indices are still
  useful

### Step 8: Stat Hash

Historical framing: this step originally called for replacing the
`StatHashStore` singleton and its SQLite backing store.  `StatHashStore`
was removed from the tree as dead code before the rewrite began, so the
step reduces to an additive one in practice.

Add:

- `stat-hash-v1`
- `tree-hash-cache-v1`
- revalidated fingerprint-keyed raw-byte entries
- validated NAR-form leaf digest entries
- compositional subtree digest entries

### Step 9: Service Rewrite

Delete the row-ID service boundary and replace it with root views and content
hash references.
This is not a later cleanup item.
It is part of the storage rewrite itself.

Concretely, this step means:

- stop treating `VerificationOrchestrator`'s current
  `TraceId` / `ResultId` / `DepKeySetId`-oriented API as the stable service
  boundary (`TraceStoreService` was previously listed here; it has since been
  removed from the tree);
- introduce the new engine-facing service API in terms of `EvalReadView`,
  `EvalSessionHandle`, and hash-derived refs;
- keep any temporary compatibility shims backend-private rather than exposing a
  second public row-ID service boundary to the rest of the evaluator.

### Step 10: Backend Adapter Migration

Rewrite `StoreTraceBackend` and `TraceSession` around the new service boundary
before attempting broader evaluator-control-flow surgery.

Concretely:

- `TraceSession` should continue to own one `TraceBackend`;
- `StoreTraceBackend` should become the sole adapter that owns the current
  bound `EvalSessionHandle` and translates evaluator operations into the new
  engine API;
- session-open logic must collapse from:
  - load runtime roots
  - merge registry entries
  - bind session
  into one adapter-owned setup path based on
  `openBoundEvalSession(...)` / `bindEvalView(...)`;
- `record(...)` and `recordRuntimeRoot(...)` must replace the backend's stored
  session handle on success;
- any replay/memo identity still needed by evaluator code must move to
  hash-derived refs or backend-private ephemeral tokens, not persistent row
  IDs.
- this step is not complete while evaluator-facing headers or translation units
  still cache or exchange `TraceId` as the replay identity:
  `TraceBackend::loadFullTrace(traceId)`,
  `TraceStore::VerifyResult { ..., traceId }`,
  `TraceStore::RecordResult { traceId }`,
  `TracedExpr::LazyState::traceId`,
  and `TracedExpr::replayTrace(..., TraceId)` all have to disappear as part of
  the adapter cutover rather than being left for a later cleanup pass.

This step is intentionally separate from storage-mechanics bring-up because it
is the point where the new engine becomes reachable from the live evaluator.
Landing it explicitly keeps the rewrite sequence honest:
the file-based engine is not "integrated" until the backend seam itself has
been rewritten.

### Step 11: Evaluator Replay Identity Rewrite

The backend seam rewrite is still not executable until the evaluator's replay
and warm-hit plumbing stops naming traces by persistent row ID.

Three shapes were considered:

1. keep `TraceId` inside `TracedExpr`, `materialize.cc`, and backend replay
   calls, and translate it internally inside the new backend adapter;
2. delete the evaluator-visible `TraceId` flow and replace it with a
   hash-derived replay identity `TraceRecordRef { trace_record_hash }`;
3. delete replay identity caching entirely and recompute warm-hit replay inputs
   from the current bound session handle on demand.

V1 chooses the second shape.

That choice is deliberate:

- the first shape would preserve the row-ID contract in evaluator state even
  after the note says the row-ID service boundary is gone;
- the third shape would change current replay/materialization behavior and
  turn a storage rewrite into a larger evaluator algorithm rewrite than v1
  needs;
- `trace_record_hash` already names the semantic trace object the evaluator
  needs to replay, while any decoded dep-vector cache can stay backend-private.

Consequences:

- `TracedExpr::LazyState` must stop storing `TraceId`; if replay identity is
  cached there at all, it must be `TraceRecordRef { trace_record_hash }`;
- evaluator-facing warm-hit results must no longer return
  `VerifyResult { ..., traceId }` / `RecordResult { traceId }` across the
  backend seam;
- `TraceBackend::loadFullTrace(traceId)` must disappear from the evaluator-
  facing abstraction;
- `TracedExpr::replayTrace(...)` and the materialization path must replay deps
  through one hash-rooted trace reference path instead of a row-ID lookup;
- if the adapter wants a decoded dep-vector cache for performance, that cache
  stays fully backend-private and is keyed by `TraceRecordRef` or another
  backend-private ephemeral token rather than by a persistent row ID;
- backend-private ephemeral replay tokens are allowed only inside
  `StoreTraceBackend` and its private helper state; they must not become part
  of the evaluator-facing `TracedExpr` / `TraceSession` contract.
- this step also needs one explicit evaluator-facing result-contract choice,
  because the live evaluator/cache seam still names store-specific nested
  result types:
  - `TraceBackend::verify(...) -> optional<TraceStore::VerifyResult>`;
  - `TraceBackend::record(...) -> optional<TraceStore::RecordResult>`;
  - `PrefetchPool::PrefetchToken.result : optional<TraceStore::VerifyResult>`;
  - `TracedExpr::replayTrace(..., TraceId)` plus
    `TraceBackend::loadFullTrace(traceId)`.

Three shapes were considered:

1. keep those evaluator/cache types and only swap the storage engine under
   them;
2. delete all structured replay/verify hit carriers and make evaluator code
   pull everything ad hoc from the backend/session handle;
3. keep structured evaluator/cache hit carriers, but rewrite them into one
   backend-owned semantic vocabulary:
   - `VerifyHit { CachedResult value, TraceRecordRef trace_record_ref }`;
   - `RecordHit { TraceRecordRef trace_record_ref }` or an equivalent
     hash-derived replay identity when record returns a hit/result at all;
   - `TraceBackend::loadTraceRecord(...)` / `loadReplayDeps(...)` addressed by
     `TraceRecordRef`, not `TraceId`;
   - `PrefetchPool` stores the same hash-derived `VerifyHit` carrier rather than
     `TraceStore::VerifyResult`.

V1 chooses the third shape.

That choice is deliberate:

- the first shape would keep `trace-store.hh` nested row-ID types as the
  evaluator/cache contract even after the note says the backend seam is
  rewritten;
- the second shape would force a larger evaluator/control-flow rewrite than is
  necessary for this storage migration;
- the evaluator still benefits from one small structured replay/verify hit
  carrier, but that carrier has to speak semantic trace identity rather than
  store row identity.

This step is intentionally separate from Step 10 because the live code stores
`TraceId` in `TracedExpr` itself and reuses it later during replay.
Leaving that rewrite implicit would permit an impossible intermediate state:
the note would claim the row-ID service seam is gone while evaluator replay
still depends on it.

### Step 12: Cutover Bundle Sequencing

The remaining rewrite steps are too coupled to pretend that every intermediate
file state is a stable public design point.

Three rollout shapes were considered:

1. require every per-file edit to remain buildable through compatibility shims
   and bridge adapters;
2. do one monolithic all-at-once rewrite with no internal sequencing at all;
3. define a small number of explicit cutover bundles that may rewrite several
   coupled files together, while still preserving a clear branch-level order.

V1 chooses the third shape.

That choice is deliberate:

- the first shape would reintroduce exactly the bridge layer this note already
  rejects;
- the second shape is acceptable in principle, but it hides real dependencies
  between evaluator-side replay state, backend method signatures, and the
  async row-ID helper layer;
- the live code shows those dependencies are real:
  `trace-backend.hh`, `traced-expr.hh`, `trace-session.cc`,
  `materialize.cc`, `context.cc`, `prefetch-pool.hh`,
  `trace-store-service.hh/.cc`, and
  `verification-orchestrator.hh/.cc` all currently exchange row-ID-shaped
  result types or session-setup responsibilities.[^live-backend-interface]

Consequences:

- Steps 10 and 11 must land as one evaluator-seam cutover bundle, not as
  isolated header cleanups;
- that bundle rewrites together at minimum:
  - `src/libexpr/include/nix/expr/eval-trace/cache/trace-backend.hh`
  - `src/libexpr/include/nix/expr/eval-trace/cache/trace-session.hh`
  - `src/libexpr/eval-trace/cache/traced-expr.hh`
  - `src/libexpr/eval-trace/cache/trace-session.cc`
  - `src/libexpr/eval-trace/cache/materialize.cc`
  - `src/libexpr/eval-trace/context.cc`
  - any `TraceRuntime` entry points that still mutate session registry state or
    backend-owned runtime-root state out-of-band.  Historically these
    were `rememberSession(...)`, `addRegistryMountPoint(...)`,
    `recordRuntimeRoot(...)`, and related helpers;
    `addRegistryMountPoint` has been removed, and runtime-root publication
    now routes through `TraceSession::registerRuntimeRootMount` +
    `TraceBackend::recordRuntimeRoot`;
- the reusable verification/recovery logic then has to move off the old
  row-ID store API as a second bundle.  (Updated 2026-04-29:
  `trace-store-protocol.hh` and `verify-pipeline.hh` have already been
  removed; the list below keeps them crossed out so the archaeological
  record stays intact.)
  - `src/libexpr/include/nix/expr/eval-trace/store/trace-store.hh`
  - `src/libexpr/eval-trace/store/trace-store.cc`
  - `src/libexpr/eval-trace/store/trace-store-lifecycle.cc`
  - `src/libexpr/eval-trace/store/trace-store-verify.cc`
  - ~~`src/libexpr/include/nix/expr/eval-trace/store/trace-store-protocol.hh`~~ (gone)
  - `src/libexpr/include/nix/expr/eval-trace/store/verification-session.hh`
  - `src/libexpr/include/nix/expr/eval-trace/store/verification-protocol.hh`
  - ~~`src/libexpr/eval-trace/store/verify-pipeline.hh`~~ (gone)
  - any verification-session or protocol types that still model
    `CurrentNodeRef`, `TraceHistoryEntry`, `TraceId`, `ResultId`, or
    `DepKeySetId` as the engine-facing algorithm interface;
- only after that port is in place does the row-ID async helper layer
  disappear as a third bundle.  (Updated 2026-04-29: the
  `trace-store-service.{hh,cc}` pair has already been removed.)
  - `src/libexpr/eval-trace/cache/prefetch-pool.hh`
  - ~~`src/libexpr/eval-trace/store/trace-store-service.hh`~~ (gone)
  - ~~`src/libexpr/eval-trace/store/trace-store-service.cc`~~ (gone)
  - `src/libexpr/eval-trace/store/verification-orchestrator.hh`
  - `src/libexpr/eval-trace/store/verification-orchestrator.cc`
  - any row-ID protocol/helper headers that exist only to support that layer;
- v1 does not require those intermediate local commits to preserve a public
  compatibility surface or a stable bridge protocol;
- what must remain stable is the branch-level order:
  the evaluator-side replay identity rewrite cannot be deferred until after
  the public async row-ID layer is supposedly deleted;
  the verification/recovery algorithms cannot remain attached to
  `trace-store.hh`'s row-ID types once the evaluator/backend seam no longer
  names them; and the row-ID helper layer cannot survive after both earlier
  bundles have landed.
- inside bundle 2 itself, the order is also constrained:
  - first replace the carrier/result types used by verification and recovery
    (`CurrentNodeRef`, `TraceHistoryEntry`, warm-hit results, and publish
    results);
  - then replace `verification-session.hh` state that still keys memoization or
    already-verified sets by `TraceId` / `NodeStamp`;
  - only then port `trace-store-verify.cc` to the new hash/root-based
    algorithm interface.  (`verify-pipeline.hh` was previously listed
    alongside here; it has since been removed.)
- bundle 2 also needs one explicit carrier replacement choice, because the
  live algorithm does not consume bare semantic roots directly; it consumes
  row-shaped helper records:
  - `CurrentNodeRef { traceId, resultId, nodeStamp }`; and
  - `TraceHistoryEntry { depKeySetId, structHash, traceId, resultId, traceHash, payload }`.

V1 chooses the following replacements:

- current-binding lookup uses `binding-v1` from the pinned snapshot root as the
  current-state carrier;
- warm-hit replay identity and result decoding then follow
  `binding-v1.candidate_id -> trace_record_hash`,
  `binding-v1.result_hash -> result-v1`, and
  `binding-v1.dep_key_set_hash -> dep-key-set-v1`;
- recovery history scanning does not return ad hoc row tuples;
  it uses deterministic acceleration objects derived from semantic history:
  `candidate-set-v1` / inlined `candidate-summary-v1` for bootstrap,
  direct-hash, and GitIdentity candidate iteration;
  structural recovery uses `struct-profile-summary-v1` to derive the candidate
  `trace_hash`, then re-enters the same `direct-index-v1 -> candidate-set-v1`
  carrier path for actual candidate iteration.
- mutable current-node helpers are replaced explicitly:
  - `lookupCurrentNode(pathId)` becomes
    `lookupCurrentBinding(view, path_key) -> optional<binding-v1>`;
  - `getCurrentTraceHash(pathId)` becomes a derivation from that current
    binding, either directly from `binding-v1.trace_hash` or by loading the
    referenced `trace-v1`;
  - bootstrap-on-miss no longer falls back to "first history row for path";
    it starts from `bootstrap-index-v1[path_digest] -> candidate_set_root_hash`.
- bundle 2 also needs one explicit algorithm-entry replacement choice, because
  the live verification core is still centered on row IDs even after carrier
  decoding:
  - `verify(pathId, ...)` bootstraps through `lookupCurrentNode(pathId)` and
    then delegates to one trace-row identity;
  - `verifyTrace(traceId, ...)` is the candidate-local verification routine;
  - `recovery(oldTraceId, pathId, ...)` derives recovery candidates from one
    historical trace row.

Three shapes were considered:

1. keep those entry points and translate the new root/hash inputs back into
   backend-private row IDs before entering the algorithm;
2. collapse everything into one monolithic `verifyPath(view, path_key, ...)`
   routine and make candidate-local verification an unnamed internal detail;
3. make the top-level entry root/path-based and the candidate-local entry
   trace-record-based:
   - `verifyCurrentPath(view, path_key, ...)`;
   - `verifyCandidate(trace_record_ref, ...)`; and
   - `recoverPath(view, path_key, old_candidate?, ...)`.

V1 chooses the third shape.

That choice is deliberate:

- top-level verification still has to start from the pinned snapshot view and
  canonical path key so that current-binding hits, snapshot misses, and
  bootstrap-on-miss all share one coherent entry;
- candidate-local verification remains a reusable semantic operation needed by
  current-binding verification, recovery candidate iteration, and future
  candidate-addressed proof/export work;
- preserving `verifyTrace(traceId, ...)` as a hidden bridge would leave the
  algorithm rooted in row IDs even after the carrier and memo rewrites.
- bundle 2 also needs one explicit protocol replacement choice, because the
  live typed verification channel still transports one row-ID candidate
  identity:
  - `VerifyPipeline = Send<TraceId, ...>`.

Three shapes were considered:

1. keep the session-typed pipeline but continue to send `TraceId`;
2. delete the typed pipeline entirely and inline all pass ordering in one
   coroutine/API surface;
3. keep the optional typed pipeline shape, but make it candidate-local and
   semantic:
   - if the pipeline remains in v1, it becomes
     `VerifyCandidatePipeline = Send<TraceRecordRef, ...>`;
   - top-level current-path lookup and recovery-path bootstrap stay outside that
     candidate-local channel and start from the pinned view/path entry points.

V1 chooses the third shape.

That choice is deliberate:

- keeping the typed phase ordering is still defensible if the project wants the
  compile-time pass-order invariant;
- but that invariant does not justify preserving `TraceId` as the wire
  identity;
- deleting the pipeline outright is allowed as an implementation choice, but
  the spec must not require a typed protocol whose first message is a row ID.
- bundle 2 also needs one explicit helper-service/protocol replacement choice,
  because the live async helper surface is still row-ID-shaped even outside the
  candidate-local pipeline:
  - `lookupCurrentNode(pathId) -> optional<CurrentNodeRef>`;
  - `loadFullTrace(TraceId) -> vector<Dep>`;
  - `scanHistory(pathId) -> vector<TraceHistoryEntry>`;
  - `getCurrentTraceHash(pathId)`;
  - `publishRecord(RecordRequest) -> CurrentNodeRef`;
  - `publishRecovery(pathId, traceId, resultId) -> CurrentNodeRef`; and
  - `decodeCachedResult(ResultId)`.

Three shapes were considered:

1. keep that helper surface and translate root/hash-based engine state back
   into backend-private row IDs until bundle 3 deletes it;
2. delete the helper surface immediately and inline every store-facing call path
   into the rewritten backend/orchestrator seam as part of bundle 2;
3. allow the helper surface to survive temporarily, but rewrite it now so that
   any surviving adapter-private async helper surface is already
   root/hash-based (`TraceStoreService` / `trace-store-protocol.hh` were
   previously listed here; both have since been removed from the tree):
   - current-state lookup becomes
     `lookupCurrentBinding(view, path_key) -> optional<binding-v1>`;
   - candidate/history lookup becomes semantic-index-based
     `lookupBootstrapCandidates(path_key)`, `lookupDirectCandidates(trace_hash)`,
     `lookupGitIdentityCandidates(git_key)`, or equivalent
     `candidate-set-v1` / `candidate-summary-v1` accessors;
   - trace/result/dep-key-set loading becomes
     `loadTrace(trace_record_ref)`, `loadResult(result_ref)`, and
     `loadDepKeySet(dep_key_set_ref)`;
   - any surviving publish helper returns replacement `EvalReadView` /
     `EvalSessionHandle` or hash-derived refs, never `CurrentNodeRef`.

V1 chooses the third shape.

That choice is deliberate:

- bundle 3 is already the helper-layer deletion bundle, so bundle 2 needs a
  coherent rule for the interim state rather than silently assuming those files
  disappear first;
- deleting the helper layer immediately is acceptable as an implementation
  shortcut, but the spec must not require row-ID-shaped helpers to survive until
  that deletion lands;
- keeping the helper layer temporarily is compatible with the existing rollout
  order only if it already speaks the same root/hash vocabulary as the rest of
  the rewritten algorithm.
- bundle 2 also needs one explicit result-contract replacement choice, because
  live store-facing helper state still packages semantic outcomes in row-shaped
  structs:
  - `TraceStore::VerifyResult { CachedResult value, TraceId traceId }`;
  - `TraceStore::RecordResult { TraceId traceId }`.

Three shapes were considered:

1. keep those structs and translate `TraceRecordRef` back into backend-private
   row IDs when talking to the surviving helper layer;
2. delete the structs entirely and force every layer to traffic only in
   `EvalSessionHandle` plus ad hoc object loads;
3. allow value/result structs to survive only if their semantic identity fields
   are hash-derived:
   - any surviving verify result becomes
     `{ CachedResult value, TraceRecordRef trace_record_ref }` or an equivalent
     hash-derived replay/result carrier;
   - `publishRecord(...)` remains a session-handle update and therefore must not
     expose `RecordResult { traceId }` across any surviving helper/service seam;
     if the engine wants an internal publication summary, it stays adapter- or
     engine-private and is keyed by `TraceRecordRef`, `ResultRef`, and
     `DepKeySetRef`, not by row IDs.

V1 chooses the third shape.

That choice is deliberate:

- the note already chose `publishRecord(session, ...) -> EvalSessionHandle` as
  the evaluator-facing write contract, so keeping `RecordResult { traceId }`
  alive as a public/helper result would reopen the row-ID seam from underneath;
- verify/replay still legitimately need a value-bearing hit result, but the
  identity in that result is the semantic trace object, not a SQLite row;
- forcing all value-bearing helper results to disappear entirely would create
  a larger rewrite than necessary and is not required for correctness.
- bundle 2 also needs one explicit core-engine state replacement choice,
  because the live `trace-store.hh` implementation still keeps long-lived
  semantic caches and allocation state keyed by persistent row IDs:
  - `traceByTraceHash : TraceHash -> TraceId`;
  - `resultByHash : ResultHash -> ResultId`;
  - `depKeySetByStructHash : StructHash -> DepKeySetId`;
  - `traceHeaderCache[TraceId]`, `traceFullCache[TraceId]`,
    `resultPayloadCache[ResultId]`, `depKeySetCache[DepKeySetId]`,
    `deferredTraceBlobs[TraceId]`;
  - `pendingResults { ResultId ... }`, `pendingDepKeySets { DepKeySetId ... }`,
    `pendingTraces { TraceId ... }`; and
  - `nextResultId`, `nextDepKeySetId`, `nextTraceId`, `nextNodeStamp`.

Three shapes were considered:

1. allow those long-lived integer IDs to remain engine-private forever, as long
   as no public seam mentions them;
2. allow only a temporary hybrid where bundle 2 rewrites the public/helper
   seams first and bundle 3 or later deletes the last internal row-ID core;
3. require bundle 2's algorithm-port to rewrite the core engine state too, so
   that any remaining engine-private caches are keyed by semantic hashes/roots
   rather than persistent row IDs:
   - trace/object caches keyed by `TraceRecordRef`, `ResultRef`,
     `DepKeySetRef`, `TraceHash`, and pinned root hashes;
   - pending-write/staging state keyed by content hashes and staged block
     payloads, not numeric IDs;
   - no monotonic semantic entity allocators analogous to
     `nextTraceId` / `nextResultId` / `nextDepKeySetId`.

V1 chooses the third shape.

That choice is deliberate:

- the note is already replacing the persistence engine, not just the public
  adapter vocabulary; preserving a row-ID core under a hash/root shell would
  still leave the semantic engine organized around the old model;
- long-lived "engine-private" row IDs would keep infecting cache keys, pending
  write queues, and verification helpers even after the public seams are
  rewritten;
- backend-private ephemeral tokens remain allowed only for bounded replay or
  memo purposes above the engine core, but they are not a license to keep
  `trace-store.hh` itself allocating persistent semantic integer IDs.

Consequences:

- there is no bundle-2 design point where a new root/hash-based engine is
  paired with a legacy `CurrentNodeRef` or `TraceHistoryEntry` carrier type;
- any helper that still wants a "current binding" carrier must derive it from
  `binding-v1`, not from a mutable current-node row cache;
- any helper that still wants a "history scan" carrier must derive it from
  canonical candidate-set/profile acceleration state that is validated against
  semantic history membership, not from an engine-private row tuple.
- there is likewise no bundle-2 design point where verification or
  trace-context resolution still begins from `lookupCurrentNode()` against a
  mutable current-state cache rather than from the pinned snapshot view.
- there is likewise no bundle-2 design point where the central verification
  algorithm still begins from helpers equivalent to `verify(pathId, ...)`,
  `verifyTrace(traceId, ...)`, or `recovery(oldTraceId, pathId, ...)`;
  the top-level algorithm entry must be rooted in
  `verifyCurrentPath(view, path_key, ...)`, and the candidate-local entry must
  be rooted in `TraceRecordRef`.
- there is likewise no bundle-2 design point where `verification-protocol.hh`
  still defines candidate-local protocol messages in terms of `TraceId`;
  if a typed pipeline remains, its candidate identity must be
  `TraceRecordRef`.  (`verify-pipeline.hh` was previously listed alongside
  here; it has since been removed.)
- there is likewise no bundle-2 design point where adapter-private async
  helpers still expose row-shaped requests or responses such as
  `CurrentNodeRef`, `TraceHistoryEntry`, `TraceId`, `ResultId`, or
  `DepKeySetId`; if those helpers survive until bundle 3, they must
  already be rewritten in terms of `binding-v1`, semantic candidate
  indexes, `TraceRecordRef`, `ResultRef`, `DepKeySetRef`, and replacement
  view/session-handle results.  (`trace-store-protocol.hh` and
  `TraceStoreService` were previously listed alongside here; both have
  been removed.)
- there is likewise no bundle-2 design point where surviving helper/result
  carriers still expose `TraceStore::VerifyResult { ..., traceId }` or
  `TraceStore::RecordResult { traceId }`;
  any surviving verify-hit carrier must use `TraceRecordRef`, and
  `publishRecord(...)` must not reintroduce a row-ID result carrier below the
  rewritten evaluator/backend seam.
- there is likewise no bundle-2 design point where the rewritten engine core
  still keeps long-lived semantic object caches, pending-write queues, or
  allocation counters keyed by `TraceId`, `ResultId`, or `DepKeySetId`;
  if an integer token survives anywhere, it must be a bounded backend-private
  ephemeral token above the engine core rather than the identity of a semantic
  object or pending semantic write.
- bundle 2 also needs one explicit verification-session state replacement
  choice, because the live algorithm still keeps two row-shaped fast-path
  caches:
  - `verifiedTraceIds : set<TraceId>`; and
  - `traceContextMemo[pathId] -> { nodeStamp, traceHash? }`.

V1 chooses the following replacements:

- `verifiedTraceIds` becomes a set keyed by semantic trace object identity,
  i.e. `trace_record_hash` / `TraceRecordRef`;
- `traceContextMemo` becomes a memo keyed by the pinned snapshot generation and
  canonical path identity, i.e.
  `(snapshot_root_hash, path-key-v1) -> optional<trace_hash>`;
- because a pinned snapshot root is immutable, the memo no longer needs to
  carry a second mutable generation token such as `NodeStamp`.

Consequences:

- `verification-session.hh` must not retain a second mutable-generation cache
  once bundle 2 lands;
- candidate-local `verifyCandidate(...)` short-circuit state becomes
  semantic-object-local rather than row-local;
- `resolveTraceContextHash(...)` memoization must derive the parent current
  binding through the pinned snapshot root and the canonical path key, not
  through `lookupCurrentNode()` plus `NodeStamp` comparison.
- each bundle also has an exit criterion:
  - bundle 1 is not done while `TraceRuntime` still performs a second
    evaluator-visible registry/backend mutation path outside the returned
    `EvalSessionHandle`, or while `releaseBackend()` no longer preserves the
    null-backend/evaluate-direct safety rule for stale traced thunks;
  - bundle 2 is not done while `trace-store-verify.cc` or the store-facing
    verification/recovery protocols still require row-ID engine identities;
  - bundle 3 is not done while evaluator-visible prefetch/verify helper state
    still stores row-ID-shaped results or depends on the public row-ID async
    service layer.
- each bundle also needs a concrete test gate before the next one starts:
  - bundle 1 must keep the evaluator-facing fixture path green, especially the
    `TraceCacheFixture` / replay / materialization / teardown-heavy tests
    under `src/libexpr-tests/eval-trace/dep/`,
    `src/libexpr-tests/eval-trace/traced-data/materialization/`, and the
    `TraceCacheFixture` release path in `helpers.hh`;
  - bundle 2 must keep the semantic verification/recovery suite green under
    `src/libexpr-tests/eval-trace/store/` and
    `src/libexpr-tests/eval-trace/verify/`;
  - bundle 3 must either preserve or deliberately replace any remaining
    prefetch-/orchestrator-level coverage before deleting the public helper
    layer; deleting that layer without replacing its coverage is not an
    acceptable "cleanup" shortcut.
- bundle 2 also needs one explicit test-helper replacement choice, because the
  live test-only seam in `src/libexpr-tests/eval-trace/helpers.hh` still
  exposes private store entry points in row-ID form:
  - `TraceStoreTestAccess::verifyTrace(store, TraceId, ...)`;
  - `TraceStoreTestAccess::recovery(store, TraceId, AttrPathId, ...)`; and
  - `TraceStoreTestAccess::verify(...) -> optional<TraceStore::VerifyResult>`.
- the same file also contains fixture convenience wrappers that preserve the
  same seam indirectly:
  - `TraceCacheFixture::getStoredDeps(attrPath)` still does
    `TraceStoreTestAccess::verify(...)` and then `loadFullTrace(traceId)`;
  - `TraceCacheFixture::getStoredResult(attrPath)` still returns the row-shaped
    `verify(...)` hit carrier as its source of truth.

Three shapes were considered:

1. allow test-only helpers to keep row-ID entry points longer than production
   code, treating them as harmless fixture convenience;
2. delete those helpers immediately and force all tests through the full
   evaluator/backend path only;
3. keep test-only helpers only if they are rewritten on the same semantic
   boundary as production code:
   - candidate-local helpers become `TraceRecordRef`-addressed;
   - current-path helpers become pinned-view/path-key or bound-session based;
   - any surviving test result carriers follow the same hash-derived
     `VerifyResult` / publish-result rules as production helpers;
   - fixture convenience wrappers like `TraceCacheFixture::getStoredDeps(...)`
     and `getStoredResult(...)` must replay through the same semantic
     `TraceRecordRef` / bound-session path instead of routing through
     `VerifyResult { ..., traceId }` and `loadFullTrace(traceId)`.

V1 chooses the third shape.

That choice is deliberate:

- letting test-only helpers lag behind would create a hidden compatibility
  surface that can keep the old algorithm entry points alive even after the
  production seam is supposedly rewritten;
- forcing every test through only the full evaluator path is stricter than
  necessary and would remove useful store-level coverage;
- the tests are supposed to validate the rewritten engine, not preserve a second
  legacy API contract.
- bundle 2 also needs one explicit test-assertion replacement choice, because
  the live verification/store suites in
  `src/libexpr-tests/eval-trace/store/` and
  `src/libexpr-tests/eval-trace/verify/` still assert directly on row-shaped
  identities and memo state:
  - `result.traceId` / `childResult.traceId`;
  - `TraceStore::RecordResult { traceId }`; and
  - `session.verifiedTraceIds` keyed by `TraceId`.

Three shapes were considered:

1. allow those assertions to remain until the last store-facing code is
   deleted, treating them as harmless test internals;
2. delete the affected tests and rely only on higher-level evaluator coverage;
3. keep the coverage, but rewrite the assertions on the same semantic boundary
   as production:
   - replay identity assertions become `TraceRecordRef` /
     `trace_record_hash`-based;
   - write-result assertions stop expecting `RecordResult { traceId }`;
   - verification-session memo assertions follow the rewritten
     `verifiedTraceIds` replacement keyed by semantic trace identity instead of
     `TraceId`.

V1 chooses the third shape.

That choice is deliberate:

- the tests should prove the new semantic identity model actually landed, not
  hide a second row-ID contract behind fixture code;
- deleting the coverage would weaken exactly the store/recovery edge cases the
  rewrite still needs to preserve;
- a bundle cannot honestly claim to have ported verification/session state if
  the assertions still require `TraceId` and `RecordResult { traceId }`.
- bundle 2 also needs one explicit protocol-coverage replacement choice,
  because the live protocol tests in
  `src/libexpr-tests/eval-trace/store/verification-protocol.cc` still validate
  the session-typed verification channel by literally sending `TraceId` test
  values through `VerifyPipeline`.

Three shapes were considered:

1. leave those protocol tests on the legacy `TraceId` channel until bundle 3
   deletes the helper/protocol layer entirely;
2. delete the protocol-coverage tests as soon as the runtime path stops using
   them;
3. keep protocol-coverage tests only if they move with the protocol rewrite:
   - if the typed verification pipeline survives, the tests must exercise the
     `TraceRecordRef`-addressed candidate-local protocol;
   - if the typed pipeline is deleted, those tests must be replaced by coverage
     for the successor ordering invariant rather than left behind as documentation
     of a dead `TraceId` contract.

V1 chooses the third shape.

That choice is deliberate:

- row-ID-shaped protocol tests can preserve a false sense that the old protocol
  is still architecturally relevant even after the spec rewrites it;
- deleting coverage without replacement would quietly drop one of the few places
  where the pass-order invariant is mechanically exercised;
- the tests should follow the surviving invariant, not the deleted wire
  identity.

Bundle 1 also needs one concrete runtime-side replacement choice.  Historical
framing: the live code used to split cold-eval runtime-root visibility across
two non-owning `TraceRuntime` pointers:

- `activeRegistry_` for `addRegistryMountPoint(...)`; and
- `activeBackend_` for `recordRuntimeRoot(...)`.

Update 2026-04-29: both of those `TraceRuntime` pointers have already been
removed.  `TraceSession` now owns the `SemanticRegistry` and the
`TraceBackend`; cold-eval publication goes through
`TraceSession::registerRuntimeRootMount` (adding mount points to the
per-session registry) and `traceBackend()->recordRuntimeRoot` (which
delegates to `TraceStore::recordRuntimeRootExclusive`).

Three shapes were considered for the original split-channel problem:

1. keep both side channels and just retarget them to the new backend;
2. delete both side channels and require cold-eval code to thread the active
   `TraceSession` / bound handle explicitly to every runtime-root publication
   site;
3. keep one active-session locator in `TraceRuntime`, but collapse the split
   mutation into one operation that updates the current bound session through
   `publishRuntimeRoot(...)` and replaces the session-owned handle atomically.

The third shape still describes the V1 target.  What remains open is the
atomic-replacement step: cold-eval currently performs two calls
(`registerRuntimeRootMount` then `recordRuntimeRoot`) in sequence rather
than a single session-owned `publishRuntimeRoot` that yields a replacement
`EvalSessionHandle`.

Consequences (updated):

- `TraceRuntime::addRegistryMountPoint(...)` has been removed (the public
  split mutation path is gone);
- `TraceSession::registerRuntimeRootMount(...)` + the backend's
  `recordRuntimeRoot(...)` are the current replacement pair; V1 still needs
  to collapse them into a single `publishRuntimeRoot(...)` returning a
  replacement `EvalSessionHandle`;
- `activeRegistry_` has disappeared from `TraceRuntime`;
- if `TraceRuntime` still keeps an active-session/back-end locator for cold
  eval, that locator must name exactly one session-owned mutation path whose
  success result is the replacement `EvalSessionHandle`, not one registry write
  plus one backend write;
- that locator must not be a raw backend pointer that becomes stale across
  `releaseBackend()` or session replacement; it must follow the owning session
  object or another handle with equally explicit lifetime semantics;
- `rememberSession(...)` therefore becomes session-ownership bookkeeping, not
  the place where a second mutable registry channel is published.

## Validation Plan

### Invariance

1. Build the same snapshot contents in different insertion orders.
   Snapshot root hashes must match.
2. For one snapshot root, rebuild every `binding-v1` from the referenced
   `trace-v1`.
   `candidate_id`, `trace_hash`, `result_hash`, and `dep_key_set_hash` must
   match exactly; any mismatch is corruption.
3. Build the same snapshot root with and without emitting `snapshot-pack-v1`.
   The `snapshot-pack-v1` bytes must match.
4. For one `(semantic_session_key, stable_recovery_key)` pair, build the same
   coherent publication in different write orders for the raw snapshot and
   semantic recovery refs.
   The resulting `eval-view-v1` root must match exactly.
5. Build the same coherent exported evaluator state from one pinned
   `EvalReadView` in repeated runs.
   The resulting `eval-package-v1` bytes must match exactly.
6. Build the same coherent exported evaluator state from one pinned
   `EvalReadView`, then re-emit `snapshot-pack-v1` and `recovery-pack-v1` from
   those same pinned semantic roots before emitting `eval-package-v1` again.
   The resulting `eval-package-v1` bytes must still match exactly because it
   names semantic roots rather than package-descriptor hashes.
7. Build the same recovery member set in different insertion orders.
   Semantic recovery roots must match.
8. For one semantic recovery root, rebuild `candidate_objects_root` from
   `members_root`.
   The rebuilt root must match exactly; extra or missing candidate-object
   entries are corruption.
9. Build and discard compiled recovery roots for the same semantic recovery
   member set.
   The semantic recovery root must not change.
10. Build the same semantic recovery root with and without emitting
   `compiled-recovery-package-v1`.
   The `recovery-pack-v1` bytes must match.
11. Build the same compiled recovery root from the same semantic recovery root
   under the same format/version.
   Compiled recovery roots must match.
12. Build the same semantic recovery root, then rebuild compiled recovery roots
   from that same semantic root in different local histories.
   The compiled namespace key and compiled recovery root must match.
13. Build the same attr vocab contents in different discovery orders.
   Vocab roots must match.
14. Build the same stat-hash contents in different insertion orders.
   Stat-hash roots must match.
15. Build the same semantic archive for the same advertised root set in repeated
    runs.
    The resulting `block-archive-v1` bytes must match exactly.
16. `block-archive-v1` import must reject duplicate block records, out-of-order
    block records, or index entries that do not match the canonical block order.
17. `block-archive-v1` import must reject any extra block not reachable from the
    advertised root set, not just missing blocks.
18. `block-archive-v1` import must reject any record whose declared
    `block_hash`, or any index entry whose key, does not match the hash derived
    from that record's `payload_bytes`.
19. `exportSnapshotPackage(snapshot_root_hash)` and
    `exportRecoveryPackage(semantic_root_hash)` must emit exact single-root
    archives whose advertised root sets match their package descriptors.
20. `exportEvalPackage(view)`'s `semantic_archive` must be rejected by
    `installSnapshotPackage(...)` and `installRecoveryPackage(...)` because its
    advertised root set is coherent-two-root rather than exact-single-root.

### Publication

1. concurrent writers to different snapshot namespaces do not block on one
   global catalog file;
2. concurrent writers to the same snapshot namespace converge by CAS retry;
3. a published ref never points at missing blocks;
4. crash before ref rename leaves the old root readable;
5. crash after ref rename leaves the new root readable.
6. semantic publication does not require vocab-root publication.
7. GC cannot miss a newly published root because root publication is also
   synchronized with shared `gc.lock`.
8. GC does not sweep staged-but-unpublished blocks.
9. stale staged transactions from dead writers are reclaimable without racing a
   live publisher.
10. transaction creation cannot race cleanup because a transaction becomes
    visible only after `tx.lock` is already held.
11. if raw snapshot and semantic recovery refs are published in different
    orders, `openEvalView()` must still return either the previously published
    coherent `(snapshot_root_hash, semantic_root_hash)` pair or the newly
    published pair; it must never mix roots from different eval-view
    publications.
12. if semantic recovery publication advances from `S1` to `S2` while a
    compiled recovery ref for `(stable_recovery_key, S1)` remains published,
    `openEvalView()` must pin `S2` and either:
    - use a compiled root published for `(stable_recovery_key, S2)`, or
    - return a semantic-only view;
    it must never pair `S2` with the compiled root for `S1`.
13. deleting a compiled recovery ref for older semantic root `S1` must not
    change semantic recovery behavior for current semantic root `S2`, and GC
    may reclaim the compiled-only blocks for `S1` once no reader pins them.
14. a ref file whose serialized namespace key does not match the requested
    namespace must be rejected as hard corruption, even if its pathname matches.
15. `compare_and_swap()` must reject an existing ref payload whose serialized
    namespace key, namespace kind, or format version does not match the
    publication target; it must not silently overwrite that ref.
16. if no eval ref exists for `(semantic_session_key, stable_recovery_key)`,
    `openEvalView()` must synthesize the canonical empty `eval-view-v1` for
    those keys and return the corresponding empty snapshot and semantic recovery
    view.
17. if no eval ref exists for `(semantic_session_key, stable_recovery_key)`,
    `openEvalView()` must not consult or attach any compiled recovery root, even
    if a compiled ref exists for the canonical empty semantic root.
18. installing `snapshot-pack-v1` and `recovery-pack-v1` separately must not by
    itself create a coherent evaluator view; `installEvalPackage(...)` is the
    step that publishes the coherent `eval-view-v1` ref.
19. coherent evaluator export must start from one pinned `EvalReadView`; tools
    must not synthesize `eval-package-v1` by separately reading raw snapshot and
    semantic recovery refs.
20. opening an absent namespace may materialize canonical empty immutable
    objects via idempotent `put()`, but it must not publish snapshot,
    semantic-recovery, or eval refs as a side effect of that read.
21. `installSnapshotPackage(...)` is a raw-head update: same root is a no-op,
    different root atomically replaces the raw snapshot head for that
    `semantic_session_key`.
22. `installRecoveryPackage(...)` is a raw-head update: same root is a no-op,
    different root atomically replaces the raw semantic recovery head for that
    `stable_recovery_key`.
23. `installEvalPackage(...)` is a coherent-head update: same `eval-view-v1`
    root is a no-op, different root atomically replaces the coherent eval head
    for that `(semantic_session_key, stable_recovery_key)` pair.
24. if `installEvalPackage(...)` loses the final coherent-head CAS, it may leave
    the coherent eval head unchanged, but it must either:
    - observe that the coherent eval head already matches its target and
      succeed through `confirm_current_ref_and_pin_exact(...)`, or
    - retry / fail with retryable contention without ever claiming success for a
      different coherent evaluator head.
25. `installEvalPackage(...)` must not publish or replace raw snapshot or raw
    semantic-recovery heads as a side effect.
26. publishing or updating a runtime root must rewrite only `snapshot-v1` and
    `eval-view-v1`; it must not rewrite semantic recovery.
    Raw snapshot maintenance-head refresh is allowed after success;
    raw semantic-recovery head refresh is not.
27. `loadAndVerifyRuntimeRoots(view)` must verify persisted runtime roots from
    the pinned `runtime-root-map-v1` before those verified roots are merged into
    the session's semantic registry.
    `bindEvalView(...)` / `openBoundEvalSession(...)` own that merge step for
    evaluator-facing use; callers must not bypass bind by merging the result
    into a live session registry on their own.
28. `publishRuntimeRoot(...)` must make a newly fetched locked runtime root
    visible in the current recording session immediately through the returned
    replacement `EvalSessionHandle`'s in-memory semantic registry, without
    waiting for the session to reopen from storage.
    Callers must not be required to perform a second out-of-band registry
    mutation after success.
    Ordinary same-namespace coherent-head contention must therefore be absorbed
    internally by `publishRuntimeRoot(...)`; once the current cold-eval path
    has accepted that runtime root, the operation must either return the
    replacement handle that includes it or fail unrecoverably.
29. `installEvalPackage(...)` must reject any semantic archive that is missing a
    block in the closure of the referenced snapshot or semantic recovery root.
30. `installSnapshotPackage(...)`, `installRecoveryPackage(...)`,
    `installCompiledRecoveryForRoot(...)`, and the view-based
    `installCompiledRecovery(...)` wrapper must reject any archive that is
    missing a referenced block in the closure of the root they are asked to
    publish.
31. every `install*Package(...)` operation must reject a `block-archive-v1`
    whose advertised root set does not exactly match the root or roots named by
    the supplied package descriptors.
32. successful coherent import via `installEvalPackage(...)` may leave raw
    snapshot or raw semantic-recovery heads unchanged from earlier local state;
    tools that require the exact imported coherent state must use the
    `EvalReadView` returned by `installEvalPackage(...)` or bind that exact
    view through `bindEvalView(...)`;
    reopening the coherent evaluator namespace through
    `openEvalView(...)` / `openBoundEvalSession(...)` is only valid when the
    tool wants whatever coherent state is current at reopen time, not
    necessarily the exact state that was just imported.
    Tools that need raw snapshot or raw semantic-recovery heads to match the
    imported semantic artifacts must install those raw heads explicitly.
33. successful local publication may also leave raw snapshot or raw
    semantic-recovery heads stale if maintenance-head refresh is skipped or
    loses contention; tools that require evaluator-visible current state must
    still use the coherent eval read path, i.e. `openEvalView(...)` or
    `openBoundEvalSession(...)`, rather than reading raw heads.
34. `publishRecord(...)` is a coherent-head update:
    on final `eval-view-v1` CAS loss, it must either observe that the
    committed head already equals its target and succeed as a no-op through
    `confirm_current_ref_and_pin_exact(...)`, or
    reopen/recompute and retry / fail with retryable contention without
    returning a stale handle or stale view.
35. `publishRuntimeRoot(...)` is a coherent-head update:
    on final `eval-view-v1` CAS loss, it must either observe that the
    committed head already equals its target and succeed as a no-op through
    `confirm_current_ref_and_pin_exact(...)`, or
    reopen/recompute and retry internally until it succeeds or fails
    unrecoverably.
    It must not expose ordinary same-namespace contention as a retryable API
    result after the cold-eval path has accepted the new runtime root.
36. evaluator-facing `publishRecord(...)` and `publishRuntimeRoot(...)` must
    return the committed replacement `EvalSessionHandle`, rebound around the
    exact committed `eval-view-v1` root that won or matched the successful CAS,
    not around a later namespace reopen and not around an uncommitted locally
    assembled result; on failure they must leave the caller's prior
    `EvalSessionHandle` authoritative, and `publishRecord(...)` may surface
    retryable contention only before a replacement handle is returned.
    The exact-root success path must update the process pin payload for that
    exact committed root set before releasing shared `gc.lock`.
37. `publishRuntimeRoot(...)` must preserve a previously valid
    `compiled_root_hash` unchanged in the returned replacement handle, because
    that operation leaves `semantic_root_hash` unchanged and does not attach
    new compiled recovery state.
38. `publishRecord(...)` must not carry a stale `compiled_root_hash` across a
    semantic-root change:
    - if the returned `semantic_root_hash` differs from the caller's prior
      handle, the returned replacement handle must have `compiled_root_hash`
      absent;
    - if the returned `semantic_root_hash` is unchanged, carrying forward the
      same valid `compiled_root_hash` is allowed.
    Later compiled attachment for that new semantic root must go through
    `installCompiledRecoveryForRoot(...)`,
    `installCompiledRecovery(...)`, or `ensureCompiledRecovery(...)` and must
    not change that returned-handle rule for `publishRecord(...)` itself.
39. `bindEvalView(state, registry, view)` must call
    `loadAndVerifyRuntimeRoots(view)` and seed the in-memory semantic registry
    before the session is allowed to verify or resolve
    runtime-root-backed provenance.
    It must construct a fresh session-local registry from the immutable
    registry seed rather than mutating and rebinding a previously live registry
    object in place.
    Rebinding the same view against the same registry seed must therefore be
    deterministic and must not duplicate runtime-root mount points.
    Persisted runtime-root metadata must restore forward entries only; reverse
    runtime-root mount points must come only from the session-local dynamic
    runtime mount overlay carried by successful evaluator-facing writes.
40. `openBoundEvalSession(...)` must be equivalent to
    `bindEvalView(state, registry, openEvalView(...))` for the same namespace
    keys.
41. `installEvalPackage(...)` must return the committed pinned
    `EvalReadView`, and evaluator code that needs to operate on that exact
    imported coherent state must be able to do so through
    `bindEvalView(state, registry, returned_view)` without reopening by
    namespace key.
    Returning a view for a later coherent head published after the successful
    install CAS is invalid.
42. `installCompiledRecovery(view, ...)` must return a replacement
    `EvalReadView` whose `compiled_root_hash` equals the exact compiled root
    that won or matched that install path, not a later compiled ref reopened by
    namespace after success.
    That replacement view must also preserve ownership of the input view's
    unchanged `snapshot_root_hash` and `semantic_root_hash` without a pin gap.
43. `ensureCompiledRecovery(view)` may return any one validated compiled root
    current for `view.semantic_root_hash` at successful preflight completion,
    but it must pin and return that exact compiled root before exposing the
    replacement view; the returned replacement view must preserve ownership of
    the input view's unchanged base roots without a pin gap; if it adopts a
    concurrently published compiled root, that root must have been fully
    validated first.
44. `openEvalView(...)` may remain available as a low-level storage primitive,
    but evaluator tests must treat calling it without the required
    runtime-root-hydration step as unsupported for provenance-sensitive
    evaluation.
45. verification- and cold-eval-facing entry points must require
    `EvalSessionHandle`; a bare `EvalReadView` must not be sufficient to enter
    the bound evaluator path.
46. evaluator-facing `publishRuntimeRoot(...)` and `publishRecord(...)` must
    consume and return `EvalSessionHandle`, not only `EvalReadView`, so the
    bound-session invariant survives successful writes.
47. standalone compiled recovery export from `exportRecoveryPackage(...)` must
    be installable through `installCompiledRecoveryForRoot(...)` without first
    opening a pinned eval view.
48. a recovery session may replace its initially opened `EvalReadView` at most
    once during compiled-recovery preflight through
    `installCompiledRecovery(...)` or `ensureCompiledRecovery(...)`;
    after that optional preflight replacement, the resulting view must be the
    one reused for the remainder of the recovery pipeline.
49. exported `eval-package-v1`, `snapshot-pack-v1`, `recovery-pack-v1`,
    `compiled-recovery-package-v1`, and `block-archive-v1` payload bytes must
    remain valid after being wrapped in one ordinary substituted store closure:
    the root store path may identify the export closure, but transport through
    that closure must not change package/archive bytes.
50. `exportEvalPackageToStoreClosure(view)` followed by
    `installEvalPackageFromStoreClosure(root_store_path)` must be equivalent to
    exporting the same payload bytes directly and then calling
    `installEvalPackage(...)` on those bytes after local fetch.
51. the root wrapper store object used by
    `exportEvalPackageToStoreClosure(view)` must be content-addressed, and its
    wrapper manifest must name exactly the sibling payload store paths present
    in `queryPathInfo(root_store_path)->references`;
    `installEvalPackageFromStoreClosure(root_store_path)` must reject any
    manifest/reference-set mismatch.
52. every sibling payload store object named by that wrapper manifest must be
    one content-addressed regular-file object whose file contents are exactly
    the corresponding package/archive bytes;
    `installEvalPackageFromStoreClosure(root_store_path)` must reject any
    directory payload, unexpected object shape, or byte mismatch.
53. every sibling payload store object named by that wrapper manifest must have
    an empty store-reference set;
    `installEvalPackageFromStoreClosure(root_store_path)` must reject any
    wrapper layout that requires a deeper closure than the root wrapper object
    plus its direct sibling payload objects.
54. the normal `TraceSession` / `StoreTraceBackend` session-open path must not
    keep a second pre-bind runtime-root merge flow alongside bind-owned
    hydration:
    opening one evaluator session must go through one adapter-owned setup path
    centered on `openBoundEvalSession(...)` or `bindEvalView(...)`, not a
    separate `loadAndVerifyRuntimeRoots(...)`-then-merge-then-bind sequence.
55. no evaluator-facing backend interface may expose persistent row IDs after
    the seam rewrite:
    `TraceBackend` / `StoreTraceBackend` results that survive across calls must
    use hash-derived refs or backend-private ephemeral tokens rather than
    `TraceId`, `ResultId`, or `DepKeySetId`.
56. successful backend-level `record(...)` and `recordRuntimeRoot(...)`
    operations must replace the backend's internally stored
    `EvalSessionHandle` before subsequent backend `verify(...)`,
    `record(...)`, or `recordRuntimeRoot(...)` calls observe session state.
57. any engine-facing async service introduced for the file-based engine must
    remain hidden behind `StoreTraceBackend`;
    evaluator code such as `TracedExpr` and `TraceSession` must not gain a
    second direct dependency on a public row-ID or root-view service object.
58. backend teardown must preserve the current `releaseBackend()` safety
    property:
    after session shutdown, stale GC-managed `TracedExpr` thunks must fall back
    to direct evaluation or an equivalent null-backend path rather than
    retaining a dead engine-backed adapter or stale pinned/session state.
59. v1 does not require a direct remote eval-query short-circuit protocol.
    A system that wants to ask a remote cache for a deterministic eval-query
    key and get back an accepted result without first naming or fetching an
    artifact closure is a future extension and must not be assumed by the v1
    correctness argument.
60. the evaluator replay/materialization seam must not retain persistent
    row-ID trace identity after the adapter rewrite:
    `TracedExpr::LazyState`, warm-hit result objects, and replay helpers must
    not store or require `TraceId`.
61. no evaluator-facing backend method may continue to require
    `loadFullTrace(traceId)` or any equivalent persistent row-ID lookup once
    Step 11 lands;
    any dep replay cache needed for performance must remain backend-private and
    keyed by hash-derived trace identity.
62. the rollout plan must not leave a mixed public seam in which
    `TraceBackend`, `TracedExpr`, or `TraceSession` have been rewritten to
    root/hash-derived identities while `PrefetchPool` or
    `VerificationOrchestrator` still expose `TraceId`-shaped
    evaluator-visible result contracts.
    (`TraceStoreService` was previously listed here; it has been removed
    from the codebase as part of Bundle 4.)
63. v1 may land as explicit rewrite bundles rather than as per-file
    compatibility-preserving commits;
    the design must not require bridge adapters or legacy shims solely to keep
    intermediate local states buildable.
64. the rollout plan must not delete the public async row-ID helper layer
    before the reusable verification/recovery logic from
    `trace-store-verify.cc` has been moved onto a hash/root-based engine
    interface.
65. the evaluator-seam cutover bundle is not complete while runtime-root
    publication during cold eval happens as two separate mutations
    (`TraceSession::registerRuntimeRootMount` then the backend's
    `recordRuntimeRoot`) rather than a single session-owned
    `publishRuntimeRoot` that yields a replacement `EvalSessionHandle`.
    (Historical: `TraceRuntime::activeRegistry_`, `activeBackend_`, and
    `addRegistryMountPoint(...)` have already been removed; the fields
    are retained here only as archaeological context.)
66. the evaluator-seam cutover bundle must collapse the live split runtime-root
    mutation path into one session-owned operation:
    no successful cold-eval runtime-root publication may depend on a separate
    mount-registration side effect (currently
    `TraceSession::registerRuntimeRootMount`, historically
    `TraceRuntime::addRegistryMountPoint`) in addition to the returned
    replacement session handle.
67. if `TraceRuntime` retains any active-session locator for cold eval, that
    locator must not be a raw backend pointer whose lifetime can be broken by
    `releaseBackend()` or session replacement.
68. the evaluator-seam cutover bundle must preserve backend teardown safety:
    after `releaseBackend()`, stale traced thunks must still fall back to
    direct evaluation / null-backend behavior rather than dereferencing stale
    session-handle or engine state.
69. the public row-ID helper-layer deletion bundle is not complete while
    evaluator-visible prefetch or verify helper state still stores
    row-ID-shaped results such as `TraceStore::VerifyResult`.
70. before the rollout advances past the evaluator-seam cutover bundle, the
    `TraceCacheFixture`-level replay/materialization/teardown tests must still
    pass, including the fixture path that exercises `releaseBackend()` and
    stale traced-thunk fallback.
71. before the rollout advances past the verification/recovery algorithm-port
    bundle, the store/recovery/verify integration suites must still pass under
    the rewritten hash/root-based algorithm interface.
72. deleting the public row-ID helper layer is invalid unless equivalent
    prefetch/verification coverage still exists afterward; test coverage must
    move with the bundle rather than being silently dropped.
73. the verification/recovery algorithm-port bundle is not complete while
    `verification-session.hh` still stores already-verified or trace-context
    memo state keyed by persistent `TraceId` or `NodeStamp`.
74. the verification/recovery algorithm-port bundle is not complete while
    "current binding" or "history scan" helpers still traffic in row-shaped
    carriers equivalent to `CurrentNodeRef` or `TraceHistoryEntry` instead of
    `binding-v1`, `candidate-set-v1` / `candidate-summary-v1`, and
    `struct-profile-summary-v1`.
75. the verification/recovery algorithm-port bundle is not complete while
    trace-context memoization still requires a mutable current-node generation
    token rather than the pinned `(snapshot_root_hash, path-key-v1)` view.
76. the verification/recovery algorithm-port bundle is not complete while
    verification or trace-context resolution still begins from a mutable
    current-node lookup helper equivalent to `lookupCurrentNode(pathId)`
    instead of `lookupCurrentBinding(view, path_key)` on the pinned snapshot.
77. the verification/recovery algorithm-port bundle is not complete while
    `trace-store-verify.cc` or equivalent store-facing verification helpers
    still center the algorithm on row-ID entry points equivalent to
    `verify(pathId, ...)`, `verifyTrace(traceId, ...)`, or
    `recovery(oldTraceId, pathId, ...)` instead of a root/path-based
    `verifyCurrentPath(view, path_key, ...)` plus candidate-local
    `TraceRecordRef` verification.  (`verify-pipeline.hh` was previously
    listed alongside here; it has since been removed.)
78. the verification/recovery algorithm-port bundle is not complete while
    `verification-protocol.hh` or equivalent typed phase protocols still
    send or require `TraceId` as the candidate identity; any retained
    typed verification pipeline must be `TraceRecordRef`-addressed, or
    else be deleted as part of the rewrite.  (`verify-pipeline.hh` was
    previously listed alongside here; it has since been removed.)
79. the verification/recovery algorithm-port bundle is not complete while
    surviving adapter-private async helpers still expose row-shaped
    request/response contracts such as `CurrentNodeRef`,
    `TraceHistoryEntry`, `TraceId`, `ResultId`, or `DepKeySetId` instead
    of hash/root-based current-binding, candidate-index, and
    semantic-object loaders.  (`trace-store-protocol.hh` and
    `TraceStoreService` were previously listed alongside here; both
    have since been removed.)
80. the verification/recovery algorithm-port bundle is not complete while
    surviving helper/result contracts still expose
    `TraceStore::VerifyResult { ..., traceId }`,
    `TraceStore::RecordResult { traceId }`, or equivalent row-ID-shaped outcome
    structs instead of hash-derived replay/result carriers and
    `EvalSessionHandle`-based publish results.
81. the verification/recovery algorithm-port bundle is not complete while the
    rewritten engine core still keeps long-lived semantic caches, pending-write
    state, or allocation counters keyed by `TraceId`, `ResultId`, or
    `DepKeySetId` instead of hash-/root-keyed semantic object caches and staged
    block state.
82. the verification/recovery algorithm-port bundle is not complete while
    `src/libexpr-tests/eval-trace/helpers.hh`, `TraceCacheFixture`
    convenience wrappers, or equivalent test-only accessors still preserve
    row-ID entry points, row-ID-shaped result carriers, or replay paths
    equivalent to `loadFullTrace(traceId)` that production code is no longer
    allowed to expose.
83. the evaluator-seam cutover bundle is not complete while
    `trace-backend.hh`, `traced-expr.hh`, `materialize.cc`, `prefetch-pool.hh`,
    or equivalent evaluator/cache-facing code still uses
    `TraceStore::VerifyResult`, `TraceStore::RecordResult`, `TraceId`-addressed
    replay, or `loadFullTrace(traceId)` as the live contract.
84. the verification/recovery algorithm-port bundle is not complete while
    `src/libexpr-tests/eval-trace/store/verification-protocol.cc` or equivalent
    protocol-coverage tests still encode the live verification protocol in terms
    of `Send<TraceId, ...>` rather than the rewritten candidate-local semantic
    protocol or its explicit successor invariant.
85. the verification/recovery algorithm-port bundle is not complete while test
    bodies under `src/libexpr-tests/eval-trace/store/`,
    `src/libexpr-tests/eval-trace/verify/`, or equivalent suites still assert
    on `result.traceId`, `TraceStore::RecordResult { traceId }`, or
    `verifiedTraceIds` keyed by `TraceId` rather than the rewritten semantic
    replay identity and verification-session memo vocabulary.

### Recovery Closure

1. exporting or GC-marking from `semantic_recovery_root_hash` must reach every
   referenced `trace-v1`, `result-v1`, `dep-key-set-v1`, and `recovery-meta-v1`
   object without consulting compiled indexes.
2. deleting all compiled recovery refs and compiled recovery blocks must not
   break semantic recovery export, import, or GC reachability.
3. importing semantic recovery plus a shipped `compiled-recovery-package-v1`
   must publish the compiled ref only if the package matches the imported
   `stable_recovery_key` and `semantic_recovery_root_hash`.
4. a mismatched `compiled-recovery-package-v1` must be rejected without
   modifying local compiled refs.
5. if a compiled ref already exists for one `(stable_recovery_key,
   semantic_root_hash)`:
   - installing the same `compiled_recovery_root_hash` is a no-op;
   - installing a different root with the same compiled format/version is hard
     corruption;
   - installing a different root with a different compiled format/version
     atomically replaces the old compiled ref.
6. if compiled-ref publication loses a CAS race:
   - observing that the current compiled ref now equals the same target root is
     a successful no-op;
   - observing a different current root with the same compiled format/version
     is hard corruption;
   - observing a different current root with a different compiled
     format/version may retry or return retryable contention, but must not
     claim success for that different root.
7. GC-marking from one `compiled_recovery_root_hash` must reach
   `compiled-recovery-meta-v1` and every compiled index root owned by that
   `compiled-recovery-v1` object.
8. if the embedded `recovery-meta-v1.stable_recovery_key` does not match the
   requested recovery key, `openEvalView()` or raw semantic-recovery open must
   reject that root as hard corruption.
8. `eval-package-v1` install must reject any supplied `recovery-pack-v1`
   payload whose embedded `stable_recovery_key` or
   `semantic_recovery_root_hash` does not match `eval-package-v1`.
9. `installEvalPackage(...)` must not depend on `snapshot-pack-v1`,
   `recovery-pack-v1`, or `eval-package-v1` remaining GC-reachable after
   installation; it validates them from supplied install inputs.
10. `installEvalPackage(...)` must reject any semantic archive that does not
    contain the full closure of the `semantic_recovery_root_hash` named by the
    supplied descriptors.

### Snapshot Closure

1. exporting or GC-marking from one `snapshot-v1` root must reach every
   referenced `trace-v1`, `result-v1`, `dep-key-set-v1`, and `snapshot-meta-v1`
   object through `binding-v1` fields and `snapshot-v1` metadata edges.
   It must also reach `runtime_roots_root` and every referenced runtime-root
   record reachable from that map.
2. deleting compiled recovery refs and compiled recovery blocks must not affect
   snapshot export or snapshot GC reachability.
3. if a published compiled recovery root uses an unsupported compiled
   format/version, `openEvalView()` must ignore it and return a semantic-only
   view rather than failing snapshot access or forcing immediate rebuild.
4. if the embedded `snapshot-meta-v1.semantic_session_key` does not match the
   requested session key, `openEvalView()` or raw snapshot open must reject that
   root as hard corruption.
5. if the embedded `eval-view-meta-v1` keys do not match the requested
   `(semantic_session_key, stable_recovery_key)`, `openEvalView()` must reject
   that root as hard corruption.
6. `eval-package-v1` install must reject any supplied `snapshot-pack-v1`
   payload whose embedded `semantic_session_key` or `snapshot_root_hash` does
   not match `eval-package-v1`.
7. `installEvalPackage(...)` must reject any supplied snapshot or recovery
   package payload whose embedded keys or root hashes do not match the semantic
   roots named by `eval-package-v1`.
8. `installEvalPackage(...)` must reject any semantic archive that does not
   contain the full closure of the `snapshot_root_hash` named by the supplied
   descriptors.

### Reader Pins And GC

1. pinned readers survive concurrent writers;
2. GC marks published roots and live pins;
3. stale pin files from dead processes are reclaimed;
4. GC never reclaims blocks reachable from a live pin file;
5. `openEvalView()` cannot race GC reclamation;
6. a `0 -> 1` pin transition cannot race GC reclamation;
7. root publication cannot race GC reclamation;
8. a delayed `1 -> 0` unpin only postpones reclamation.
9. bounded startup hygiene may reclaim stale pin and staging liveness debris
   without performing a full engine fsck.
10. if GC encounters a corrupt published ref or a published root whose required
    root-object meta validation fails, it must fail closed as hard corruption;
    it must not silently ignore or delete that ref.
11. recovery from corrupt published refs is an explicit maintenance action, not
    an automatic side effect of ordinary startup or GC.
12. `verifyEngine` is read-only: it must not rewrite, delete, or quarantine
    published refs as a side effect of reporting corruption.
13. mutating published-ref quarantine or repair must run under exclusive
    `gc.lock`, not interleaved with ordinary opens, publishes, or GC mark/sweep.
14. quarantined refs must move out of published `refs/` discovery so ordinary
    open and GC root discovery do not treat them as live namespace heads.

### Recovery

1. exact-hit parity
2. snapshot-miss bootstrap parity in terms of "finds a valid candidate", while
   accepting the canonical-order winner rule
3. GitIdentity recovery parity in terms of validity/rejection, not recency
   winner identity
4. direct-hash recovery parity in terms of validity/rejection, including
   duplicate-`trace_hash` candidate handling
5. structural-variant recovery parity in terms of validity/rejection, including
   multiple historical candidates that share one dep-key set but differ in the
   stored hashes reusable by structural subsumption
6. volatile-dep rejection parity
7. candidate identity parity: semantically distinct `trace-v1` records never
   alias to one `candidate_id`
8. `candidate-summary-v1` validation parity: summaries rebuilt from core objects
   match imported summaries byte-for-byte
9. `candidate-set-v1` validation parity: candidate sets rebuilt from semantic
   history membership and core objects match imported sets byte-for-byte
10. `struct-profile-summary-v1` validation parity: profile summaries rebuilt
    from semantic history membership and core objects match imported summaries
    byte-for-byte

### Block Integrity

1. a block file whose pathname matches `block_hash` but whose bytes do not hash
   to `block_hash` is rejected as hard corruption
2. `put(payload_bytes)` computes the stored `block_hash` from exactly the bytes
   it stages; callers do not supply an unchecked hash
3. publish does not rehash large final block files under shared `gc.lock`;
   integrity is enforced by `get(block_hash)` verification and the
   verified-block cache
4. verified-block cache hits are valid only while the block file fingerprint is
   unchanged
5. v1 does not require a background compactor or packed-block format for
   correctness
6. if a later packed-block implementation is added, it must preserve the same
   logical `block_hash -> payload_bytes` mapping and must not reclaim old
   physical containers while they remain reachable from published roots or live
   pins

### Identity

1. string/path identity-sensitive equality parity
2. list/attrset identity-sensitive equality parity
3. warm-hit materialization parity for currently supported identity cases

### Stat-Hash Race Handling

1. mutate a file between pre-hash and post-hash fingerprint checks;
   the entry must not be published
2. keep a file stable across both checks;
   the entry must be published
3. exercise mtime-collision scenarios analogous to racily-clean files;
   the cache must fall back to content validation before publication
4. recursive tree hashes are not admitted to the stat-hash namespace in v1
5. recursive tree hashing reuses cached NAR-form leaf digests and unchanged
   subtree digests without trusting root-path `stat` alone

## Net Recommendation

The correct rewrite target is now concrete:

1. immutable content-addressed blocks
2. fixed `prolly-map-v1`
3. per-namespace root refs with file-lock CAS
4. process pin files plus root-based GC
5. `snapshot-v1` replacing `Sessions` and `SessionRuntimeRoots`
6. `recovery-pack-v1` replacing `History`
7. `stat-hash-v1` replacing the stat-hash singleton semantics; its machine-local
   cache backend may remain SQLite-backed or memory-fronted
8. `tree-hash-cache-v1` preserving performant recursive `NarIdentity` hashing
9. deterministic persistent attr vocab replacing `attr-vocab.sqlite`
10. a new service boundary built around root views and content hashes
11. `trace_hash` retained as the direct-recovery key and split from
    `trace_record_hash`

That design is consistent with the stated constraints:

- it replaces every SQLite store currently in play;
- it is history-invariant by construction;
- it is safe under threads and processes;
- it makes crash consistency explicit;
- it preserves exact hits and recovery strength;
- it preserves identity-sensitive warm-hit behavior;
- it keeps fast recovery by indexing on live-style `trace_hash` while using
  compact candidate summaries with inlined hot-path result data;
- it keeps recursive tree hashing performant without relying on unsound
  root-path `stat` shortcuts;
- it accepts the necessary breaking change that recovery winner selection can no
  longer depend on insertion recency.

## Sources

[^live-threading]: Local code: `src/libexpr/include/nix/expr/eval-trace/store/trace-store.hh:218-223`; `src/libexpr/eval-trace/store/trace-store-service.hh:38-42`.
[^live-service-ids]: Local code: `src/libexpr/eval-trace/store/trace-store-service.hh:61-112`; `src/libexpr/include/nix/expr/eval-trace/store/trace-store.hh:210-217,299-304`.
[^live-backend-interface]: Local code: `src/libexpr/include/nix/expr/eval-trace/cache/trace-backend.hh:24-197`; `src/libexpr/include/nix/expr/eval-trace/cache/trace-session.hh:23-103`; `src/libexpr/eval-trace/cache/trace-session.cc:614-667`; `src/libexpr/eval-trace/context.cc:223-306`; `src/libexpr/eval-trace/cache/traced-expr.cc:24-49,308-347`; `src/libexpr/eval-trace/store/trace-store-service.hh:34-101`; `src/libexpr/eval-trace/store/verification-orchestrator.hh:49-200`.
[^live-schema]: Local code: `src/libexpr/eval-trace/store/trace-store-lifecycle.cc:158-196`.
[^live-publish]: Local code: `src/libexpr/eval-trace/store/trace-store.cc:581-650,667-681`; `src/libexpr/eval-trace/store/trace-store-lifecycle.cc:499-506`; `src/libstore/include/nix/store/sqlite.hh:156-214`; `src/libstore/sqlite.cc:222-273`.
[^live-cache-dir]: Local code: `src/libexpr/eval-trace/store/trace-store-lifecycle.cc:220-229`.
[^users-cache-dir]: Local code: `src/libutil/users.cc:15-23`; `src/libutil/include/nix/util/users.hh:28-30`.
[^live-orchestrator]: Local code: `src/libexpr/eval-trace/store/verification-orchestrator.cc:76-97`.
[^live-recovery]: Local code: `src/libexpr/eval-trace/store/trace-store-verify.cc:988-1035,1117-1165,1291-1335`.
[^live-fast-history]: Local code: `src/libexpr/eval-trace/store/trace-store-lifecycle.cc:340-353`; `src/libexpr/eval-trace/store/trace-store-verify.cc:897-983`.
[^live-history-order]: Local code: `src/libexpr/eval-trace/store/trace-store-lifecycle.cc:346-364`; `src/libexpr/eval-trace/store/verification-orchestrator.cc:85-97`.
[^live-git-summary]: Local code: `src/libexpr/eval-trace/store/trace-store.cc:81-121`; `src/libexpr/eval-trace/store/trace-store-verify.cc:989-1044`.
[^live-struct-profile]: Local code: `src/libexpr/eval-trace/store/trace-store-verify.cc:1137-1256`.
[^live-node-stamp]: Local code: `src/libexpr/include/nix/expr/eval-trace/store/trace-store.hh:299-304`; `src/libexpr/eval-trace/store/trace-store.cc:618-645`; `src/libexpr/eval-trace/store/trace-store-verify.cc:302-330`; `src/libexpr/include/nix/expr/eval-trace/store/verification-session.hh:34-39`.
[^live-vocab]: Local code: `src/libexpr/eval-trace/store/attr-vocab-store.cc:70-160`.
[^live-stat-hash]: Local code: `src/libexpr/eval-trace/store/stat-hash-store.cc:17-29,56-90,110-123`.
[^live-periodic-flush]: Local code: `src/libexpr/include/nix/expr/eval-trace/store/trace-store.hh:203-217,324-379,407-442`; `src/libexpr/eval-trace/store/trace-store-lifecycle.cc:443-460`.
[^live-runtime-roots]: Local code: `src/libexpr/eval-trace/context.cc:276-405`; `src/libexpr/primops/fetchTree.cc:232-260`; `src/libexpr/include/nix/expr/eval-trace/store/semantic-registry.hh:22-31`.
[^live-session-open]: Local code: `src/libexpr/eval-trace/cache/trace-session.cc:651-667`; `src/libexpr/eval-trace/store/verification-orchestrator.hh:194-206`; `src/libexpr/eval-trace/context.cc:194-205`.
[^live-nar-identity]: Local code: `src/libexpr/eval-trace/store/dep-resolution-service.cc:384-392`; `src/libexpr/eval-trace/deps/dep-hash-fns.cc:15-20`.
[^live-trace-serialize]: Local code: `src/libexpr/eval-trace/store/trace-serialize.cc:23-140`.
[^live-result-codec]: Local code: `src/libexpr/eval-trace/store/trace-result-codec.cc:183-191,226-227,239-245,268-269,275-323`.
[^live-materialize]: Local code: `src/libexpr/eval-trace/cache/materialize.cc:430-447,496-511`.
[^live-identity]: Local code: `src/libexpr/eval-trace/context.cc:945-990`.
[^live-substituter-query]: Local code: `src/libstore/include/nix/store/path-info.hh:35-49`; `src/libstore/include/nix/store/realisation.hh:128-158`; `src/libstore/daemon.cc:794-825,969-975`.
[^in-tree-locks]: Local code: `src/libstore/unix/pathlocks.cc:17-160`.
[^in-tree-publish]: Local code: `src/libutil/file-system.cc:318-334,551-575`; `src/libstore/indirect-root-store.cc:5-16`; `src/libstore/local-binary-cache-store.cc:73-84`.
[^in-tree-gc]: Local code: `src/libstore/gc.cc:48-140`.
[^in-tree-hash]: Local code: `src/libutil/file-content-address.cc:66-108`.
[^in-tree-mmap]: Local code: `src/libutil/file-system.cc:234-253`; `src/libutil/nar-accessor.cc:154-177`.
[^in-tree-sync]: Local code: `src/libutil/include/nix/util/sync.hh:1-178`; `src/libfetchers/git-utils.cc:27-36`.
[^in-tree-nar]: Local code: `src/libutil/archive.cc:37-115`; `src/libutil/include/nix/util/nar-accessor.hh:15-54`; `src/libutil/include/nix/util/nar-listing.hh:25-49`.
[^in-tree-pack]: Local code: `src/libfetchers/git-utils.cc:258-350`.
[^dummy-store]: Local code: `src/libstore/include/nix/store/dummy-store.hh:15-31`; `src/libstore/include/nix/store/dummy-store-impl.hh:16-58`; `src/libstore/dummy-store.cc:381-383`.
[^git-update-ref]: Git documentation, `git-update-ref`, especially safe ref updates with old-value verification and batched transactions, <https://git-scm.com/docs/git-update-ref>.
[^git-hash-object]: Git documentation, `git-hash-object`, especially computing the object ID from object contents and optionally writing that object, <https://git-scm.com/docs/git-hash-object>.
[^git-gc-race]: Git documentation, `git-gc`, especially the note that concurrent GC can delete objects another process is using but has not yet referenced, <https://git-scm.com/docs/git-gc>.
[^git-receive-pack]: Git documentation, `git-receive-pack`, especially the quarantine environment rules that incoming objects are staged in a temporary directory, migrated only after `pre-receive` succeeds, and ref updates to quarantined objects are rejected, <https://git-scm.com/docs/git-receive-pack>.
[^git-racy]: Git documentation, `racy-git`, especially the fallback from cached stat matches to stronger file checks for racily-clean entries, <https://git-scm.com/docs/racy-git>.
[^sqlite-atomic]: SQLite documentation, `Atomic Commit In SQLite`, especially the guarantees for atomic commit across multiple attached databases and the need for a single read view to avoid observing part-before and part-after a change, <https://sqlite.org/atomiccommit.html>.
[^ipfs-block-put]: IPFS documentation, `Work with blocks`, especially `ipfs block put` creating a block from raw input data and returning the resulting content identifier, <https://docs.ipfs.tech/how-to/work-with-blocks/>.
[^oci-descriptor]: OCI Image Specification, `descriptor.md`, especially descriptors embedding a content type, digest, and size so higher-level objects can safely reference independently content-addressed content, <https://raw.githubusercontent.com/opencontainers/image-spec/v1.1.1/descriptor.md>.
[^oci-image-index]: OCI Image Specification, `image-index.md`, especially an index object naming multiple manifest descriptors without collapsing them into one mutable object identity, <https://raw.githubusercontent.com/opencontainers/image-spec/v1.1.1/image-index.md>.
[^oci-image-layout]: OCI Image Specification, `image-layout.md`, especially the `blobs` directory plus `index.json` entry-point model and the statement that the layout may be transported via tar/zip/filesystem/networked fetch, <https://raw.githubusercontent.com/opencontainers/image-spec/v1.1.1/image-layout.md>.
[^carv2]: IPLD CARv2 specification, especially CARv2 as an archive with a CARv1 payload and optional index for random access, <https://ipld.io/specs/transport/car/carv2/>.
[^git-bundle]: Git documentation, `git-bundle`, especially bundles as transport for pack data plus refs/prerequisites, <https://git-scm.com/docs/git-bundle>.
[^atproto-repo]: AT Protocol, `Repository` specification, especially the requirement that MST structure and root hash be reproducible from the current key/value mapping regardless of insertion and deletion history, <https://atproto.com/specs/repository>.
[^dolt-block-store]: Dolt documentation, `Block Store`, especially the content-addressed and idempotent-persistence discussion, <https://docs.dolthub.com/architecture/storage-engine/block-store>.
[^btreap]: Daniel Golovin, "B-Treaps: A Uniquely Represented Alternative to B-Trees", ICALP 2009, <https://www.cs.cmu.edu/~dgolovin/papers/btreap.pdf>.
