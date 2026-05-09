# Eval Trace Plan: SQLite-First Semantic Authority

Date: 2026-04-09

This plan is the concrete SQLite-founded alternative to the immutable
block/ref/pin design in
[eval-trace-order-invariant-persistence-notes.md](/home/connorbaker/nix/research/eval-trace-order-invariant-persistence-notes.md).

The governing choice is:

- keep SQLite as the live local semantic authority;
- rebuild the schema and evaluator seam around semantic hashes rather than row
  ids;
- add a canonical content-addressed export/import layer above SQLite for
  portable deterministic identity.

This plan assumes breaking rewrites are acceptable.
It does not preserve row-ID APIs, attached semantic databases, or the current
`sqlite_strand`-centered service topology.

## Summary

V1 uses:

1. one authoritative SQLite database per cache owner;
2. WAL mode for local concurrency;
3. semantic-hash-keyed tables for immutable objects;
4. explicit mutable head tables for current evaluator state;
5. short write transactions and logical generation pins rather than
   whole-eval write or read locks;
6. deterministic SQL indexes for exact-hit and constructive recovery;
7. canonical export/import packages above SQLite for portability and ordinary
   Nix store/substituter transport, with local immutable-row ingest and
   explicit head install.
8. process-local, non-authoritative L1 mirrors for hot evaluator paths where
   the current implementation already benefits from them.

V1 does not use:

- SQLite row IDs as semantic identity;
- multiple attached semantic authority DBs;
- raw `.sqlite` bytes as the portable semantic identity;
- SQLite changesets as semantic union;
- a custom block/ref/pin/GC engine for the live local authority.

## Goals

1. Preserve one coherent local commit boundary for current state and recovery
   history.
2. Preserve exact-hit reuse and current constructive recovery strength.
3. Reduce custom storage-engine implementation relative to the immutable
   block/ref/pin design.
4. Make portable export/import deterministic and substitutable through existing
   Nix transport.
5. Remove persistent semantic dependence on `TraceId`, `ResultId`,
   `DepKeySetId`, `NodeStamp`, and recency-biased history ordering.
6. Keep machine-local hot caches and operational metadata simple.
7. Ensure different Nix processes can interact with the authority DB without
   blocking for the full duration of eval.
8. Keep the authority DB smaller than a naive fully denormalized hash-per-row
   schema by using local normalization and interning where that does not define
   semantic identity.

## Non-Goals

1. V1 does not make live SQLite state itself the portable semantic identity.
2. V1 does not provide CRDT-style commutative live-head merge across machines.
3. V1 does not require a new remote eval-query protocol.
4. V1 does not preserve the current attached-SQLite topology or row-ID helper
   seams.

## Foundation Choice

Three architectures were considered:

1. keep the current SQLite topology and only clean up row IDs;
2. one rebuilt SQLite semantic authority DB plus canonical export/import above
   it;
3. replace live authority with immutable blocks/refs/pins.

V1 chooses the second.

That choice is deliberate:

- SQLite already gives the strongest simple local coherence story: one writer,
  many readers, one transaction, one stable read snapshot;
- exact-hit and recovery indexing are naturally expressed in SQL;
- the hardest objections in the immutable-root note are to the current
  topology, not to SQLite's transaction model in the abstract;
- deterministic portable identity can be recovered honestly above SQLite at the
  export boundary without requiring a custom live object/GC engine.

Consequences:

- local semantic truth is mutable SQL state;
- portable semantic identity is derived during export/import, not identical to
  the live DB image;
- the design intentionally gives up "live root hash equals semantic authority"
  in exchange for a much smaller implementation surface.

## Authority Layout

The authority is one SQLite DB file under the eval-trace cache owner root.

Three placements were considered:

1. daemon-owned state under local-store directories;
2. evaluator-owned cache state under `getCacheDir()` or a successor cache root;
3. one ephemeral in-memory DB with periodic flush.

V1 chooses the second.

Consequences:

- this DB is not a `StorePath`;
- it is local mutable cache authority, not a directly substituted artifact;
- transport and portability are handled by explicit export/import packages, not
  by sharing the live WAL/DB files directly;
- memory-only DBs remain acceptable for tests and ephemeral import staging, but
  not as the durable semantic authority.

## Single-DB Rule

Three semantic-topology choices were considered:

1. recreate the current attached-DB split across trace/history/vocab/cache DBs;
2. one authority DB for semantic state, with optional separate local DBs only
   for machine-local rebuildable caches;
3. shard semantic authority by namespace across many DBs.

V1 chooses the second.

That choice is deliberate:

- WAL weakens cross-DB crash atomicity for attached databases;
- one DB gives one coherent local commit boundary for current heads, history,
  runtime roots, and compiled-acceleration metadata;
- sharding semantic authority moves coherence complexity into import/merge and
  defeats SQLite's strongest property.

Consequences:

- all semantic-authority tables live in one DB file;
- machine-local operational caches such as stat-hash/tree-hash may remain in
  separate SQLite DBs or in-memory caches if they are explicitly
  non-authoritative.

## Schema Model

The schema splits into two classes:

1. immutable semantic object tables;
2. mutable head/projection tables.

### Immutable Semantic Object Tables

These tables are keyed by explicit semantic hashes and never use row order as
  identity:

- `results(result_hash, ...)`
- `dep_key_sets(dep_key_set_hash, ...)`
- `traces(trace_record_hash, trace_hash, result_hash, dep_key_set_hash, ...)`
- `history_members(stable_recovery_key, path_key, trace_record_hash, ...)`

Optional derived acceleration tables may also exist, but must be rebuildable
from the semantic base tables:

- `candidate_summaries(...)`
- `candidate_sets(...)`
- `git_candidates(...)`
- `direct_candidates(...)`
- `struct_profiles(...)`

These tables are never semantic authority.
They are valid only if they can be deterministically rebuilt from immutable
semantic rows and explicit winner-order rules.

### Local Normalization / Interning Tables

To keep the SQLite authority compact, V1 explicitly allows local storage
normalization tables that do not define semantic identity:

- `attr_names(name_id INTEGER PRIMARY KEY, name TEXT UNIQUE, ...)`
- `attr_paths(path_id INTEGER PRIMARY KEY, parent_path_id, child_name_id, path_digest, ...)`
- `strings(string_id INTEGER PRIMARY KEY, value TEXT UNIQUE, ...)`
- `data_paths(data_path_id INTEGER PRIMARY KEY, parent_data_path_id, component, array_index, ...)`
- `dir_sets(dir_set_hash PRIMARY KEY, dirs_blob, ...)`

Three storage-shape choices were considered:

1. no local normalization: repeat full strings and path material everywhere;
2. local normalization/interning tables inside the authority DB;
3. keep external attached vocab DBs and string stores as separate semantic
   authorities.

V1 chooses the second.

That choice is deliberate:

- current eval-trace already demonstrates that attr/path and string interning
  materially reduce storage and repeated blob size;
- keeping those tables inside the one authority DB avoids the cross-DB WAL
  atomicity problem of the current attached-vocab arrangement;
- local surrogate IDs are acceptable for storage layout as long as semantic
  identity, export, and winner order are derived from canonical hashes/digests
  above them.

Consequences:

- row IDs may survive as storage-local surrogates in normalization tables;
- they must not be exposed as evaluator/store semantic identity;
- canonical export resolves normalized rows back into canonical semantic bytes;
- a table can therefore be storage-efficient locally while still exporting a
  stable semantic object model.

### Mutable Head / Projection Tables

These tables define live evaluator-visible state:

- `binding_versions(semantic_session_key, path_key, snapshot_generation, trace_record_hash, trace_hash, result_hash, dep_key_set_hash, is_tombstone, ...)`
- `eval_heads(semantic_session_key, stable_recovery_key, snapshot_generation, snapshot_manifest_hash, semantic_recovery_generation, semantic_recovery_identity, compiled_generation, compiled_recovery_identity, head_generation, ...)`
- `runtime_root_versions(semantic_session_key, source_id, snapshot_generation, locked_url, nar_hash, store_path, is_tombstone, ...)`
- `compiled_recovery_heads(stable_recovery_key, semantic_recovery_identity, format_version, ...)`

### Preferred Physical Table Layout

The plan should be concrete about which tables are physically keyed by local
surrogates and which are physically keyed by composite semantic keys.

V1 chooses one default split:

1. local normalization tables:
   - `INTEGER PRIMARY KEY` local surrogate plus `UNIQUE` canonical key
   - examples:
     - `attr_names(name_id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)`
     - `attr_paths(path_id INTEGER PRIMARY KEY, parent_path_id INTEGER NOT NULL, child_name_id INTEGER NOT NULL, path_digest BLOB, UNIQUE(parent_path_id, child_name_id))`
     - `strings(string_id INTEGER PRIMARY KEY, value TEXT NOT NULL UNIQUE)`
     - `data_paths(data_path_id INTEGER PRIMARY KEY, parent_id INTEGER NOT NULL, component TEXT, array_index INTEGER, UNIQUE(parent_id, component, array_index))`
2. large immutable object tables:
   - local surrogate row plus `UNIQUE` semantic hash column
   - examples:
     - `results(result_id INTEGER PRIMARY KEY, result_hash BLOB NOT NULL UNIQUE, result_kind INTEGER NOT NULL, encoding_version INTEGER NOT NULL, payload BLOB NOT NULL, aux_context BLOB NOT NULL)`
     - `dep_key_sets(dep_key_set_id INTEGER PRIMARY KEY, dep_key_set_hash BLOB NOT NULL UNIQUE, keys_blob BLOB NOT NULL)`
     - `traces(trace_id INTEGER PRIMARY KEY, trace_record_hash BLOB NOT NULL UNIQUE, trace_hash BLOB NOT NULL, result_id INTEGER NOT NULL, dep_key_set_id INTEGER NOT NULL, values_blob BLOB NOT NULL)`
3. associative, versioned, and head tables:
   - composite semantic primary key
   - `WITHOUT ROWID` by default
   - examples:
     - `history_members PRIMARY KEY(stable_recovery_key, path_key, trace_record_hash) WITHOUT ROWID`
     - `candidate_summaries PRIMARY KEY(stable_recovery_key, path_key, trace_record_hash) WITHOUT ROWID`
     - `binding_versions PRIMARY KEY(semantic_session_key, path_key, snapshot_generation) WITHOUT ROWID`
     - `runtime_root_versions PRIMARY KEY(semantic_session_key, source_id, snapshot_generation) WITHOUT ROWID`
     - `eval_heads PRIMARY KEY(semantic_session_key) WITHOUT ROWID`
     - `compiled_recovery_heads PRIMARY KEY(stable_recovery_key, semantic_recovery_identity, format_version) WITHOUT ROWID`

This is deliberate:

- it matches the current implementation's use of compact local IDs and integer
  keyed caches for hot object reuse;
- it keeps secondary indexes on large payload tables smaller than a
  fully-`WITHOUT ROWID`, hash-only layout would;
- it still makes semantic identity explicit and portable because the semantic
  hash columns remain `UNIQUE` and are the only identities that may escape the
  local authority DB.

One key-encoding choice is also explicit:

- semantic digests, manifest hashes, `path_key`, `semantic_session_key`, and
  `stable_recovery_key` should be stored in canonical binary form (`BLOB`)
  rather than hex/base32 text wherever the value is already a fixed or
  deterministic byte string;
- human-facing names and free text remain normalized `TEXT`.

Consequences:

- hot joins inside one process may still use compact local integer FKs for
  `results`, `dep_key_sets`, `traces`, and normalization tables;
- exported/installable identity continues to be semantic-hash based and never
  depends on local row layout;
- index size and cache locality stay closer to the current row-ID store than a
  naive "every table is keyed directly by long text hashes" rewrite.

### Preferred DDL Skeleton

The rewrite should not leave table layout implicit.
One concrete V1 DDL family is preferred:

```sql
CREATE TABLE attr_names (
    name_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
) STRICT;

CREATE TABLE attr_paths (
    path_id INTEGER PRIMARY KEY,
    parent_path_id INTEGER NOT NULL REFERENCES attr_paths(path_id),
    child_name_id INTEGER NOT NULL REFERENCES attr_names(name_id),
    path_digest BLOB,
    UNIQUE(parent_path_id, child_name_id)
) STRICT;
CREATE INDEX idx_attr_paths_digest ON attr_paths(path_digest);

CREATE TABLE strings (
    string_id INTEGER PRIMARY KEY,
    value TEXT NOT NULL UNIQUE
) STRICT;

CREATE TABLE data_paths (
    data_path_id INTEGER PRIMARY KEY,
    parent_id INTEGER NOT NULL REFERENCES data_paths(data_path_id),
    component TEXT NOT NULL,
    array_index INTEGER NOT NULL,
    UNIQUE(parent_id, component, array_index),
    CHECK (
        (array_index = -1 AND component <> '')
        OR (array_index >= 0 AND component = '')
    )
) STRICT;

CREATE TABLE dir_sets (
    dir_set_hash BLOB PRIMARY KEY,
    dirs_blob BLOB NOT NULL
) STRICT, WITHOUT ROWID;

CREATE TABLE results (
    result_id INTEGER PRIMARY KEY,
    result_hash BLOB NOT NULL UNIQUE,
    result_kind INTEGER NOT NULL,
    encoding_version INTEGER NOT NULL,
    payload BLOB NOT NULL,
    aux_context BLOB NOT NULL
) STRICT;

CREATE TABLE dep_key_sets (
    dep_key_set_id INTEGER PRIMARY KEY,
    dep_key_set_hash BLOB NOT NULL UNIQUE,
    keys_blob BLOB NOT NULL
) STRICT;

CREATE TABLE traces (
    trace_id INTEGER PRIMARY KEY,
    trace_record_hash BLOB NOT NULL UNIQUE,
    trace_hash BLOB NOT NULL,
    result_id INTEGER NOT NULL REFERENCES results(result_id),
    dep_key_set_id INTEGER NOT NULL REFERENCES dep_key_sets(dep_key_set_id),
    values_blob BLOB NOT NULL
) STRICT;
CREATE INDEX idx_traces_trace_hash ON traces(trace_hash);
CREATE INDEX idx_traces_result_id ON traces(result_id);
CREATE INDEX idx_traces_dep_key_set_id ON traces(dep_key_set_id);

CREATE TABLE history_members (
    stable_recovery_key BLOB NOT NULL,
    path_key BLOB NOT NULL,
    trace_record_hash BLOB NOT NULL,
    PRIMARY KEY(stable_recovery_key, path_key, trace_record_hash)
) STRICT, WITHOUT ROWID;

CREATE TABLE candidate_summaries (
    stable_recovery_key BLOB NOT NULL,
    path_key BLOB NOT NULL,
    trace_record_hash BLOB NOT NULL,
    trace_hash BLOB NOT NULL,
    result_hash BLOB NOT NULL,
    dep_key_set_hash BLOB NOT NULL,
    result_kind INTEGER NOT NULL,
    encoding_version INTEGER NOT NULL,
    payload BLOB NOT NULL,
    aux_context BLOB NOT NULL,
    git_identity_hash BLOB,
    git_repo_root_digest BLOB,
    git_recoverable INTEGER NOT NULL CHECK (git_recoverable IN (0, 1)),
    struct_profile_hash BLOB,
    PRIMARY KEY(stable_recovery_key, path_key, trace_record_hash)
) STRICT, WITHOUT ROWID;
CREATE INDEX idx_candidate_git
    ON candidate_summaries(stable_recovery_key, path_key, git_identity_hash, trace_record_hash);
CREATE INDEX idx_candidate_trace
    ON candidate_summaries(stable_recovery_key, path_key, trace_hash, trace_record_hash);
CREATE INDEX idx_candidate_struct
    ON candidate_summaries(stable_recovery_key, path_key, dep_key_set_hash, struct_profile_hash, trace_record_hash);

CREATE TABLE binding_versions (
    semantic_session_key BLOB NOT NULL,
    path_key BLOB NOT NULL,
    snapshot_generation INTEGER NOT NULL,
    trace_record_hash BLOB,
    trace_hash BLOB,
    result_hash BLOB,
    dep_key_set_hash BLOB,
    is_tombstone INTEGER NOT NULL CHECK (is_tombstone IN (0, 1)),
    CHECK (
        is_tombstone = 1
        OR (
            trace_record_hash IS NOT NULL
            AND trace_hash IS NOT NULL
            AND result_hash IS NOT NULL
            AND dep_key_set_hash IS NOT NULL
        )
    ),
    PRIMARY KEY(semantic_session_key, path_key, snapshot_generation)
) STRICT, WITHOUT ROWID;
CREATE INDEX idx_binding_lookup
    ON binding_versions(semantic_session_key, path_key, snapshot_generation DESC);

CREATE TABLE runtime_root_versions (
    semantic_session_key BLOB NOT NULL,
    source_id BLOB NOT NULL,
    snapshot_generation INTEGER NOT NULL,
    locked_url TEXT,
    nar_hash TEXT,
    store_path TEXT,
    is_tombstone INTEGER NOT NULL CHECK (is_tombstone IN (0, 1)),
    CHECK (
        is_tombstone = 1
        OR (
            locked_url IS NOT NULL
            AND nar_hash IS NOT NULL
            AND store_path IS NOT NULL
        )
    ),
    PRIMARY KEY(semantic_session_key, source_id, snapshot_generation)
) STRICT, WITHOUT ROWID;
CREATE INDEX idx_runtime_root_lookup
    ON runtime_root_versions(semantic_session_key, source_id, snapshot_generation DESC);

CREATE TABLE eval_heads (
    semantic_session_key BLOB PRIMARY KEY,
    stable_recovery_key BLOB NOT NULL,
    snapshot_generation INTEGER NOT NULL,
    snapshot_manifest_hash BLOB,
    semantic_recovery_generation INTEGER NOT NULL,
    semantic_recovery_identity BLOB NOT NULL,
    compiled_generation INTEGER,
    compiled_recovery_identity BLOB,
    head_generation INTEGER NOT NULL
) STRICT, WITHOUT ROWID;

CREATE TABLE canonical_manifests (
    manifest_hash BLOB PRIMARY KEY,
    kind INTEGER NOT NULL CHECK (kind IN (1, 2, 3, 4)),
    snapshot_generation INTEGER,
    semantic_recovery_identity BLOB,
    compiled_recovery_identity BLOB,
    payload_format_version INTEGER NOT NULL CHECK (payload_format_version > 0),
    payload_bytes_hash BLOB NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at >= 0)
) STRICT, WITHOUT ROWID;
```

This is not a promise about every column name, but it is the intended storage
shape:

- one authority DB;
- `STRICT` everywhere by default;
- integer local IDs only where they materially help locality;
- semantic keys as `BLOB`;
- version/head tables explicitly index the newest visible generation lookup.

Two table-shape choices are now explicit:

- `candidate_summaries` stores the replay-critical result kernel directly:
  - `result_kind`
  - `encoding_version`
  - `payload`
  - `aux_context`
  rather than one opaque encoded blob, so common recovery hits do not need a
  second lookup just to materialize the cached result;
- `canonical_manifests` remains local import/export bookkeeping only:
  - artifact kind
  - payload format version
  - payload bytes hash
  - local generation / semantic identities when present
  It must not become a second semantic authority layer.

One constraint posture is also preferred:

- enable `PRAGMA foreign_keys = ON` for the authority DB;
- use local surrogate FKs where large payload tables reference one another,
  e.g. `traces.result_id -> results(result_id)` and
  `traces.dep_key_set_id -> dep_key_sets(dep_key_set_id)`;
- use `CHECK` constraints for enum-like and boolean-like columns, e.g.
  `git_recoverable IN (0, 1)` and `is_tombstone IN (0, 1)`;
- keep root sentinels where the current in-memory trie/path structures already
  depend on them:
  - `attr_names(name_id=0, name='')`
  - `attr_paths(path_id=0, parent_path_id=0, child_name_id=0)`
  - `data_paths(data_path_id=0, parent_id=0, component='', array_index=-1)`

This is deliberate:

- it matches the current `AttrVocabStore` and `DataPathPool` root-sentinel
  model instead of forcing optional-parent logic through every hot path;
- it lets SQLite reject obvious impossible states at the storage boundary
  instead of relying on C++ convention alone;
- it keeps local payload joins honest without exporting local surrogate IDs as
  semantic identity.

One nullability/domain posture is preferred:

- semantic identity columns are `NOT NULL` wherever the row would be
  nonsensical without them:
  - `eval_heads.semantic_recovery_identity`
  - `history_members.trace_record_hash`
  - `candidate_summaries.trace_hash`
  - `candidate_summaries.result_hash`
  - `candidate_summaries.dep_key_set_hash`
- snapshot-only version rows may leave semantic replay columns null only when
  the row is a tombstone or the table is not semantically about replay state;
- optional acceleration columns remain nullable only when absence is itself
  meaningful:
  - `candidate_summaries.git_identity_hash`
  - `candidate_summaries.git_repo_root_digest`
  - `candidate_summaries.struct_profile_hash`
  - `eval_heads.compiled_recovery_identity`
- integer booleans should be constrained with `CHECK (col IN (0, 1))`
- enum-like storage columns should be constrained to the explicitly supported
  numeric domain rather than left open-ended.
- tombstone/version rows should carry `CHECK` constraints that force payload
  columns to be either all present or intentionally absent according to the
  tombstone bit, rather than allowing half-populated rows.

This is deliberate:

- it keeps the SQLite-first design robust to malformed writes and partial
  implementation drift;
- it matches the current preference for eliminating bug classes at the
  boundary instead of relying on later validation passes;
- it keeps optional compiled and structural acceleration honest without
  confusing "missing" with "present but empty."

### Schema Rules

1. semantic identity must never depend on `rowid`, insertion order, or
   `AUTOINCREMENT`;
2. semantic tables must use explicit primary keys and `WITHOUT ROWID` where
   practical;
3. mutable head rows must carry explicit generation / epoch fields if CAS-like
   update rules are required;
4. any rebuildable acceleration table must have a validation/rebuild rule from
   immutable base tables.
5. binding/runtime-root projection rows are versioned by
   `snapshot_generation`; readers and exports resolve the latest row whose
   generation is `<= eval_heads.snapshot_generation` for each key and must not
   mix rows newer than that generation into the same logical snapshot.
6. `semantic_recovery_identity` and `compiled_recovery_identity` are explicit
   semantic digests or manifest hashes, not implicit row order or update time.
7. storage-local surrogate IDs are allowed inside normalization tables and as
   foreign keys from authority tables, but only if a unique canonical digest or
   digest-bearing parent row remains the semantic identity.
8. `WITHOUT ROWID` is required where it materially reduces duplicate B-tree
   storage for composite semantic keys, but it is not required on every table:
   large object tables may instead use local integer surrogate keys with unique
   semantic-hash columns if that yields smaller indexes and better locality.
9. `STRICT` tables are the default for the rewritten authority schema unless a
   specific SQLite feature requires otherwise; permissive typing should not be
   the default.

## Semantic Replay Identity

V1 defines semantic replay identity directly:

- `trace_record_hash` is the semantic identity of one trace record;
- `result_hash` is the semantic identity of one cached result payload;
- `dep_key_set_hash` is the semantic identity of one canonical dep-key set.

Consequences:

- evaluator/cache-facing replay must use `trace_record_hash`, not `TraceId`;
- candidate identity and exact-hit identity are stable across export/import;
- any surviving helper, test, or backend seam that still requires `TraceId` is
  outside the SQLite-first plan.

## Semantic Identity Model

SQLite remains the local authority, but portable identity is defined above it.

Three identity stories were considered:

1. the raw SQLite DB image is the semantic identity;
2. semantic identity is a canonical export manifest derived from semantic rows
   and current heads;
3. only immutable rows have identity; coherent current state remains
   intentionally non-portable.

V1 chooses the second.

Consequences:

- `sqlite3_serialize()`, backup API, and `VACUUM INTO` are operational snapshot
   tools, not semantic identity by themselves;
- export builds canonical semantic packages from sorted semantic rows plus an
   explicit coherent-head manifest;
- import validates canonical package contents, ingests rows, and then
   re-establishes local head rows transactionally.

## Coherence Object

SQLite does not need `eval-view-v1` as a live Merkle object, but it still needs
an explicit coherence contract.

V1 defines one coherent evaluator-state tuple:

- `(semantic_session_key, stable_recovery_key)` names one live evaluator state;
- that state consists of:
  - one current-binding projection for `semantic_session_key` at one
    `snapshot_generation`;
  - one recovery namespace for `stable_recovery_key` at one
    `semantic_recovery_identity`;
  - one optional compiled-recovery attachment for the current semantic recovery
    identity.

The coherent local success boundary is one SQLite transaction that updates:

- the relevant immutable semantic rows if newly observed;
- the relevant mutable head/projection rows;
- the matching evaluator head row.

Readers must open one read transaction and reuse that snapshot for one
verification or recovery session.

The `eval_heads` row is therefore not only a locator.
It is the coherence token for the live SQL model:

- it names the exact snapshot generation the session may read;
- it names the exact semantic recovery identity the session may use for
  recovery;
- it names the optional compiled attachment that is valid for that recovery
  identity;
- it carries `head_generation`, which is the compare-and-swap token for
  evaluator-visible head updates.

## Logical Generation Pins

Holding one SQLite read transaction open for the entire wall-clock duration of
eval is too expensive in WAL mode: long readers can retain old pages and delay
checkpoint cleanup for unrelated writers.

Three session-pinning choices were considered:

1. one read transaction stays open for the full bound evaluator session;
2. bound sessions pin logical generations/identities and open short read
   transactions against those generations as needed;
3. rely on SQLite snapshot API for every platform/build and reopen historical
   snapshots directly.

V1 chooses the second.

That choice is deliberate:

- it keeps the semantic contract of "one coherent bound session" without
  forcing one long-lived SQLite transaction to stay open through the full eval;
- it avoids tying correctness to universal availability of advanced SQLite
  snapshot features;
- it gives other Nix processes room to checkpoint and write while one eval is
  still running.

Consequences:

- `EvalSessionHandle` becomes a logical generation pin, not just a live DB
  transaction handle;
- it stores at least:
  - `semantic_session_key`
  - `stable_recovery_key`
  - `snapshot_generation`
  - `semantic_recovery_identity`
  - `compiled_recovery_identity`
  - `head_generation`
  - session-local runtime-root overlay;
- each evaluator-facing verify/recovery/read operation opens a short read
  transaction, selects rows for the pinned generations/identities, and closes
  that transaction when the operation completes;
- old generations therefore need lease-aware pruning rather than immediate
  deletion.

## Current Heads And Generation Rules

Mutable heads are not union objects.

Three live-head choices were considered:

1. hidden recency / update-order semantics;
2. explicit generation/CAS semantics on head rows;
3. MV-register rows with deterministic read projection.

V1 chooses the second.

Consequences:

- `current_bindings`, `eval_heads`, and compiled-recovery head rows carry
  explicit compare-and-swap inputs;
- local writers update heads in one transaction and fail/retry on generation
  mismatch;
- import must not silently merge conflicting head rows by row arrival order;
- immutable semantic rows may be unioned by hash-key set union, but head rows
  require explicit same-root/no-op, replace, or conflict rules.

To make this executable, head updates follow one additional rule:

- evaluator-visible write success is defined by one successful update of the
  matching `eval_heads.head_generation` row in the same transaction that wrote
  the corresponding projection rows;
- `binding_versions` / `runtime_root_versions` rows for the new snapshot
  generation are written before the `eval_heads` compare-and-swap update
  becomes visible;
- a reader that begins after commit and selects the committed `eval_heads`
  generation must observe only rows from that generation in the same SQLite
  snapshot.

Generation cleanup follows the same model:

- writers do not rewrite the whole current projection for each new generation;
  they append only the changed `binding_versions` / `runtime_root_versions`
  rows for that generation;
- cleanup may delete obsolete historical projection rows only when a later row
  for the same key dominates them and no live lease still needs the older
  generation;
- long-lived readers remain safe because SQLite snapshot/WAL semantics preserve
  the older view for transactions that began before deletion;
- cleanup policy affects space usage and checkpoint behavior, not semantic
  correctness.

## Read Path

The evaluator-facing read path is:

1. open a short read transaction;
2. read or confirm the bound session's pinned `eval_heads` generation data;
3. resolve the current binding for one requested path by selecting the newest
   `binding_versions` row whose `snapshot_generation <= pinned_generation`;
4. resolve any runtime-root rows the same way for their keyed sources;
5. load the immutable semantic rows for the pinned
   `semantic_recovery_identity`;
6. complete one verification/recovery/read operation;
7. close the read transaction.

This is the SQLite analogue of a pinned coherent `EvalReadView`, but the pin is
logical generation state rather than an indefinitely open SQLite transaction.

Evaluator-facing code should still treat this as a bound capability:

- `openBoundEvalSession(...)` owns "open read transaction + hydrate runtime
  roots + bind evaluator";
- subsequent `verify(...)`, `record(...)`, and `recordRuntimeRoot(...)`
  operations consume and return the bound session state, not a bare DB handle.
- implementations may use ordinary read transactions or `sqlite3_snapshot`
  where useful, but the semantic contract is "one stable SQLite snapshot per
  bound operation against one logical generation pin", not a best-effort
  sequence of reads.

## Process-Local L1 Mirrors

The authority is SQLite, but V1 does not require every hot-path lookup to hit
SQLite from cold state.

Three hot-path choices were considered:

1. no process-local mirrors: every current-binding/history/replay lookup is a
   DB query;
2. process-local mirrors for authoritative rows and decoded payloads, with the
   DB as source of truth;
3. broad mutable process-local mirrors with delayed flush, effectively making
   RAM the first authority.

V1 chooses the second.

That choice is deliberate:

- the current implementation already benefits from `currentNodeIndex`,
  `traceFullCache`, preloaded result payloads, and shared interning pools;
- throwing those away would likely lose more performance than SQLite itself
  costs;
- keeping them explicitly non-authoritative preserves correctness while still
  letting one process avoid repeated lookups during one evaluation burst.

Allowed mirrors include:

- current-binding cache keyed by
  `(semantic_session_key, path_key, snapshot_generation)`;
- decoded result / replay cache keyed by `trace_record_hash` or `result_hash`;
- candidate-summary cache keyed by
  `(stable_recovery_key, path_key, semantic_recovery_identity)`;
- normalization-table mirrors for strings, attr paths, and data paths.

Rules:

1. L1 mirrors must be invalidated or version-keyed by the pinned generation /
   semantic identity they were derived from;
2. they must be safely discardable without semantic loss;
3. they must not become a second mutable publication channel;
4. write success is still the SQLite transaction commit, not cache mutation.
5. normalization mirrors should follow the current `AttrVocabStore` /
   `InterningPools` discipline: resolve from process-local intern tables first,
   and only hit SQLite on cache miss or bulk warmup;
6. process-local mirrors may retain decoded trie/path/string state far longer
   than one read transaction, but they must never outlive the semantic
   generation or normalization epoch they were derived from.

## Session Leases And Pruning

Logical generation pins require a pruning rule.

V1 therefore allows one small lease table:

- `session_leases(lease_id PRIMARY KEY, semantic_session_key, stable_recovery_key, snapshot_generation, acquired_at, renewed_at, owner_pid, owner_start_time, ...)`

Rules:

1. bound sessions publish or renew a lease while they remain live;
2. generation cleanup must not remove rows reachable from any non-stale lease;
3. stale-lease detection is explicit maintenance logic, not semantic
   correctness;
4. if cleanup is conservative and leaves extra old generations, correctness is
   preserved and only disk/WAL pressure changes.

## Failure Model And Atomicity

The SQLite-first design must be robust to interruption, busy contention, and
partial import/publish progress without relying on convention.

Three failure-boundary shapes were considered:

1. treat publish/install as one large opaque operation and recover informally;
2. define explicit phase boundaries and require typed success/failure outcomes
   per phase;
3. rely on exceptions alone and audit side effects by code review.

V1 chooses the second.

That choice is deliberate:

- the current eval-trace code already uses typestate, GDP proofs, and opaque
  capabilities specifically to make phase errors visible in types;
- SQLite gives transaction atomicity, but not semantic clarity about which
  phases may have safely committed partial work;
- imports and publishes need different retry stories for immutable-row union,
  head contention, and fatal corruption.

V1 therefore distinguishes four outcome classes:

1. pre-commit validation failure
   - artifact bytes invalid, recovery summary invalid, runtime-root validation
     failed
   - no authority mutation is allowed
2. safe partial ingest
   - immutable-row union committed, but no evaluator-visible head move
   - safe to retry or abandon; only idempotent semantic rows may have landed
3. retryable publication/install contention
   - busy/locked writer window or `head_generation` CAS mismatch
   - no replacement bound session is returned; the caller retries from the
     still-authoritative prior session or freshly opened head
4. fatal authority failure
   - corruption, invariant violation, schema mismatch, or unrecoverable SQLite
     failure
   - fail closed; do not synthesize a "best effort" replacement session

One distinction is important:

- safe partial ingest is allowed only for ingest/import-style operations whose
  first phase unions immutable rows without moving evaluator-visible heads;
- ordinary evaluator-facing publishes (`publishRecord(...)`,
  `publishRuntimeRoot(...)`) remain all-or-nothing transactions and must not
  expose a partially published visible state.

Two additional atomicity rules follow:

- replacement `EvalSessionHandle` state is created only from the committed head
  row after a successful write transaction; failed attempts leave the prior
  handle authoritative;
- session-local runtime-root reverse overlays are updated only after the
  durable SQLite transaction commits successfully, never on speculative write
  attempt.

The single-DB rule materially improves failure safety here:

- the current split `attr-vocab.sqlite` design requires an explicit vocab
  checkpoint ordering workaround to avoid trace rows committing without their
  referenced vocab rows;
- by putting normalization tables in the same authority DB, the SQLite-first
  plan restores one atomic commit boundary for vocab/interning rows and the
  semantic rows that reference them.

## Write Path

### `publishRecord(...)`

One record operation must:

1. hash and upsert any new immutable semantic rows;
2. insert any new recovery membership rows;
3. refresh derived acceleration rows if maintained eagerly;
4. append one new `binding_versions` row for the affected path under a fresh
   `snapshot_generation`;
5. update the evaluator head row;
6. commit as one transaction or fail.

Returned session state must reflect the committed transaction snapshot.

More concretely:

- `publishRecord(...)` allocates one fresh `snapshot_generation`;
- it appends only the changed path row to `binding_versions`, not a copy of the
  full projection;
- if semantic recovery changed, it also writes or references the new
  `semantic_recovery_identity`;
- it then updates `eval_heads` with a compare-and-swap on `head_generation`;
- if that CAS predicate fails, the whole transaction rolls back and the caller
  retries against the new head.

The performance-critical shape is:

1. perform hashing, dep-key construction, result encoding, and candidate-summary
   derivation outside the authority write transaction;
2. begin the authority write transaction only when the immutable rows and the
   new head target are ready;
3. `INSERT OR IGNORE` immutable semantic rows and normalization rows;
4. write the new generation rows;
5. update `eval_heads` with the compare-and-swap predicate;
6. commit immediately.

This avoids blocking all writers for the full duration of evaluation.

The write path must also be explicitly phase-split for concurrency:

1. phase A, outside the writer transaction:
   - dep hashing
   - result encoding
   - candidate-summary derivation
   - runtime-root verification
   - store/substituter reads
   - canonical import decoding
2. phase B, inside the short writer transaction:
   - resolve or create any missing normalization rows
   - `INSERT OR IGNORE` immutable semantic rows
   - append version rows
   - CAS/update `eval_heads`
3. phase C, after commit:
   - publish replacement session handle
   - update process-local mirrors
   - schedule optional deferred rebuild/checkpoint work

The authority write lock must not be held across phase A or phase C.

### `publishRuntimeRoot(...)`

Runtime-root publication is snapshot-only:

1. verify and upsert runtime-root row(s);
2. append changed `runtime_root_versions` rows under a fresh
   `snapshot_generation`;
3. leave semantic recovery identity unchanged;
4. commit as one transaction or fail.

This operation writes a new `snapshot_generation` but must preserve:

- the prior `semantic_recovery_identity`;
- the prior valid compiled attachment for that recovery identity;
- the bound session's session-local reverse mount overlay, updated with the
  newly accepted runtime root before the replacement session handle returns.

The same projection-lifecycle rule applies here:

- runtime-root version rows that are dominated by newer rows for the same
  source may be pruned once no evaluator-visible head references them;
- reverse mount overlays remain session-local and therefore are not part of
  persisted generation cleanup.

This versioned-row model is the plan's answer to partial updates:

- different Nix processes do not block on whole-snapshot rewrites;
- one path update or runtime-root update appends one small version row and then
  advances the head;
- older bound sessions can still read their pinned generation until cleanup.

This operation is intentionally allowed to be smaller and faster than
`publishRecord(...)`:

- it does not touch semantic recovery membership;
- it should not rebuild recovery acceleration;
- it should reuse prior compiled attachment identity unchanged.

Its failure semantics are correspondingly tighter:

- failed validation returns before any SQLite mutation;
- busy/CAS failure leaves the prior bound session and reverse overlay
  untouched;
- only a successful commit may mint the replacement bound session carrying the
  new persisted runtime-root state.

### Compiled Recovery

Compiled recovery remains derived acceleration.

Three publication boundaries were considered:

1. allow ordinary evaluator-facing writes to opportunistically publish compiled
   recovery heads;
2. allow only explicit compiled-install/ensure operations to publish compiled
   recovery heads;
3. remove persisted compiled recovery entirely.

V1 chooses the second.

Consequences:

- `publishRecord(...)` and `publishRuntimeRoot(...)` do not mutate
  compiled-recovery heads as a side effect;
- compiled-recovery publication remains a validated explicit step;
- runtime-root writes keep a valid prior compiled attachment when semantic
  recovery identity is unchanged.

## Exact-Hit And Recovery Indexes

V1 keeps the fast paths in SQL.

Required indexes:

1. exact current hit:
   covering index on
   `(semantic_session_key, path_key, snapshot_generation DESC)` for
   `binding_versions`, including at least
   `(is_tombstone, trace_record_hash, trace_hash, result_hash, dep_key_set_hash)`
2. exact historical hit:
   `(stable_recovery_key, path_key, trace_hash)` or
   `(stable_recovery_key, path_key, trace_record_hash)`
3. GitIdentity shortcut:
   covering index on
   `(stable_recovery_key, path_key, git_identity_hash, trace_record_hash)` for
   `candidate_summaries`
4. direct-hash shortcut:
   covering index on
   `(stable_recovery_key, path_key, trace_hash, trace_record_hash)` for
   `candidate_summaries`
5. structural candidate grouping:
   covering index on
   `(stable_recovery_key, path_key, dep_key_set_hash, struct_profile_hash, trace_record_hash)`
   for `candidate_summaries`
6. runtime-root latest-version lookup:
   covering index on
   `(semantic_session_key, source_id, snapshot_generation DESC)` for
   `runtime_root_versions`
7. immutable history/export scan:
   covering index on
   `(stable_recovery_key, path_key, trace_record_hash)` for
   `history_members`

The schema should also include one export/install locator:

- `canonical_manifests(manifest_hash PRIMARY KEY, kind, snapshot_generation?, semantic_recovery_identity?, compiled_recovery_identity?, created_at, ...)`

This table is local bookkeeping only:

- it maps a previously exported/imported canonical artifact identity to local
  generations/semantic identities;
- it is never the semantic source of truth;
- it lets imports detect "already ingested" quickly without rewalking the full
  semantic row universe.

Derived acceleration rows may denormalize:

- decoded result payloads;
- Git recoverability flags;
- structural override vectors;
- candidate ordering summaries.

These remain rebuildable from immutable semantic rows.

The exact-hit query shape is therefore:

- given `(semantic_session_key, path_key, pinned_snapshot_generation)`,
  resolve the newest `binding_versions` row with
  `snapshot_generation <= pinned_snapshot_generation`
  using the descending covering index and `LIMIT 1`;
- reject tombstones;
- then use the already denormalized `trace_record_hash`, `trace_hash`,
  `result_hash`, and `dep_key_set_hash` columns without an additional
  history scan.

This is the SQLite-first analogue of the immutable-root plan's
`lookupCurrentBinding(view, path_key)` hot path.

The recovery query shapes should be equally explicit:

1. GitIdentity recovery:
   - query `candidate_summaries`
     by `(stable_recovery_key, path_key, git_identity_hash)`
     ordered by `trace_record_hash`
     with `LIMIT 1`;
   - return encoded result payload and replay identity in the same row;
   - do not join full trace/result rows on the hot path.
2. direct-hash recovery:
   - after recomputing current dep hashes and `trace_hash`, query
     `candidate_summaries`
     by `(stable_recovery_key, path_key, trace_hash)`
     ordered by `trace_record_hash`;
   - iterate only the candidate-summary rows for that exact trace hash.
3. structural recovery:
   - compute current `struct_profile_hash` from the pinned dep-key set and
     current structured dep facts;
   - query `candidate_summaries`
     by `(stable_recovery_key, path_key, dep_key_set_hash, struct_profile_hash)`
     ordered by `trace_record_hash`;
   - only if grouped structural acceleration is absent or stale may the engine
     rebuild it from immutable history members first.

This is deliberate.

The current implementation's `scanHistoryForAttr` and
`lookupHistoryByGitIdentity` are fast because they preload enough trace/result
data in one SQL step to avoid per-candidate round-trips.
The SQLite-first plan must preserve that property, even if the exact tables and
semantic keys change.

One fallback rule is therefore required:

- `history_members` plus immutable `traces` / `results` / `dep_key_sets` rows
  are the semantic source;
- `candidate_summaries` are the normative hot path;
- if those summaries are missing or dirty, the engine may rebuild them from
  `history_members` before retrying recovery, but it should not default to
  scanning and joining the full history for every ordinary recovery attempt.

For hot-path parity with the current SQL implementation, one derived row shape
is specifically allowed:

- `candidate_summaries(stable_recovery_key, path_key, trace_record_hash, trace_hash, result_hash, dep_key_set_hash, result_kind, encoding_version, payload, aux_context, git_identity_hash, git_repo_root_digest, git_recoverable, struct_profile_hash, ...)`

This is deliberate:

- SQLite can keep the existing hot-path advantage only if recovery can accept a
  candidate without joining through full trace/result rows on every candidate;
- therefore wide recovery-summary rows are acceptable derived acceleration, so
  long as they validate against immutable semantic rows or are rebuilt.

To keep write amplification bounded, V1 chooses one additional default:

- `candidate_summaries` for exact/direct/GitIdentity paths are updated eagerly
  during ordinary record publication;
- structural-grouped summaries may be marked dirty and rebuilt lazily from
  immutable rows when first needed or during maintenance.

One additional rule is required for hot-path determinism:

- candidate identity is `trace_record_hash`;
- candidate iteration order is the bytewise lexicographic order of
  `trace_record_hash`;
- SQL helper queries must therefore use explicit `ORDER BY trace_record_hash`
  or a derived equivalent, never implicit row or index iteration order.

## Candidate Ordering

The SQLite design does not preserve hidden recency semantics.

V1 keeps the same explicit semantic change as the immutable-root note:

- candidate order is deterministic and derived from semantic identity, not row
  insertion order;
- winner choice for bootstrap, GitIdentity, direct-hash, and structural
  recovery is explicit and portable;
- if multiple candidates verify, the deterministic winner rule applies.

The implementation may realize that order with SQL indexes, but SQL row order
is not itself the rule.

If a grouped acceleration table such as `candidate_sets` survives, it is
derived state only.
It must either validate to the same ordered candidate universe obtainable from
the immutable semantic rows or be rebuilt.

The same rule applies to all recovery-acceleration tables:

- `candidate_summaries`
- `candidate_sets`
- `git_candidates`
- `direct_candidates`
- `struct_profiles`

None of them may become the sole semantic source for candidate membership or
winner order.

For hot-path performance, V1 allows eager or lazy maintenance:

1. eager:
   - update candidate summaries / grouped acceleration in the same short write
     transaction when cheap enough;
2. deferred:
   - mark acceleration dirty and rebuild in later short maintenance
     transactions;
3. hybrid:
   - keep exact-hit/direct-hash summaries eager, rebuild structural-grouped
     acceleration lazily.

V1 chooses the third shape by default.

Consequences:

- exact-hit and common direct recovery paths remain hot;
- structural acceleration can be rebuilt opportunistically without lengthening
  every ordinary record transaction;
- dropping all acceleration remains semantically safe.

This matches the current implementation's spirit:

- keep the exact-hit and common recovery path fast with eagerly usable
  summaries;
- avoid forcing structural-group rebuild or full replay decode work into every
  writer's critical section.

## Runtime Roots

Runtime roots are part of the current snapshot projection, not recovery
membership.

V1 requires:

1. session open hydrates and verifies persisted runtime roots from the DB into
   the session-local semantic registry;
2. cold eval adds newly fetched runtime roots to the session-local registry
   immediately and then persists them transactionally;
3. reverse mount-point state remains session-local, even if forward runtime
   root entries are persisted.

## Export / Import

### Export

Export does not ship raw live SQLite state as the semantic artifact.

V1 export builds:

1. canonical packages for:
   - current coherent evaluator state
   - snapshot-only state
   - recovery-only state
   - optional compiled recovery
2. a canonical manifest whose identity is derived from sorted semantic rows and
   explicit head fields
3. wrapper store-closure payloads for transport through ordinary Nix
   substituters

Export has one mandatory coherence rule:

- one export operation must run against one SQLite read transaction snapshot
  opened from one chosen `eval_heads` row;
- the exported current snapshot projection, semantic recovery state, and
  optional compiled attachment must all come from that same read transaction;
- export must not read `current_bindings`, recovery rows, and head rows in
  separate transactions.

Export also follows one performance rule:

- export must not hold a writer transaction;
- if canonical package construction is expensive, it should first materialize
  the canonical row set from one read snapshot and then encode outside that
  transaction if possible.

### Import

Import does:

1. validate the canonical package/manifest bytes;
2. ingest immutable semantic rows by hash-key set union;
3. validate or rebuild derived acceleration rows;
4. publish/update mutable head rows transactionally under explicit same-root /
   replace / conflict rules.

Import is split into two phases:

1. ingest phase:
   - union immutable semantic rows into the authority DB;
   - validate or rebuild derived acceleration rows;
   - do not move evaluator-visible heads yet;
2. install phase:
   - compare the imported coherent target against the live `eval_heads` row;
   - same target: no-op;
   - explicit replace allowed: replace in one transaction;
   - otherwise: conflict.

### Transport

V1 reuses existing Nix store/substituter transport for exported artifacts.

Consequences:

- SQLite remains the local authority;
- portable identity lives in the canonical export layer;
- store paths are transport/authentication wrappers, not semantic identity.

The wrapper format should be concrete:

- one content-addressed root manifest file object;
- sibling content-addressed payload file objects for the canonical package
  bytes;
- one store closure rooted at that manifest object;
- payload objects should be plain content-addressed file objects with empty
  reference sets, so the root manifest's direct reference set is the complete
  payload closure for install;
- install fetches the closure through ordinary substituters, reads the local
  payload bytes, validates them, and only then mutates the authority DB.

The root manifest should be a regular-file store object rather than a
directory-shaped wrapper.

That shape is chosen because the current store APIs can read a realised file
store object directly via `requireStoreObjectAccessor(path)->readFile(root)`,
while ordinary closure substitution already follows the root path's declared
references.

Consequences:

- install can read the root manifest bytes directly from the realised root
  `StorePath`;
- install can read each payload object's bytes directly from its realised
  `StorePath`;
- install does not need to reconstruct wrapper bytes by reserialising a NAR or
  walking an arbitrary directory layout.
- export can create the wrapper objects with the same content-addressed
  store-path machinery the current store already exposes for regular-file
  objects, rather than inventing a second transport-specific storage layer.
- install must verify that the root object is a regular file and that each
  referenced payload object is likewise a regular file with an empty reference
  set before treating the root path's direct references as the complete payload
  closure.

That choice is constrained by the current in-tree substitution model:

- `PathSubstitutionGoal` is keyed by `StorePath` and recursively realises
  `ValidPathInfo.references` before copying the target path;
- `ensurePath(path)` therefore materialises a store-path closure, not an
  eval-specific semantic query result;
- `fetchClosure` similarly operates on ordinary store paths and content-address
  / input-addressed closure semantics, not on eval-trace-specific identities.

V1 therefore chooses one concrete remote transport shape:

- the exported eval artifact is represented by one ordinary content-addressed
  root `StorePath`;
- that root path's reference set names every payload object needed for local
  install;
- remote substituters only need to serve a normal store closure for that root
  path;
- the eval cache implementation does not require a custom substituter RPC,
  `.narinfo` extension, or `queryRealisation` meaning in V1.
- the install helper is a local-store operation after materialisation; remote
  stores/substituters are byte sources, not places where SQLite union/head
  semantics run.

## Remote Substitution And Install Semantics

Remote reuse in V1 is "substitute a store closure, then install locally".

It is not "ask a substituter to union semantic rows directly into SQLite".

Three remote-install choices were considered:

1. eval-specific RPC that answers semantic-row or head-update requests
   directly;
2. raw `.sqlite` transport plus local attach/copy/union;
3. ordinary store-closure transport carrying canonical eval artifacts, followed
   by local validation and install.

V1 chooses the third.

That choice is deliberate:

- it fits the current store APIs: `ensurePath(...)`, `queryPathInfo(...)`, and
  closure substitution already exist;
- it preserves the rule that SQLite is the local authority while portable
  semantic identity lives in canonical export bytes;
- it keeps remote transport and local head-install semantics separate.

The concrete V1 install flow is:

1. obtain the exported root manifest `StorePath`;
2. call `ensurePath(root_store_path)` so ordinary substitution realises the
   full referenced closure locally;
3. call `queryPathInfo(root_store_path)` and verify the realised reference set
   matches the manifest's declared payload object set exactly;
4. verify the root object is a regular file and each referenced payload object
   is a regular file with an empty reference set;
5. read the root manifest bytes from the realised root store object and read
   the payload bytes from the realised referenced file store objects;
6. validate manifest and canonical package bytes;
7. ingest immutable semantic rows by local union into the authority DB;
8. validate or rebuild derived acceleration rows;
9. optionally run the explicit head-install transaction for the imported
   coherent target.

This has two important consequences:

- a remote substituter hit never directly updates local evaluator-visible
  heads; it only makes bytes available for local install;
- a remote substituter miss is only a transport miss for that exported artifact
  closure, not an authoritative semantic "cache miss" for the eval cache as a
  whole.

## Remote Union Update Semantics

Remote union/update is split into two separate operations:

1. immutable-row ingest;
2. mutable-head install.

That split is mandatory.

It exists because the current store/substituter protocol knows how to fetch
store objects and closures, but it does not encode eval-trace winner selection
or head conflict rules.

V1 therefore defines two install entry points conceptually:

- `ingestEvalArtifact(root_store_path)`:
  - fetch/realise closure if needed;
  - validate canonical bytes;
  - union immutable semantic rows;
  - rebuild/validate derived acceleration;
  - do not change `eval_heads`;
- `installEvalArtifact(root_store_path, mode)`:
  - perform ingest;
  - then apply explicit same-target / replace / conflict rules to the relevant
    head rows.

The concrete local implementation should follow this shape:

1. decode the canonical package bytes into staged row batches or a temporary
   staging DB owned by the import job;
2. load local normalization rows first, resolving or creating local surrogate
   IDs for repeated strings / attr paths / data paths as needed;
3. union immutable semantic rows into the authority DB with
   `INSERT OR IGNORE`-style semantics on semantic hash primary keys;
4. rebuild or validate derived acceleration rows either in the same staging
   context or by deferred local rebuild;
5. only after immutable ingest succeeds, open the short head-install
   transaction that checks the live `eval_heads` row and performs explicit
   same-target / replace / conflict logic.

Two additional implementation rules follow from the current SQLite and store
APIs:

- canonical exports and imported payload bytes must never encode local
  surrogate IDs from normalization tables; import always resolves those IDs
  locally during staging;
- large immutable-row ingests may be chunked across multiple short writer
  transactions because immutable-row union is idempotent and does not move
  evaluator-visible heads until the final explicit install step.

This preserves the intended separation of concerns:

- store substitution moves bytes;
- SQLite ingest unions immutable semantic rows locally;
- head install remains the only place where local winner state changes.

It also gives large imports a concrete concurrency story:

- two processes importing the same remote artifact may race on immutable-row
  ingest, but semantic-hash primary keys make the converged row set the same;
- they still serialize at the final head-install transaction, where the
  explicit same-target / replace / conflict rules decide the visible outcome;
- interrupted imports may leave a strict subset of immutable rows installed,
  but no visible head movement until install runs successfully.

The same split applies to snapshot-only, recovery-only, and compiled-recovery
artifacts:

- snapshot/recovery ingest may safely union immutable rows first;
- coherent evaluator-head movement remains an explicit local decision;
- compiled recovery remains acceleration state and must not piggyback on
  ordinary evaluator-facing semantic writes.

This also sharpens the meaning of "union":

- remote transport may deliver new immutable semantic rows;
- local install may union those rows by semantic primary key;
- remote transport does not itself choose winners for mutable heads.

## Optional Delta / Changeset Transport

V1's normative portable format remains the canonical package/manifest closure.

SQLite changesets or row-delta bundles are optional implementation
optimizations, not the semantic transport format.

If a later optimization ships deltas, it must obey all of the following:

- the delta names a base canonical manifest identity and a target canonical
  manifest identity;
- it contains only immutable-row inserts and optional derived-row payloads;
- it excludes `eval_heads`, compiled head rows, and any other mutable winner
  rows;
- it excludes local surrogate IDs from normalization tables and instead carries
  canonical values that can be resolved into local normalization rows during
  staging;
- it is only applicable when the importer has already ingested the named base
  canonical manifest identity locally; otherwise install must fall back to the
  full canonical package closure;
- applying it locally must still produce the same immutable semantic row set as
  validating and ingesting the target canonical package bytes;
- explicit head-install rules still run afterward as a separate step.

This keeps SQLite changesets in the only role where they plausibly help:

- bulk row transport into local staging tables or import jobs for immutable
  rows, potentially in chunked batches that avoid one giant writer
  transaction.

It keeps them out of roles where they would blur semantics:

- remote head movement,
- winner selection,
- semantic identity,
- authoritative remote negative answers.

## Merge / Union Semantics

Three merge domains exist:

1. immutable semantic rows:
   - merge by set union on semantic primary keys
2. derived acceleration rows:
   - validate and keep or discard/rebuild
3. mutable heads:
   - explicit conflict rules only

V1 head rules:

- same semantic target: no-op
- same key, different target, explicit replace allowed by the operation:
  replace transactionally
- same key, different target, no replace permission:
  conflict

There is no hidden last-writer-wins by import order.

This distinction is important:

- immutable row union is always safe by semantic key;
- head installation is never implicit row union;
- importing a package must not silently advance local evaluator heads unless the
  caller requested the install phase for that target.

SQLite session/changeset machinery is not the semantic merge protocol.

V1 allows it only as an optional row-transport optimization for immutable-row
ingest if the row keys already match semantic hashes.
It must not be used as the meaning of head merge or winner selection.

It may still be useful for one narrower purpose:

- bulk transport of immutable-row inserts between local staging DBs or import
  jobs, provided those rows are already keyed by semantic hashes and the
  install phase still runs the explicit head rules afterward.
- transport of immutable-row deltas inside a wrapper store closure, provided
  the delta still names explicit base/target canonical manifest identities and
  no mutable head rows are included.

## Concurrency Model

V1 uses:

- one authority DB in WAL mode;
- separate SQLite connections per worker/thread;
- one writer at a time, many readers;
- logical generation pins for bound sessions, with short read transactions per
  bound operation.

This is intentionally simpler than a custom file/block/ref/pin/GC engine.

Consequences:

- write concurrency is lower than the immutable-root design's theoretical
  namespace-level parallelism;
- local correctness is easier to reason about;
- exact-hit and recovery lookup hot paths stay close to current SQL behavior;
- cross-process liveness and crash semantics come from SQLite's journaling and
  transaction model, not from custom pins/GC.

The plan therefore makes one performance trade explicit:

- semantic-authority writes serialize through SQLite's one-writer model;
- the design compensates by keeping hot-path lookup and recovery logic in SQL
  and by moving only rebuildable machine-local caches outside the authority DB.

That trade implies one implementation rule:

- evaluator-facing write transactions must stay short and avoid long-running
  export, rebuild, or bulk-import work inside the same write transaction;
- large rebuild/import work should stage immutable rows first and publish head
  movement only in the final short transaction.
- different Nix processes may evaluate concurrently because ordinary eval work
  happens outside writer transactions; contention is concentrated in short
  publish/install windows rather than spanning whole eval duration.

It also implies one operational rule:

- WAL checkpointing and vacuum/compaction policy are maintenance concerns that
  must not be used as semantic boundaries;
- a checkpoint may change IO/space behavior, but it must not affect export
  identity, winner order, or coherent head semantics.

Recommended operational posture:

- WAL enabled;
- one SQLite connection per worker/process role, not one shared multi-thread
  connection;
- busy timeout/backoff configured so short writer conflicts retry rather than
  fail immediately;
- passive or bounded checkpoints driven by maintenance heuristics, not by
  semantic events;
- large imports use staging and final short install transactions.
- prepared statements and connection-local caches should be reused per role so
  SQL compilation cost does not become the hot-path bottleneck once
  `sqlite_strand` is removed.
- any `retrySQLite(...)`-style retry loop should wrap only the short SQL
  transaction or statement bundle, not the expensive phase-A hashing,
  substitution, parsing, or artifact-validation work.

The lock-avoidance story is therefore explicit:

- bound evaluator sessions do not hold one SQLite read transaction open for
  the full wall-clock duration of eval;
- exact-hit verification and recovery open one short read transaction, run one
  bounded set of indexed queries against the pinned logical generation, and
  close immediately;
- long-running work such as dep hashing, result decoding, substitution,
  structured-data parsing, and artifact validation runs outside authority write
  transactions;
- immutable-row ingest for large imports may be chunked across multiple short
  writer transactions because it is head-free and idempotent;
- the final head-install transaction remains small because it should perform
  only compare-and-swap checks plus the minimal version-row/head updates.

This is a direct break from the current `sqlite_strand`-centered topology:

- the rewrite should not preserve one in-process serialized service shell that
  forces all DB work through a single long-lived executor;
- correctness comes from SQLite's own transaction model plus explicit logical
  generation pins and typed head-install rules, not from keeping the old
  helper topology alive.

## Storage Size Strategy

The SQLite-first plan must not accidentally become larger than the current
store by repeating long strings and path material everywhere.

V1 therefore chooses:

1. semantic-hash rows for immutable object identity;
2. local normalization tables for repeated strings, attr-path tries, data paths,
   and other repeated vocabulary;
3. wide derived recovery summaries only where they demonstrably save more IO
   than they cost in duplication.

Consequences:

- repeated attr path components should be interned/trie-normalized in the same
  authority DB, not in a second attached authority DB;
- repeated dep-source/file-path/data-path strings may use one shared strings
  table plus local surrogate IDs;
- canonical export must resolve those normalized rows back into canonical
  semantic bytes;
- compression and page-size tuning remain implementation choices, but semantic
  correctness must not depend on them.

## Interning Strategy And Evaluator Locality

The storage-size win is not just "normalize some strings".
It should deliberately preserve the useful parts of the current
`AttrVocabStore`, `StringInternTable`, and `InterningPools` design.

V1 therefore chooses:

1. one authority-local attr-name interner:
   - `attr_names(name_id, name)`
   - unique by canonical name bytes
   - process-local mirror keeps `name_id <-> Symbol` and resolved
     `string_view` mappings hot
2. one authority-local attr-path trie:
   - `attr_paths(path_id, parent_path_id, child_name_id, path_digest, ...)`
   - unique by `(parent_path_id, child_name_id)`
   - `path_digest` is a derived/indexed secondary column, not the primary
     interning key
   - process-local mirror keeps the trie structure hot so evaluator code can
     extend/resolve paths without repeated SQL walks
3. one shared string/value interner:
   - `strings(string_id, value)`
   - used for repeated dep-source strings, dep keys, generic string payloads,
     and other repeated textual vocabulary
4. one data-path trie:
   - `data_paths(data_path_id, parent_id, component, array_index)`
   - mirrors the current `DataPathPool` shape and avoids repeating structured
     JSON/TOML access paths in every semantic row
5. one dir-set definition table:
   - `dir_sets(dir_set_hash, dirs_blob, ...)`
   - deduplicates large structured directory-origin sets exactly once

That choice is deliberate:

- the current tree already proves these shapes are worthwhile:
  `AttrVocabStore` keeps a monotonic attr trie plus `StringInternTable`,
  while `InterningPools` shares one string interner across dep/source/key
  vocab and one trie for data paths;
- using the same shapes inside the one authority DB preserves the disk-size
  and hot-lookup wins without the current cross-DB durability workaround;
- local surrogate IDs remain excellent storage/layout tools even though they
  are forbidden as semantic identity.

One storage-key choice is explicit:

- `attr_paths` should be interned primarily by trie edge
  `(parent_path_id, child_name_id)`, matching the current `AttrVocabStore`
  `packKey(parent, child)` design and the evaluator's natural "extend one path
  by one child" operation;
- `path_digest` remains useful as a deterministic derived/indexed column for
  hashing, export validation, or reverse lookup, but it should not replace the
  edge key as the authoritative local interning key unless profiling proves a
  clear win.

The intended evaluator fast path is:

1. resolve name/path/string/data-path IDs from process-local mirrors first;
2. on miss, do one indexed lookup or `INSERT OR IGNORE` in SQLite;
3. cache the local surrogate mapping in the process-local mirror;
4. keep semantic/export identity based on canonical hashes/digests, not those
   surrogates.

Consequences:

- repeated attr-path components, dep-source strings, file paths, and structured
  data paths should be stored once locally and referenced by surrogate ID;
- hot evaluator operations can stay close to the current zero-copy
  `string_view` / trie-resolution style rather than materializing repeated SQL
  text blobs for every dep or path lookup;
- canonical export still resolves all local surrogates away, so no foreign DB
  ever has to preserve local ID assignments.

## Interning Concurrency And Warmup Policy

Interning must be both space-efficient and contention-aware.
It cannot assume one process privately allocates surrogate IDs and flushes them
later the way the current split stores do.

Three implementation shapes were considered:

1. eager full-table bulk warm for every normalization table on every process;
2. fully lazy per-value SQL lookup for every normalization miss;
3. hybrid warmup plus concurrent insert-or-select resolution.

V1 chooses the third.

That choice is deliberate:

- the current code demonstrates two different useful patterns:
  `AttrVocabStore` bulk-loads one monotonic trie because it is central and
  comparatively compact, while the general dep/string pools are still treated
  as interning pools rather than semantic authority;
- a one-DB rewrite should preserve those wins without forcing every process to
  full-scan every large normalization table at startup;
- concurrent Nix processes need one deterministic way to converge on the same
  local surrogate mapping without cross-process ID preallocation.

V1 therefore requires one explicit normalization-resolution algorithm:

1. consult the process-local mirror first;
2. on miss, attempt `INSERT OR IGNORE` or equivalent against the table's
   canonical unique key:
   - `attr_names(name)`
   - `attr_paths(parent_path_id, child_name_id)`
   - `strings(value)`
   - `data_paths(parent_id, component, array_index)`
3. read back the canonical local surrogate ID with one indexed `SELECT`;
4. cache that mapping in the process-local mirror.

This algorithm should not depend on `last_insert_rowid()` or
SQLite-`RETURNING` as the semantic source of the chosen surrogate:

- concurrent processes may race on the same canonical value;
- `INSERT OR IGNORE` plus indexed `SELECT` is the correct convergent rule;
- the table's unique constraint, not local arrival order, determines the
  stable surrogate row.

Consequences:

- different processes inserting the same canonical interned value or trie edge
  converge by the table's unique constraint and then read back the same local
  surrogate ID;
- no cross-process surrogate-ID allocator or coordination channel is required;
- local surrogate IDs stay stable enough for local FKs and process mirrors
  while remaining entirely absent from canonical export identity.

Warmup policy is likewise hybrid by default:

1. eager once-per-process warmup is allowed for compact monotonic vocab that is
   central to the evaluator hot path:
   - attr-name table
   - attr-path trie
2. lazy or bounded incremental warmup is the default for larger tables:
   - shared strings
   - data paths
   - dir-set definitions
3. append-only normalization tables may support high-water-mark refresh:
   - load rows with `id > last_loaded_id`
   - merge them into the local mirror outside writer-critical publication work

The default V1 posture is therefore concrete:

- eager warmup by default:
  - `attr_names`
  - `attr_paths`
- lazy or bounded incremental warmup by default:
  - `strings`
  - `data_paths`
  - `dir_sets`
- widening eager warmup beyond attr vocab requires profiling evidence that the
  startup/scan cost is justified by evaluator hot-path wins.
- in particular, V1 should not default to full-table eager warmup for
  `strings`, `data_paths`, or `dir_sets`; any eager treatment there must be
  bounded by an explicit size cap, high-water window, or measured hotspot set.

This keeps the plan close to the current implementation without copying its
topology:

- it preserves the value of `maxLoadedNameId` / `maxLoadedPathId`,
  `bulkLoadString`, and `bulkLoad`-style incremental mirror refresh;
- but it keeps normalization tables inside the one authority DB so semantic
  rows and normalization rows commit atomically.

## Compiled Recovery Rules

Compiled recovery remains optional derived acceleration keyed by:

- `stable_recovery_key`
- `semantic_recovery_identity`
- `format_version`

V1 rules:

1. same semantic recovery identity + same compiled target: no-op;
2. same semantic recovery identity + different target + same format version:
   conflict or corruption;
3. same semantic recovery identity + different target + different format
   version: explicit replace allowed;
4. evaluator-facing writes do not publish compiled recovery as a side effect;
5. exported compiled recovery is valid only if it names the exact semantic
   recovery identity it accelerates.

This keeps compiled recovery as validated acceleration instead of a hidden
mutable side channel.

## What This Plan Gives Up

1. live semantic identity is not already a content-addressed root;
2. portable identity exists only at export/import boundaries;
3. live mutable heads are not naturally commutative union objects;
4. remote proof-only short-circuit is a later protocol, not a property of the
   live store.

These are accepted tradeoffs in exchange for a much smaller implementation
surface.

## Migration Plan

The migration is a rewrite, not a compatibility bridge.

V1 explicitly allows:

- deleting the attached semantic SQLite topology rather than adapting it;
- deleting public row-ID helper seams rather than preserving bridge adapters;
- rewriting tests and fixtures to the new semantic vocabulary in the same
  rollout bundles.

V1 explicitly does not require:

- keeping intermediate bridge APIs buildable;
- dual-writing both row-ID and semantic-hash authority paths for an extended
  period;
- preserving `TraceId`-shaped evaluator/store helper contracts after the
  corresponding bundle lands.

## Type-Level Enforcement Strategy

The SQLite-first rewrite should reuse the same type-level enforcement toolbox
already used throughout eval-trace rather than falling back to comments and
convention.

The governing rule is the same one called out in
`src/libexpr/eval-trace/CLAUDE.md`:

- if a correctness property can be expressed as type-level structure, it
  should be;
- fixes should eliminate bug classes, not just patch call sites.

For this plan, the patterns map as follows:

1. phantom types (`Tagged<Tag, T>`)
   - semantic identities such as `trace_record_hash`, `result_hash`,
     `dep_key_set_hash`, `semantic_recovery_identity`, and artifact manifest
     hashes should remain nominally typed, not raw `Blake3Hash` / `string`
     aliases;
   - import/export helpers should not accept structurally identical but
     semantically distinct hashes interchangeably.
2. GDP continuations (`Proof<Tag>`, `Certifier<Tag>`)
   - use scoped proofs for local preconditions such as:
     - authority-DB mutation permission
     - SQLite blocking/strand access if a blocking store service survives
     - runtime-root/session binding preconditions
   - use `withProofIf`, not unconditional `withProof`, whenever the proof
     certifies a runtime condition.
3. opaque capabilities
   - for high-blast-radius state transitions, prefer opaque capabilities with
     private constructors over public proof tags;
   - `EvalSessionHandle` should remain an opaque bound capability, not a bag of
     fields callers can synthesize;
   - any future "replace allowed" head-install mode or validated runtime-root
     publication token should use the same sealing rule if misuse would be
     catastrophic.
4. typestate + linear consumption
   - the import path is naturally a state machine:
     - substituted bytes
     - validated artifact
     - immutable rows ingested
     - heads installed
   - if helper code survives as separate phases, those transitions should be
     modeled with typestate/linear consumption rather than informal sequencing.
5. session types
   - if an async store/orchestrator protocol survives, keep the protocol
     session-typed;
   - if the SQLite-first rewrite deletes that protocol layer, delete the
     session types too rather than preserving a fake legacy protocol.
6. singleton/exhaustive dispatch
   - query/recovery behavior classification should stay exhaustively encoded,
     not become ad hoc stringly branching in SQL helper code.
7. closed failure algebras
   - mutating helpers should return a closed typed failure domain rather than
     raw booleans, ad hoc exception text, or "maybe succeeded" conventions;
   - retryable contention, validation failure, and fatal corruption must be
     distinguishable in type, not inferred from strings.

The plan makes three concrete type-level choices:

1. import/install pipeline:
   - model internal install flow as linear typestate, e.g.
     `SubstitutedArtifact -> ValidatedArtifact -> IngestedArtifact -> InstalledArtifact`
   - only the validated/ingested states may perform the next transition
   - callers do not get a public "just install these bytes" escape hatch.
2. session bootstrap:
   - public code receives only an opaque `BoundEvalSession`
   - any intermediate `OpenedSession` / `HydratedSession` / `RuntimeRootsLoaded`
     states are internal bootstrap typestate, not public surface
   - this mirrors the current `Unbound -> Bound` orchestrator discipline but
     keeps the intermediate phases hidden.
3. runtime-root publication:
   - publishing a runtime root should consume a sealed, validated runtime-root
     fact, not arbitrary `(source_id, locked_url, nar_hash, store_path)` tuples
     that callers can fabricate after the fact.
4. typed mutating outcomes:
   - `publishRecordSqlite(...)`, `publishRuntimeRootSqlite(...)`, and
     `installEvalArtifact...(...)` should return typed result carriers that
     distinguish retryable contention from validation failure and fatal
     authority corruption;
   - only the success branch yields a replacement bound session or installed
     artifact state;
   - `PublishFailure` should not include "safe partial ingest", because
     evaluator-facing publish remains all-or-nothing;
   - `InstallFailure` may distinguish validation failure from
     "immutable rows ingested but head install not attempted/completed" if the
     API keeps ingest and install as one combined entry point.
   - carrier spelling should follow the current codebase's existing style:
     named structs plus `std::variant`, not an invented generic `Result<T, E>`
     abstraction.

Two explicit carryovers from the current eval-trace discipline are required:

- no raw `future.get()` or equivalent untyped blocking in the new helper path;
  eval-time async blocking still goes through `syncAwait`, and shutdown paths
  call the local store/service directly when no concurrency remains;
- no mutable reference escape hatches that bypass proof/capability guards for
  head updates, runtime-root publication, or import/install staging state.

## Local Helper API Shape

The SQLite-first rewrite needs one explicit helper vocabulary so the new design
lands on concrete call sites instead of reintroducing row-ID seams indirectly.

V1 should expose a deliberately small local helper layer.

The public/local surface should be:

- `openBoundEvalSessionSqlite(state, useCache, rootLoader, sessionConfig, registrySeed) -> EvalSessionHandle`
- `lookupCurrentBindingSqlite(session, path_key) -> optional<BindingRow>`
- `verifyCurrentPathSqlite(session, path_key) -> optional<VerifyHit>`
- `recoverPathSqlite(session, path_key, old_trace_record_hash?) -> optional<VerifyHit>`
- `publishRecordSqlite(session, path_key, cached_result, deps) -> PublishResult`
- `publishRuntimeRootSqlite(session, verified_runtime_root) -> PublishResult`
- `exportEvalPackageSqlite(session_or_head) -> CanonicalEvalArtifact`
- `exportEvalPackageToStoreClosureSqlite(local_store, artifact) -> StorePath`
- `installEvalArtifactFromStoreClosureSqlite(local_store, db, root_store_path, mode) -> InstallResult`

Normalization resolution should also be explicit internal helper vocabulary:

- `resolveOrInsertAttrNameSqlite(name) -> AttrNameLocalId`
- `resolveOrInsertAttrPathSqlite(parent_path_id, child_name_id, path_digest?) -> AttrPathLocalId`
- `resolveOrInsertStringSqlite(value) -> StringLocalId`
- `resolveOrInsertDataPathSqlite(parent_id, component, array_index) -> DataPathLocalId`
- `resolveOrInsertDirSetSqlite(dir_set_hash, dirs_blob) -> DirSetLocalRef`

Those helpers remain internal because:

- they traffic in local surrogate IDs that must never escape as semantic API
  identity;
- they encode the table-specific insert-or-select convergence rules; and
- they are performance-sensitive enough that the implementation may bulk warm,
  batch, or statement-cache them without exposing that complexity upward.

Each helper should own one fixed prepared-statement bundle per SQLite
connection:

- `resolveOrInsertAttrNameSqlite(...)`
  - `SELECT name_id FROM attr_names WHERE name = ?`
  - `INSERT OR IGNORE INTO attr_names(name) VALUES (?)`
- `resolveOrInsertAttrPathSqlite(...)`
  - `SELECT path_id FROM attr_paths WHERE parent_path_id = ? AND child_name_id = ?`
  - `INSERT OR IGNORE INTO attr_paths(parent_path_id, child_name_id, path_digest) VALUES (?, ?, ?)`
  - optional secondary `SELECT path_id FROM attr_paths WHERE path_digest = ?`
    only for validation/debug/rebuild tooling, not the hot evaluator path
- `resolveOrInsertStringSqlite(...)`
  - `SELECT string_id FROM strings WHERE value = ?`
  - `INSERT OR IGNORE INTO strings(value) VALUES (?)`
- `resolveOrInsertDataPathSqlite(...)`
  - `SELECT data_path_id FROM data_paths WHERE parent_id = ? AND component = ? AND array_index = ?`
  - `INSERT OR IGNORE INTO data_paths(parent_id, component, array_index) VALUES (?, ?, ?)`
- `resolveOrInsertDirSetSqlite(...)`
  - `SELECT dir_set_hash FROM dir_sets WHERE dir_set_hash = ?`
  - `INSERT OR IGNORE INTO dir_sets(dir_set_hash, dirs_blob) VALUES (?, ?)`

These statement bundles should be created once per role-local SQLite
connection and reused, not prepared ad hoc on every lookup.

The same rule should extend to the rewritten semantic authority statements.
The implementation should adopt one role-local statement catalog with
current-codebase-style names rather than ad hoc SQL callsites.

Preferred statement families include:

- current snapshot / head:
  - `lookupEvalHead`
  - `updateEvalHeadCAS`
  - `appendBindingVersion`
  - `lookupCurrentBindingAtGeneration`
  - `appendRuntimeRootVersion`
  - `lookupRuntimeRootAtGeneration`
- immutable semantic rows:
  - `lookupResultByHash`
  - `insertResultByHash`
  - `lookupDepKeySetByHash`
  - `insertDepKeySetByHash`
  - `lookupTraceByRecordHash`
  - `insertTraceByRecordHash`
  - `insertHistoryMember`
- recovery acceleration:
  - `lookupCandidateSummaryByGitIdentity`
  - `lookupCandidateSummaryByTraceHash`
  - `lookupCandidateSummaryByStructProfile`
  - `insertCandidateSummary`
- import/export bookkeeping:
  - `lookupCanonicalManifest`
  - `insertCanonicalManifest`
  - `scanSemanticRowsForExport`
  - `scanBindingVersionsForExport`

The statement catalog should also be split by connection role rather than
forcing one kitchen-sink connection state:

1. bound-session read connection:
   - `lookupEvalHead`
   - `lookupCurrentBindingAtGeneration`
   - `lookupRuntimeRootAtGeneration`
   - `lookupCandidateSummaryByGitIdentity`
   - `lookupCandidateSummaryByTraceHash`
   - `lookupCandidateSummaryByStructProfile`
   - `lookupTraceByRecordHash`
   - `lookupResultByHash`
   - `lookupDepKeySetByHash`
2. publish/write connection:
   - normalization `resolveOrInsert*` bundles
   - `insertResultByHash`
   - `insertDepKeySetByHash`
   - `insertTraceByRecordHash`
   - `insertHistoryMember`
   - `insertCandidateSummary`
   - `appendBindingVersion`
   - `appendRuntimeRootVersion`
   - `updateEvalHeadCAS`
3. import/ingest connection:
   - normalization `resolveOrInsert*` bundles
   - immutable-row `INSERT OR IGNORE` statements
   - `lookupCanonicalManifest`
   - `insertCanonicalManifest`
   - optional chunk-progress bookkeeping
4. export/maintenance connection:
   - `lookupCanonicalManifest`
   - `scanSemanticRowsForExport`
   - `scanBindingVersionsForExport`
   - generation-pruning and stale-lease scan statements

One concrete C++ layout is preferred:

```c++
struct NormalizationStmts {
    SQLiteStmt lookupAttrName;
    SQLiteStmt insertAttrName;
    SQLiteStmt lookupAttrPath;
    SQLiteStmt insertAttrPath;
    SQLiteStmt lookupString;
    SQLiteStmt insertString;
    SQLiteStmt lookupDataPath;
    SQLiteStmt insertDataPath;
    SQLiteStmt lookupDirSet;
    SQLiteStmt insertDirSet;
};

struct ReadConnStmts {
    SQLiteStmt lookupEvalHead;
    SQLiteStmt lookupCurrentBindingAtGeneration;
    SQLiteStmt lookupRuntimeRootAtGeneration;
    SQLiteStmt lookupCandidateSummaryByGitIdentity;
    SQLiteStmt lookupCandidateSummaryByTraceHash;
    SQLiteStmt lookupCandidateSummaryByStructProfile;
    SQLiteStmt lookupTraceByRecordHash;
    SQLiteStmt lookupResultByHash;
    SQLiteStmt lookupDepKeySetByHash;
};

struct WriteConnStmts {
    NormalizationStmts norm;
    SQLiteStmt insertResultByHash;
    SQLiteStmt insertDepKeySetByHash;
    SQLiteStmt insertTraceByRecordHash;
    SQLiteStmt insertHistoryMember;
    SQLiteStmt insertCandidateSummary;
    SQLiteStmt appendBindingVersion;
    SQLiteStmt appendRuntimeRootVersion;
    SQLiteStmt updateEvalHeadCAS;
};

struct IngestConnStmts {
    NormalizationStmts norm;
    SQLiteStmt insertResultByHash;
    SQLiteStmt insertDepKeySetByHash;
    SQLiteStmt insertTraceByRecordHash;
    SQLiteStmt insertHistoryMember;
    SQLiteStmt insertCandidateSummary;
    SQLiteStmt lookupCanonicalManifest;
    SQLiteStmt insertCanonicalManifest;
};

struct ExportConnStmts {
    SQLiteStmt lookupCanonicalManifest;
    SQLiteStmt scanSemanticRowsForExport;
    SQLiteStmt scanBindingVersionsForExport;
    SQLiteStmt scanRuntimeRootVersionsForExport;
    SQLiteStmt scanSessionLeases;
    SQLiteStmt pruneOldBindingVersions;
    SQLiteStmt pruneOldRuntimeRootVersions;
};
```

The point of these names is not aesthetics.
It is to make the role split visible in the same way the current
`TraceStore::State::Stmts` makes statement drift visible today.

One concrete lifecycle helper family is preferred:

- `createNormalizationStmts(SQLite &, NormalizationStmts &)`
- `createReadConnStmts(SQLite &, ReadConnStmts &)`
- `createWriteConnStmts(SQLite &, WriteConnStmts &)`
- `createIngestConnStmts(SQLite &, IngestConnStmts &)`
- `createExportConnStmts(SQLite &, ExportConnStmts &)`

That keeps prepared-statement ownership centralized in the lifecycle/schema
layer instead of scattering `SQLiteStmt::create(...)` calls throughout the
store core.

One ownership rule is preferred:

- bundle struct declarations live with the rewritten SQLite authority core
  types in the successor to
  [trace-store.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/store/trace-store.hh)
- prepared SQL text and `SQLiteStmt::create(...)` calls live in the successor
  to
  [trace-store-lifecycle.cc](/home/connorbaker/nix/src/libexpr/eval-trace/store/trace-store-lifecycle.cc)
- the evaluator/backend seam must not own raw statement bundles directly
- helper functions in higher layers consume typed helper methods, not naked
  `SQLiteStmt` handles.

This is deliberate:

- it preserves the current separation between store-core schema/statement
  ownership and evaluator-facing behavior;
- it avoids leaking raw SQL machinery upward into `TraceBackend`,
  `TraceSession`, or materialization code;
- it makes store-core rewrites local rather than forcing SQL text to sprawl
  across unrelated files.

One concrete file landing is preferred:

- keep [trace-store.hh](/home/connorbaker/nix/src/libexpr/include/nix/expr/eval-trace/store/trace-store.hh) as the primary public store-core declaration file unless implementation size forces a purely private split
  - it declares:
    - row structs
    - statement-bundle structs
    - store-core-private helper result carriers
- keep [trace-store-lifecycle.cc](/home/connorbaker/nix/src/libexpr/eval-trace/store/trace-store-lifecycle.cc) as the schema/pragma/statement-construction implementation file
  - it owns:
    - DDL strings
    - pragma setup
    - statement creation
    - bulk-load / flush / schema-upgrade hooks
- keep [trace-store.cc](/home/connorbaker/nix/src/libexpr/eval-trace/store/trace-store.cc) as the store-core query/write helper implementation file
  - it owns:
    - row materialization helpers
    - exact-hit lookups
    - record/runtime-root write helpers
    - import/export bookkeeping helpers
- keep [trace-store-verify.cc](/home/connorbaker/nix/src/libexpr/eval-trace/store/trace-store-verify.cc) as the verification/recovery algorithm file
  - it owns:
    - verification/recovery algorithms over the rewritten row/helper vocabulary
- if extra files are needed for size, they should be private implementation files under the same store directory, not new public compatibility layers

The goal is not to preserve the old class boundaries mechanically.
The goal is to preserve a coherent ownership split:

- schema/SQL ownership in the store core
- algorithm ownership in verify/recovery code
- evaluator-facing seams above that layer

One concrete SQL-shape rule is preferred for the named hot statements:

- `lookupCurrentBindingAtGeneration`
  - selects exactly one newest visible row with
    `WHERE semantic_session_key = ? AND path_key = ? AND snapshot_generation <= ?`
    `ORDER BY snapshot_generation DESC LIMIT 1`
- `lookupRuntimeRootAtGeneration`
  - same newest-visible pattern for `(semantic_session_key, source_id, snapshot_generation)`
- `updateEvalHeadCAS`
  - one `UPDATE ... WHERE semantic_session_key = ? AND head_generation = ?`
    that bumps `head_generation` and rewrites the coherent head fields
- `lookupCandidateSummaryByGitIdentity`
  - `WHERE stable_recovery_key = ? AND path_key = ? AND git_identity_hash = ?`
    `ORDER BY trace_record_hash LIMIT 1`
- `lookupCandidateSummaryByTraceHash`
  - `WHERE stable_recovery_key = ? AND path_key = ? AND trace_hash = ?`
    `ORDER BY trace_record_hash`
- `lookupCandidateSummaryByStructProfile`
  - `WHERE stable_recovery_key = ? AND path_key = ? AND dep_key_set_hash = ? AND struct_profile_hash = ?`
    `ORDER BY trace_record_hash`

The point is to pin down the intended indexed query shape, not merely the
conceptual lookup name.

One concrete SQL text family is preferred for the main hot statements:

- `lookupEvalHead`
  ```sql
  SELECT stable_recovery_key,
         snapshot_generation,
         snapshot_manifest_hash,
         semantic_recovery_generation,
         semantic_recovery_identity,
         compiled_generation,
         compiled_recovery_identity,
         head_generation
  FROM eval_heads
  WHERE semantic_session_key = ?
  ```
- `lookupCurrentBindingAtGeneration`
  ```sql
  SELECT trace_record_hash,
         trace_hash,
         result_hash,
         dep_key_set_hash,
         is_tombstone,
         snapshot_generation
  FROM binding_versions
  WHERE semantic_session_key = ?
    AND path_key = ?
    AND snapshot_generation <= ?
  ORDER BY snapshot_generation DESC
  LIMIT 1
  ```
- `appendBindingVersion`
  ```sql
  INSERT INTO binding_versions(
      semantic_session_key,
      path_key,
      snapshot_generation,
      trace_record_hash,
      trace_hash,
      result_hash,
      dep_key_set_hash,
      is_tombstone)
  VALUES (?, ?, ?, ?, ?, ?, ?, ?)
  ```
- `appendRuntimeRootVersion`
  ```sql
  INSERT INTO runtime_root_versions(
      semantic_session_key,
      source_id,
      snapshot_generation,
      locked_url,
      nar_hash,
      store_path,
      is_tombstone)
  VALUES (?, ?, ?, ?, ?, ?, ?)
  ```
- `updateEvalHeadCAS`
  ```sql
  UPDATE eval_heads
  SET snapshot_generation = ?,
      snapshot_manifest_hash = ?,
      semantic_recovery_generation = ?,
      semantic_recovery_identity = ?,
      compiled_generation = ?,
      compiled_recovery_identity = ?,
      head_generation = head_generation + 1
  WHERE semantic_session_key = ?
    AND head_generation = ?
  ```
- `lookupTraceByRecordHash`
  ```sql
  SELECT trace_id, trace_hash, result_id, dep_key_set_id, values_blob
  FROM traces
  WHERE trace_record_hash = ?
  ```
- `lookupResultByHash`
  ```sql
  SELECT result_id, result_kind, encoding_version, payload, aux_context
  FROM results
  WHERE result_hash = ?
  ```
- `lookupDepKeySetByHash`
  ```sql
  SELECT dep_key_set_id, keys_blob
  FROM dep_key_sets
  WHERE dep_key_set_hash = ?
  ```
- `lookupCandidateSummaryByGitIdentity`
  ```sql
  SELECT trace_record_hash,
         trace_hash,
         result_hash,
         dep_key_set_hash,
         result_kind,
         encoding_version,
         payload,
         aux_context
  FROM candidate_summaries
  WHERE stable_recovery_key = ?
    AND path_key = ?
    AND git_identity_hash = ?
  ORDER BY trace_record_hash
  LIMIT 1
  ```
- `lookupCandidateSummaryByTraceHash`
  ```sql
  SELECT trace_record_hash,
         result_hash,
         dep_key_set_hash,
         result_kind,
         encoding_version,
         payload,
         aux_context
  FROM candidate_summaries
  WHERE stable_recovery_key = ?
    AND path_key = ?
    AND trace_hash = ?
  ORDER BY trace_record_hash
  ```
- `lookupCandidateSummaryByStructProfile`
  ```sql
  SELECT trace_record_hash,
         trace_hash,
         result_hash,
         dep_key_set_hash,
         result_kind,
         encoding_version,
         payload,
         aux_context
  FROM candidate_summaries
  WHERE stable_recovery_key = ?
    AND path_key = ?
    AND dep_key_set_hash = ?
    AND struct_profile_hash = ?
  ORDER BY trace_record_hash
  ```

One concrete SQL text family is also preferred for the next-tier statements:

- `lookupRuntimeRootAtGeneration`
  ```sql
  SELECT locked_url,
         nar_hash,
         store_path,
         is_tombstone,
         snapshot_generation
  FROM runtime_root_versions
  WHERE semantic_session_key = ?
    AND source_id = ?
    AND snapshot_generation <= ?
  ORDER BY snapshot_generation DESC
  LIMIT 1
  ```
- `insertCandidateSummary`
  ```sql
  INSERT OR REPLACE INTO candidate_summaries(
      stable_recovery_key,
      path_key,
      trace_record_hash,
      trace_hash,
      result_hash,
      dep_key_set_hash,
      result_kind,
      encoding_version,
      payload,
      aux_context,
      git_identity_hash,
      git_repo_root_digest,
      git_recoverable,
      struct_profile_hash)
  VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  ```
- `lookupCanonicalManifest`
  ```sql
  SELECT kind,
         snapshot_generation,
         semantic_recovery_identity,
         compiled_recovery_identity,
         payload_format_version,
         payload_bytes_hash,
         created_at
  FROM canonical_manifests
  WHERE manifest_hash = ?
  ```
- `insertCanonicalManifest`
  ```sql
  INSERT OR IGNORE INTO canonical_manifests(
      manifest_hash,
      kind,
      snapshot_generation,
      semantic_recovery_identity,
      compiled_recovery_identity,
      payload_format_version,
      payload_bytes_hash,
      created_at)
  VALUES (?, ?, ?, ?, ?, ?, ?, ?)
  ```
- `scanSemanticRowsForExport`
  ```sql
  SELECT t.trace_record_hash,
         t.trace_hash,
         r.result_hash,
         dk.dep_key_set_hash,
         t.values_blob,
         r.result_kind,
         r.encoding_version,
         r.payload,
         r.aux_context,
         dk.keys_blob
  FROM export_trace_record_set x
  JOIN traces t ON t.trace_record_hash = x.trace_record_hash
  JOIN results r ON r.result_id = t.result_id
  JOIN dep_key_sets dk ON dk.dep_key_set_id = t.dep_key_set_id
  ORDER BY t.trace_record_hash
  ```
- `scanBindingVersionsForExport`
  ```sql
  SELECT path_key,
         snapshot_generation,
         trace_record_hash,
         trace_hash,
         result_hash,
         dep_key_set_hash,
         is_tombstone
  FROM binding_versions
  WHERE semantic_session_key = ?
    AND snapshot_generation <= ?
  ORDER BY path_key, snapshot_generation
  ```
- `scanRuntimeRootVersionsForExport`
  ```sql
  SELECT source_id,
         snapshot_generation,
         locked_url,
         nar_hash,
         store_path,
         is_tombstone
  FROM runtime_root_versions
  WHERE semantic_session_key = ?
    AND snapshot_generation <= ?
  ORDER BY source_id, snapshot_generation
  ```
- `scanSessionLeases`
  ```sql
  SELECT lease_id,
         semantic_session_key,
         stable_recovery_key,
         snapshot_generation,
         renewed_at,
         owner_pid,
         owner_start_time
  FROM session_leases
  ORDER BY renewed_at
  ```
- `pruneOldBindingVersions`
  ```sql
  DELETE FROM binding_versions
  WHERE semantic_session_key = ?
    AND snapshot_generation < ?
    AND path_key = ?
  ```
- `pruneOldRuntimeRootVersions`
  ```sql
  DELETE FROM runtime_root_versions
  WHERE semantic_session_key = ?
    AND snapshot_generation < ?
    AND source_id = ?
  ```

Export is therefore allowed one temporary workset table inside the export
transaction:

```sql
CREATE TEMP TABLE export_trace_record_set(
    trace_record_hash BLOB PRIMARY KEY
) WITHOUT ROWID;
```

The point is to keep export scans on one stable fixed query shape rather than
building ad hoc SQL text with arbitrarily long `IN (...)` lists.

These are intentionally close to the current `TraceStore::State::Stmts`
discipline:

- one named statement per hot query shape;
- rows pre-shaped so recovery and replay do not devolve into repeated ad hoc
  joins;
- no hidden dependence on implicit row order.

This is deliberate:

- it matches the current style where one `State::Stmts` bundle names hot SQL
  explicitly;
- but it avoids reintroducing the old "single serialized store shell" as an
  architectural dependency;
- it makes it obvious which paths must stay short and write-critical versus
  which paths are read-only or chunked maintenance.

The intent is the same as the current `TraceStore::State::Stmts` layout:

- all hot SQL is named, prepared once per connection, and grouped by role;
- query shape drift becomes visible in the C++ API surface instead of hiding in
  arbitrary string literals throughout the codebase;
- tests can assert that hot-path helpers continue to use the intended indexed
  statements rather than silently regressing to ad hoc queries.

Helper/type naming should likewise be concrete once coding starts:

- persistent/local-row types should use `...Row` or `...VersionRow`:
  - `BindingRow`
  - `BindingVersionRow`
  - `RuntimeRootVersionRow`
  - `EvalHeadRow`
  - `CandidateSummaryRow`
- semantic API carriers should use capability/result nouns, not table nouns:
  - `BoundEvalSession`
  - `VerifyHit`
  - `PublishedSession`
  - `InstalledArtifact`
- backend-local SQLite plumbing may use `...Sqlite` suffix at the boundary:
  - `publishRecordSqlite(...)`
  - `installEvalArtifactFromStoreClosureSqlite(...)`
- nominal semantic hash wrappers should not gain SQLite-specific names merely
  because the local authority implementation is SQLite-backed.

The decomposed phases should remain internal helper/state-machine steps, not
the stable public vocabulary:

- `substituteArtifactClosure(...) -> SubstitutedArtifact`
- `validateArtifact(...) -> ValidatedArtifact`
- `ingestArtifactRows(...) -> IngestedArtifact`
- `installArtifactHeads(...) -> InstalledArtifact`
- `prepareRuntimeRoot(...) -> VerifiedRuntimeRoot`

These helpers are intentionally local:

- they are not new daemon/store protocol operations;
- they sit above ordinary store substitution and below the evaluator seam;
- they consume/return semantic hash identities and bound session handles, not
  `TraceId` / `ResultId` / `DepKeySetId`.

That helper vocabulary also forces one result-carrier choice:

- evaluator/cache hit carriers become semantic:
  - `VerifyHit { CachedResult value, trace_record_hash }`
  - `RecordHit { trace_record_hash }` if a distinct record result survives
- import/export/install carriers become artifact-oriented:
  - `CanonicalEvalArtifact`
  - `IngestSummary`
  - `HeadInstallResult`

The helper vocabulary should also carry the existing eval-trace enforcement
patterns into the SQLite-first design:

- `EvalSessionHandle` is an opaque capability, not caller-constructible state;
- semantic identities in helper signatures stay phantom-typed;
- if `ingestEvalArtifactSqlite(...)` and `installEvalArtifactFromStoreClosureSqlite(...)`
  are decomposed internally, their intermediate state should be typestated so
  an install step cannot run on unvalidated bytes;
- if an async service layer survives, its protocol should carry semantic
  identities rather than raw row IDs and remain session-typed.
- mutating helper result carriers should be closed sums such as:
  - `RetryableHeadConflict`
  - `AuthorityBusy`
  - `ArtifactInvalid`
  - `AuthorityCorruption`
  rather than untyped "false"/"null" failure channels.

The default SQLite-first choice is stricter (updated 2026-04-29):

- `TraceStoreService`, `trace-store-protocol.hh`, and `verify-pipeline.hh`
  have already been removed (the Bundle 4 removal).  What remains from
  that original list is `verification-protocol.hh` (holds the shared
  `VerifyOutcome` enum and `RecoveryState` stage tags) and
  `VerificationOrchestrator` (owns the `PrefetchPool` and drives
  `verifyAttrImpl`).
- Do not preserve `verification-protocol.hh` /
  `VerificationOrchestrator` just because they exist today;
- delete those layers if direct local backend/helper calls are sufficient;
- only keep a typed async protocol if it still serves a real concurrency or
  ownership purpose after the rewrite.

The mutating helper carriers should therefore be explicit:

- `PublishedSession { EvalSessionHandle session; uint64_t snapshot_generation; uint64_t head_generation; }`
- `HeadInstallResult { uint64_t snapshot_generation; uint64_t head_generation; Blake3Hash semantic_recovery_identity; std::optional<Blake3Hash> compiled_recovery_identity; }`
- `InstalledArtifact { HeadInstallResult head_install; optional<EvalSessionHandle> session_after_install; }`
- `IngestSummary { Blake3Hash manifest_hash; uint64_t semantic_rows_inserted; uint64_t normalization_rows_inserted; bool already_present; }`
- `IngestedOnly { IngestSummary summary; }`
- `PublishResult = std::variant<PublishedSession, PublishFailure>`
- `InstallResult = std::variant<InstalledArtifact, IngestedOnly, InstallFailure>`
- `PublishFailure` / `InstallFailure` are closed typed sums that separate:
  - retryable head contention
  - retryable SQLite busy/lock contention
  - validation failure
  - fatal authority corruption/invariant failure

One concrete carrier split is preferred:

- `PublishFailure`:
  - `RetryableHeadConflict { expected_head_generation, observed_head_generation }`
  - `AuthorityBusy { operation, sqlite_extended_code? }`
  - `PublishValidationRejected { reason_kind, offending_path_key? }`
  - `AuthorityCorruption { invariant, detail }`
- `InstallFailure`:
  - `ArtifactInvalid { manifest_hash?, reason_kind }`
  - `BaseArtifactMissing { base_manifest_hash }`
  - `ReplaceConflict { live_head_generation, live_manifest_hash?, incoming_manifest_hash }`
  - `AuthorityBusy { operation, sqlite_extended_code? }`
  - `AuthorityCorruption { invariant, detail }`
- if a combined helper performs both immutable ingest and head install, it may
  additionally return a typed "ingested only" success/result state rather than
  smuggling that distinction through exceptions or logs.

The corresponding C++ spelling should be direct:

- `using PublishFailure = std::variant<RetryableHeadConflict, AuthorityBusy, PublishValidationRejected, AuthorityCorruption>;`
- `using InstallFailure = std::variant<ArtifactInvalid, BaseArtifactMissing, ReplaceConflict, AuthorityBusy, AuthorityCorruption>;`
- `using PublishResult = std::variant<PublishedSession, PublishFailure>;`
- `using InstallResult = std::variant<InstalledArtifact, IngestedOnly, InstallFailure>;`

One concrete C++ shape is preferred:

```c++
struct RetryableHeadConflict {
    uint64_t expected_head_generation;
    uint64_t observed_head_generation;
};

struct AuthorityBusy {
    enum class Operation : uint8_t {
        PublishRecord,
        PublishRuntimeRoot,
        IngestArtifact,
        InstallArtifactHeads,
    };
    Operation operation;
    std::optional<int> sqlite_extended_code;
};

struct PublishValidationRejected {
    enum class ReasonKind : uint8_t {
        InvalidDeps,
        InvalidResultEncoding,
        InvalidRuntimeRoot,
    };
    ReasonKind reason_kind;
    std::optional<Blake3Hash> offending_path_key;
};

struct AuthorityCorruption {
    std::string invariant;
    std::string detail;
};

struct ArtifactInvalid {
    std::optional<Blake3Hash> manifest_hash;
    enum class ReasonKind : uint8_t {
        InvalidManifest,
        MissingPayload,
        PayloadHashMismatch,
        InvalidCanonicalBytes,
    };
    ReasonKind reason_kind;
};

struct BaseArtifactMissing {
    Blake3Hash base_manifest_hash;
};

struct ReplaceConflict {
    uint64_t live_head_generation;
    std::optional<Blake3Hash> live_manifest_hash;
    Blake3Hash incoming_manifest_hash;
};

struct PublishedSession {
    EvalSessionHandle session;
    uint64_t snapshot_generation;
    uint64_t head_generation;
};

struct HeadInstallResult {
    uint64_t snapshot_generation;
    uint64_t head_generation;
    Blake3Hash semantic_recovery_identity;
    std::optional<Blake3Hash> compiled_recovery_identity;
};

struct IngestSummary {
    Blake3Hash manifest_hash;
    uint64_t semantic_rows_inserted;
    uint64_t normalization_rows_inserted;
    bool already_present;
};

struct InstalledArtifact {
    HeadInstallResult head_install;
    std::optional<EvalSessionHandle> session_after_install;
};

struct IngestedOnly {
    IngestSummary summary;
};

using PublishFailure = std::variant<
    RetryableHeadConflict,
    AuthorityBusy,
    PublishValidationRejected,
    AuthorityCorruption>;

using InstallFailure = std::variant<
    ArtifactInvalid,
    BaseArtifactMissing,
    ReplaceConflict,
    AuthorityBusy,
    AuthorityCorruption>;

using PublishResult = std::variant<PublishedSession, PublishFailure>;
using InstallResult = std::variant<InstalledArtifact, IngestedOnly, InstallFailure>;
```

The intent is not to freeze every field name now.
The intent is to prevent the implementation from drifting back into untyped
`bool`/`optional`/exception-only control flow once coding starts.

The intended style is:

- small named structs with explicit fields for the branch-specific facts the
  caller actually needs;
- `std::variant` over those structs;
- no generic catch-all error string as the primary control-flow carrier.

### Step 1: Semantic-Key Schema Rewrite

- replace row-ID identity with semantic hash keys in the live schema
- eliminate evaluator-visible dependence on `TraceId`, `ResultId`,
  `DepKeySetId`, `NodeStamp`
- collapse semantic authority into one SQLite DB
- rewrite these files together:
  - `src/libexpr/include/nix/expr/eval-trace/store/trace-store.hh`
  - `src/libexpr/eval-trace/store/trace-store-lifecycle.cc`
  - `src/libexpr/eval-trace/store/trace-store.cc`
- concrete schema rewrite obligations:
  - `binding_versions`, `runtime_root_versions`, `eval_heads`,
    `candidate_summaries`, and normalization tables replace the current
    row-ID/current-node schema
  - long-lived semantic caches and indexes stop keying on `TraceId`,
    `ResultId`, and `DepKeySetId`

### Step 2: Evaluator Seam Rewrite

- keep `TraceBackend` as the evaluator-facing seam
- rewrite `StoreTraceBackend`, `TraceSession`, `TracedExpr`, `PrefetchPool`,
  and related code to use semantic replay identity and bound SQLite snapshots
- rewrite these files together:
  - `src/libexpr/include/nix/expr/eval-trace/cache/trace-backend.hh`
  - `src/libexpr/include/nix/expr/eval-trace/cache/trace-session.hh`
  - `src/libexpr/eval-trace/cache/trace-session.cc`
  - `src/libexpr/eval-trace/context.cc`
  - `src/libexpr/eval-trace/cache/materialize.cc`
  - `src/libexpr/eval-trace/cache/traced-expr.hh`
  - `src/libexpr/eval-trace/cache/prefetch-pool.hh`
- concrete seam changes:
  - `verify(...)` returns semantic `VerifyHit`
  - `record(...)` returns replacement bound-session state or semantic
    `RecordHit`
  - replay loads by `trace_record_hash`, not `loadFullTrace(traceId)`
  - runtime-root publication stops bypassing the bound-session contract
  - public evaluator code only ever sees `BoundEvalSession`, never intermediate
    session bootstrap state

### Step 3: Verification / Recovery Rewrite

- replace row-ID verification entry points with path-key and semantic-trace
  entry points
- rewrite helper/protocol/test seams to stop using `TraceId` and
  `loadFullTrace(traceId)`
- rewrite these files together:
  - `src/libexpr/eval-trace/store/trace-store-verify.cc`
  - `src/libexpr/include/nix/expr/eval-trace/store/verification-session.hh`
  - delete by default (updated 2026-04-29; the first three were already
    removed in Bundle 4):
    - ~~`src/libexpr/eval-trace/store/trace-store-service.hh`~~ (gone)
    - ~~`src/libexpr/include/nix/expr/eval-trace/store/trace-store-protocol.hh`~~ (gone)
    - ~~`src/libexpr/eval-trace/store/verify-pipeline.hh`~~ (gone)
    - `src/libexpr/include/nix/expr/eval-trace/store/verification-protocol.hh`
    - `src/libexpr/eval-trace/store/verification-orchestrator.hh`
    - `src/libexpr/eval-trace/store/verification-orchestrator.cc`
- concrete seam changes:
  - `lookupCurrentNode(pathId)` becomes `lookupCurrentBinding(session, path_key)`
  - `scanHistoryForAttr` hot-path use becomes `candidate_summaries` lookups
  - `VerificationSession::verifiedTraceIds` and `traceContextMemo` move to
    semantic identities / logical generation pins
  - if any typed verification pipeline survives after profiling, it carries
    semantic replay identity only and must justify its existence independently
    of the legacy service layer

### Step 4: Canonical Export / Import Layer

- define canonical export packages and manifest hashing
- implement import validation, set-union ingest, and transactional head publish
- wrap exported artifacts in ordinary Nix store closures for transport
- add or rewrite helper code around:
  - rewritten SQLite artifact export/import helpers under
    `src/libexpr/eval-trace/store/`
  - existing store accessors and CA store-path helpers under `src/libstore/`
- concrete helper responsibilities:
  - encode canonical package bytes from one chosen head snapshot
  - wrap them in CA file store objects
  - materialize via ordinary `ensurePath(...)`
  - read bytes locally via `requireStoreObjectAccessor(...)`
  - ingest immutable rows and then optionally install heads
- implementation touch points should include:
  - rewritten store-core export/import helpers under
    `src/libexpr/eval-trace/store/`
  - `src/libexpr/include/nix/expr/eval-trace/store/trace-store.hh`
  - `src/libexpr/eval-trace/store/trace-store-lifecycle.cc`
  - `src/libexpr/eval-trace/store/trace-store.cc`
  - `src/libstore/build/entry-points.cc`
  - `src/libstore/include/nix/store/store-api.hh`
  - `src/libstore/store-api.cc`
- completion rule:
  - no export/import path still depends on live row IDs, attached semantic DBs,
    or raw `.sqlite` transport
  - public import/export entry points expose artifact objects and bound-session
    results, not partially validated staging states

### Step 4A: Bound Session / Runtime-Root Rewrite

- collapse session-open, runtime-root hydration, and evaluator bind into one
  bound-session path over one SQLite read snapshot
- ensure same-session runtime-root visibility remains immediate even though the
  durable authority is SQLite
- rewrite these files together:
  - `src/libexpr/include/nix/expr/eval-trace/cache/trace-session.hh`
  - `src/libexpr/eval-trace/cache/trace-session.cc`
  - `src/libexpr/eval-trace/context.cc`
- completion rule:
  - no public split path remains where runtime roots are separately merged into
    a registry and separately recorded to storage outside the bound-session
    update path
  - runtime-root publication consumes a validated runtime-root capability
    produced by the bootstrap/fetch path, not raw caller-provided tuples

### Step 5: Optional Derived Acceleration Rebuild

- make candidate summaries, structural profiles, and compiled recovery
  validation/rebuild explicit
- keep eager maintenance only for exact/direct/GitIdentity summaries unless
  profiling justifies widening the writer critical section
- rewrite or add helper code around:
  - `src/libexpr/eval-trace/store/trace-store.cc`
  - `src/libexpr/eval-trace/store/trace-store-verify.cc`
  - any new rebuild/maintenance helper files introduced under
    `src/libexpr/eval-trace/store/`
- completion rule:
  - dropping all derived acceleration must still leave one correct semantic
    recovery path, even if slower

### Step 6: Test And Coverage Rewrite

- rewrite helper, fixture, and protocol tests so they no longer assert on row
  IDs or row-ID-shaped replay/result carriers
- make store/verify tests assert on semantic hashes, deterministic candidate
  ordering, and generation-based head movement
- add canonical export/import determinism tests
- rewrite these test seams together:
  - `src/libexpr-tests/eval-trace/helpers.hh`
  - `src/libexpr-tests/eval-trace/store/verification-protocol.cc`
  - store/verify fixture coverage under `src/libexpr-tests/eval-trace/`
- completion rule:
  - no test-only helper remains as a hidden legacy row-ID seam after the
    production rewrite

## Validation Targets

1. one local record transaction updates current state and history coherently
2. one bound read/verify/recovery operation sees one stable evaluator view for
   its pinned logical generation
3. exact-hit parity with the current implementation
4. bootstrap/GitIdentity/direct-hash/structural recovery parity in terms of
   validity/rejection, accepting the explicit deterministic winner rule
5. immutable semantic rows merge by set union without semantic loss
6. conflicting head imports are explicit same-root/no-op, replace, or conflict
7. canonical export bytes are deterministic for logically identical semantic
   state
8. import of a canonical export reconstructs equivalent local semantic state
9. runtime-root hydration and same-session visibility remain correct
10. compiled-recovery publication remains outside ordinary evaluator-facing
    write paths
11. export from one chosen head row reads one coherent SQLite snapshot rather
    than mixed transactions
12. import ingest does not silently install or advance evaluator-visible heads
13. derived acceleration rows can be dropped and rebuilt without changing
    semantic answers
14. store-closure wrapped exports round-trip through substituter-style transport
    without changing canonical package bytes
15. wrapper root manifests name the complete payload closure directly, so
    `ensurePath(root)` plus `queryPathInfo(root)` is sufficient to validate the
    fetched payload set before local install, provided payload objects are
    verified as leaf file objects with empty refs
16. remote substitution hits do not move local evaluator heads until explicit
    install runs
17. remote substitution misses are treated only as transport misses, not as
    authoritative semantic negative answers
18. imported canonical bytes never depend on foreign/local surrogate IDs from
    normalization tables; local staging resolves those IDs deterministically
19. chunked immutable-row ingest converges to the same authority row set as
    one-shot ingest and still leaves head movement to the final explicit
    install transaction
20. concurrent imports of the same artifact converge by immutable-row union and
    same-target/no-op head install
21. delta transport is only accepted when the named base manifest identity is
    already present locally; otherwise install falls back to the full canonical
    package closure
22. exact current-hit lookup resolves by one indexed `binding_versions` query
    against the pinned logical generation and does not scan full history
23. GitIdentity/direct-hash recovery hot paths read from `candidate_summaries`
    or an equivalent validated summary path rather than joining full immutable
    history on every ordinary attempt
24. evaluator-facing helper APIs and replay/result carriers no longer expose
    `TraceId` / `ResultId` / `DepKeySetId`
25. export/import helper paths use ordinary local store materialization and
    local store-object accessors rather than a custom substituter RPC
26. semantic helper signatures and artifact/session identities remain nominally
    typed rather than collapsing back to raw untagged hashes or strings
27. no import/install helper path allows head movement from unvalidated bytes
    or from a caller-forged bound-session/session-install capability
28. no eval-time helper path reintroduces raw `future.get()` / untyped blocking
    outside the existing `syncAwait` discipline
29. internal import/install phases are linearly consumed or equivalently
    typestated so validated/ingested/installable states cannot be skipped or
    reused out of order
30. public session-open/runtime-root APIs expose only opaque bound/validated
    capabilities, not forgeable structs of raw fields
31. the legacy async service/protocol layer is deleted by default; if any part
    survives, it carries semantic identities only and has a non-compatibility
    justification
32. pruning superseded snapshot generations does not break older readers that
    still hold live logical generation leases
33. partial immutable-row ingest without head install is safe to retry and
    does not create evaluator-visible head movement
34. mutating helper failures are typed strongly enough to distinguish
    retryable head contention from validation failure and fatal corruption
35. runtime-root reverse overlays are updated only after the durable publish
    transaction succeeds
36. bound evaluator operations hold only short read transactions and do not
    pin one long-lived SQLite reader for the full wall-clock duration of eval
37. authority writer transactions exclude hashing, substitution, artifact
    validation, structured parsing, and other long-running phase-A work
38. normalization/interning rows and semantic rows commit atomically in the
    same DB, eliminating the cross-DB vocab crash gap of the current topology
39. attr-name/path, string, data-path, and dir-set interning materially reduce
    repeated storage and repeated hot-path lookup work relative to a naive
    denormalized schema
40. concurrent insertion of the same canonical interned value or trie edge
    converges to one local surrogate mapping and one semantic export view
41. ordinary evaluator-facing publish failure is all-or-nothing and never
    leaves a visible partially advanced head
42. normalization resolution converges by unique-key insert-or-select semantics
    rather than process-local surrogate-ID preallocation
43. hybrid normalization warmup preserves hot attr-vocab/path locality without
    requiring every process to full-scan large string/data-path tables at
    startup
44. normalization convergence does not depend on `last_insert_rowid()` or
    SQLite `RETURNING`; unique-key insert-or-select remains the authoritative
    rule under concurrent writers
45. busy-retry helpers wrap only the short SQLite transaction/statement
    critical section, not the expensive phase-A computation outside it
46. `attr_paths` interning is keyed authoritatively by
    `(parent_path_id, child_name_id)`; any stored `path_digest` remains a
    secondary derived/indexed column rather than the primary local interning
    key
47. per-connection normalization helper statement bundles are reused rather
    than prepared ad hoc on every hot-path lookup
48. combined install helpers may report typed `IngestedOnly` success without
    implying evaluator-visible head movement
49. large immutable payload tables use local surrogate row IDs plus `UNIQUE`
    semantic-hash columns, while associative/version/head tables use
    composite semantic primary keys and `WITHOUT ROWID` by default
50. semantic digests and semantic keys are stored in canonical binary form
    rather than widened hex/base32 text unless a specific interoperability
    boundary requires text
51. V1 does not eagerly full-scan `strings`, `data_paths`, or `dir_sets` at
    process startup by default; any eager warmup beyond attr vocab must be
    explicitly bounded or profiling-justified
52. rewritten authority tables are `STRICT` by default unless a specific
    SQLite feature requires a documented exception
53. the rewritten authority schema follows the preferred physical split:
    local-surrogate normalization tables, local-surrogate immutable payload
    tables with `UNIQUE` semantic hashes, and composite-key `WITHOUT ROWID`
    associative/version/head tables
54. hot statement catalogs are grouped by connection role rather than hidden in
    ad hoc SQL callsites or one undifferentiated global bundle
55. `PublishResult` / `InstallResult` remain explicit `std::variant` carriers
    with named branch structs rather than collapsing into `bool`,
    `optional`, or exception-only control flow
56. rewritten authority DDL enables foreign-key enforcement and uses explicit
    `CHECK` constraints for boolean-like and enum-like columns where practical
57. root sentinels for attr-path and data-path tries remain explicit storage
    invariants rather than being recreated informally in process-local memory
58. concrete per-role statement-bundle structs remain visible in the C++ store
    core instead of collapsing into one global statement bag
59. helper/type naming distinguishes row-shape storage structs from semantic
    capability/result carriers rather than mixing table names into the public
    evaluator-facing surface
60. head/version table nullability and enum/boolean domains are explicit, so
    impossible combinations are rejected by SQLite rather than left implicit
61. raw prepared-statement ownership stays in the rewritten store core
    lifecycle/schema layer rather than leaking into evaluator/backend files
62. named hot statements keep the intended indexed SQL shape
    (`ORDER BY ... DESC LIMIT 1`, CAS `UPDATE ... WHERE head_generation = ?`,
    candidate summary lookups keyed by their declared recovery indexes)
63. `candidate_summaries` carries the replay-critical result kernel in typed
    columns (`result_kind`, `encoding_version`, `payload`, `aux_context`)
    rather than one opaque summary blob
64. `canonical_manifests` remains bookkeeping for canonical package identity
    and local install/export state, not a second semantic authority layer
65. the rewritten store core keeps schema/statement ownership in
    `trace-store.hh` / `trace-store-lifecycle.cc` and algorithm ownership in
    `trace-store.cc` / `trace-store-verify.cc` by default, unless a purely
    private implementation split is justified
66. the rewrite keeps the current public store-core file anchors by default
    (`trace-store.hh`, `trace-store-lifecycle.cc`, `trace-store.cc`,
    `trace-store-verify.cc`); any extra files are private implementation
    splits, not new public compatibility layers
67. tombstone/version rows use `CHECK` constraints so replay or runtime-root
    payload columns are either fully present or intentionally absent according
    to the tombstone bit
68. the second-tier named statements for runtime-root lookup, manifest
    bookkeeping, export scans, and pruning are also fixed to explicit SQL
    shapes rather than left as conceptual placeholders
69. canonical export/import helper ownership stays in the rewritten
    `src/libexpr/eval-trace/store/` layer, with store API integration only at
    the outer transport boundary in `src/libstore/`
70. export scans use a fixed temporary workset table plus stable join queries,
    not ad hoc dynamically generated `IN (...)` SQL over semantic hashes
71. prepared-statement bundle construction itself is centralized in explicit
    lifecycle helper functions rather than open-coded across the store core

## Recommendation

Choose this plan if the priority is:

- reducing custom implementation;
- preserving a strong local transactional authority;
- keeping SQL as the hot-path engine for exact-hit and recovery;
- accepting that portable semantic identity is derived at export time rather
  than identical to the live local store.

Do not choose this plan if the priority is:

- making live semantic truth identical to one content-addressed root;
- making local/live/exported identity all the same object model;
- building first-class commutative union semantics for live heads.
