# Eval Cache Research Track 2: SQLite-Centered Semantic Authority

Date: 2026-04-09

This note answers the question the main proposal leaves open:
what would it actually take for SQLite to remain the primary semantic authority
for the eval-cache / eval-trace system?

Baseline:

- the current note rejects SQLite as semantic truth for order-invariant state:
  [research/eval-trace-order-invariant-persistence-notes.md](research/eval-trace-order-invariant-persistence-notes.md)
- the live implementation still relies on SQLite `Sessions`, `History`,
  `Traces`, `DepKeySets`, and `Results` with recency-biased recovery ordering:
  [src/libexpr/eval-trace/store/trace-store-lifecycle.cc](src/libexpr/eval-trace/store/trace-store-lifecycle.cc)
  and [src/libexpr/eval-trace/store/trace-store-verify.cc](src/libexpr/eval-trace/store/trace-store-verify.cc)

The question here is not "can SQLite store the bytes?".
It obviously can.
The question is whether SQLite can honestly buy:

1. deterministic semantic identity;
2. coherent snapshot/recovery heads;
3. portable export/import over the network;
4. union/merge semantics that are not secretly history-order dependent;
5. exact-hit and constructive recovery indexes;
6. safe multi-process/threaded access.

My conclusion:

- SQLite can be a good local transactional authority for the mutable view of
  eval state.
- SQLite does not, by itself, give portable deterministic semantic identity.
  That identity must either be reconstructed above SQLite or moved into a
  content-addressed export layer.
- SQLite's native replication/delta tools are row-change tools, not semantic
  union/CRDT tools.
- If SQLite remains authoritative, the best credible design is:
  one mutable authority DB for local operation, plus an explicit canonical
  export layer that re-materializes deterministic semantic objects.

## Required Shape If SQLite Stays Authoritative

If SQLite is the authority, the schema has to stop treating row ids and commit
order as semantic facts.

At minimum, the authority DB needs explicit semantic keys:

```sql
CREATE TABLE results (
  result_hash BLOB PRIMARY KEY,
  result_kind INTEGER NOT NULL,
  encoding_version INTEGER NOT NULL,
  payload BLOB NOT NULL,
  aux_context BLOB
) WITHOUT ROWID;

CREATE TABLE dep_key_sets (
  struct_hash BLOB PRIMARY KEY,
  keys_blob BLOB NOT NULL
) WITHOUT ROWID;

CREATE TABLE traces (
  trace_hash BLOB PRIMARY KEY,
  struct_hash BLOB NOT NULL REFERENCES dep_key_sets(struct_hash),
  values_blob BLOB NOT NULL
) WITHOUT ROWID;

CREATE TABLE current_bindings (
  semantic_session_key BLOB NOT NULL,
  attr_path_key BLOB NOT NULL,
  trace_hash BLOB NOT NULL REFERENCES traces(trace_hash),
  result_hash BLOB NOT NULL REFERENCES results(result_hash),
  generation_token BLOB NOT NULL,
  PRIMARY KEY (semantic_session_key, attr_path_key)
) WITHOUT ROWID;

CREATE TABLE history_members (
  stable_recovery_key BLOB NOT NULL,
  attr_path_key BLOB NOT NULL,
  trace_hash BLOB NOT NULL REFERENCES traces(trace_hash),
  result_hash BLOB NOT NULL REFERENCES results(result_hash),
  git_identity_hash BLOB,
  PRIMARY KEY (stable_recovery_key, attr_path_key, trace_hash)
) WITHOUT ROWID;

CREATE INDEX history_git_identity
  ON history_members(stable_recovery_key, attr_path_key, git_identity_hash);

CREATE INDEX history_by_struct
  ON history_members(stable_recovery_key, attr_path_key, trace_hash);
```

That still is not enough.
You also need explicit rules that the DB will not infer for you:

- no semantic dependence on `rowid`, insertion order, or `AUTOINCREMENT`;
- deterministic winner policy when more than one history member matches;
- deterministic encoding for exported semantic objects;
- explicit coherence object for "snapshot head + recovery head + optional
  compiled attachments".

Without those rules, SQLite gives transactional state, not order-invariant
semantic identity.

## What SQLite Actually Buys

### Local coherence and current-head updates

A single SQLite database can atomically update:

- current binding rows;
- history membership rows;
- runtime-root rows;
- local compiled/cache rows.

This is the strongest argument for a SQLite-centered design.
If everything lives in one DB file, one transaction gives one coherent local
head update.

### Good exact-hit indexes

SQLite is perfectly comfortable serving:

- exact current binding lookup:
  `(semantic_session_key, attr_path_key) -> (trace_hash, result_hash)`
- exact history membership by full trace hash:
  `(stable_recovery_key, attr_path_key, trace_hash)`
- exact GitIdentity shortcut:
  `(stable_recovery_key, attr_path_key, git_identity_hash)`
- structural candidate scans:
  all `trace_hash` values for one `(stable_recovery_key, attr_path_key)`.

For the current verifier/recovery flow, this is already close to the live
implementation.

### Stable local snapshots for readers

In WAL mode, readers get snapshot isolation and can run concurrently with one
writer.
That is enough for "open read view, verify against stable head, then release".

### Simple local maintenance and backup

SQLite has practical tooling for:

- online snapshots via backup API;
- compact snapshots via `VACUUM INTO`;
- serialization into a byte image via `sqlite3_serialize()`.

Those are useful for local maintenance and point-in-time copy-out.

## What SQLite Does Not Buy By Itself

### Deterministic semantic identity

A SQLite database file is not a canonical semantic object.
Even if the logical rows are the same, file bytes can differ because of:

- page layout;
- freelist state;
- checkpoint timing;
- vacuum timing;
- row placement;
- attached-db topology;
- schema-level representation choices.

`sqlite3_serialize()` is just the database image.
`VACUUM INTO` gives a consistent compact copy of the current DB, not a semantic
Merkle root.
Backup API gives a snapshot copy, not canonical semantic identity.

So if the user asks "what is the portable identity of this snapshot/history
state?", SQLite alone does not answer.
You must build that identity above the DB.

### Union semantics

Set union is easy only for append-only content-addressed tables like:

- `results`
- `dep_key_sets`
- `traces`
- `history_members`

Current heads are different:

- `current_bindings`
- `eval_heads`
- `runtime_roots` when last-writer-wins matters

Those are not union objects.
They require:

- CAS;
- epoch/order tokens;
- explicit winner policy;
- or an MV-register style "keep multiple candidates and resolve on read".

SQLite change tooling does not solve this semantically.
It only transports row mutations.

### Real CRDT behavior

The only realistic CRDT story in SQLite is selective:

- treat content-addressed object tables as immutable grow-only sets;
- treat history membership as a grow-only OR-set keyed by semantic hashes;
- treat current heads as derived, not replicated state.

If current heads themselves are replicated as mutable rows, you are back in
conflict-resolution land, not CRDT land.

So a "SQLite CRDT design" is really:

- CRDT-like for immutable object membership;
- custom deterministic projection for current heads.

That is workable, but it is not native SQLite behavior.

### Cross-host live sharing

WAL is a local-host concurrency mechanism, not a network semantic protocol.
If SQLite remains authoritative and hosts exchange state, they need:

- exported DB snapshots;
- changesets;
- custom manifests;
- or canonical semantic packages derived from the DB.

The live DB is not itself the portable protocol.

## SQLite Feature Assessment

### WAL

Buys:

- concurrent readers with one writer;
- stable read snapshots;
- good local latency.

Fails to buy:

- multi-host shared authority;
- multi-DB atomicity across attached databases;
- deterministic identity.

Important limit:

- if you split semantic authority across attached DBs, WAL loses crash-atomic
  all-or-nothing semantics across the set.

Verdict:

- good for one authoritative DB file on one machine;
- bad as the basis for a multi-file semantic topology.

### Immutable mode

Buys:

- fast read-only open of a frozen exported snapshot;
- no locking or change detection overhead.

Fails to buy:

- trust.
  The caller must already know the file is immutable and correct.

Verdict:

- useful for imported frozen snapshots only;
- not a publication or authority mechanism.

### Backup API

Buys:

- online consistent copy of a live DB;
- incremental copy.

Fails to buy:

- canonical export bytes;
- semantic closure manifest;
- merge semantics.

Verdict:

- good operational snapshot tool;
- not a semantic export format.

### `sqlite3_serialize()` / `sqlite3_deserialize()`

Buys:

- move a DB image through memory;
- embed a DB snapshot inside another artifact;
- cheap handoff to read-only consumers.

Fails to buy:

- canonical identity.
  It serializes the database image, not the abstract semantic set.

Verdict:

- useful transport for "exact DB snapshot";
- wrong abstraction if exported identity must survive re-import/rebuild.

### `VACUUM INTO`

Buys:

- compact consistent snapshot;
- deleted-page cleanup;
- one-file export.

Fails to buy:

- canonical semantic digest;
- merge/union;
- stable identity across logically equivalent DB histories.

Verdict:

- better than raw file copy for distribution;
- still a DB-image export, not a semantic manifest.

### Session / changeset / patchset / changegroup / rebase

Buys:

- capture row deltas;
- apply them transactionally;
- combine row changes by primary key;
- rebase local row changes against remote-applied row changes.

Fails to buy:

- semantic merge of eval-cache meaning;
- deterministic recovery semantics;
- CRDT current-head convergence without custom policy.

Key constraint:

- the merge unit is "row identified by PRIMARY KEY", not "semantic object set
  plus coherent head projection".

Verdict:

- plausible for shipping append-only table deltas or staging imports;
- not enough for semantic authority unless the schema is redesigned so that the
  only replicated rows are immutable objects and grow-only memberships.

### RBU

Buys:

- resumable bulk ingest;
- efficient large updates;
- background reads during ingest.

Fails to buy:

- normal multi-writer semantic authority;
- WAL target support;
- merge semantics.

Verdict:

- good maintenance/import primitive for large snapshot ingest;
- not the live authority protocol.

### ATTACH

Buys:

- one connection, multiple logical databases;
- convenient schema separation.

Fails to buy:

- cross-DB crash atomicity in WAL mode.

Verdict:

- acceptable only if all semantic authority is in one DB or rollback journal is
  acceptable;
- a trap if used to recreate the current attached-database topology under WAL.

## Export / Import Strategies

If SQLite remains primary, there are only three honest identity stories.

### 1. DB-image identity

Export the SQLite image directly via:

- file copy;
- backup API;
- `VACUUM INTO`;
- `sqlite3_serialize()`.

What it buys:

- simple exact snapshot export;
- cheap import;
- local tooling reuse.

What it fails to buy:

- semantic identity independent of page layout and history;
- dedup across logically equivalent DBs;
- content-addressed closure semantics.

Identity status:

- portable identity is the DB image, not the semantic state.

### 2. Canonical dump / manifest identity

Export a custom canonical object:

- sorted semantic rows from core tables;
- canonical binary encodings of rows;
- explicit root/head manifest.

Example export unit:

- `results/` keyed by `result_hash`
- `dep_key_sets/` keyed by `struct_hash`
- `traces/` keyed by `trace_hash`
- `history_members/` keyed by `(stable_recovery_key, attr_path_key, trace_hash)`
- `current_heads/` keyed by `(semantic_session_key, stable_recovery_key)`
- top-level canonical manifest hashing all of the above in sorted order

What it buys:

- real deterministic portable identity;
- Nix-substitutable content-addressed exports;
- stable union logic at the export-object level.

What it fails to buy:

- a native SQLite answer.
  This is an extra semantic layer above SQLite.

Identity status:

- deterministic and portable, but reconstructed during export.

### 3. Content-addressed tables inside SQLite

Store canonical objects as rows whose keys are already content hashes, and make
export a sorted projection of those tables.

This improves matters, but current heads still remain mutable rows.

What it buys:

- immutable object tables with natural dedup;
- better import union for core objects;
- smaller semantic gap between live store and export.

What it fails to buy:

- native canonical current-head identity;
- lock-free union of mutable heads.

Identity status:

- real for object tables;
- reconstructed for coherent current state.

## Union / Merge Stories

### Content-addressed tables

These can merge by set union safely:

- `results`
- `dep_key_sets`
- `traces`
- `history_members`

This is the strongest SQLite-friendly part of the design.

### Mutable heads

These cannot merge by set union:

- `current_bindings`
- `eval_heads`
- compiled-attachment pointers

You need one of:

1. last-writer-wins with explicit clock or origin ordering;
2. CAS plus retry at import time;
3. MV-register rows plus deterministic read projection;
4. event-log fold into derived heads.

The main proposal rejects hidden recency semantics.
So any SQLite-centered design must make this winner policy explicit.

### Practical CRDT answer

The realistic CRDT-ish design is:

- immutable object rows are the replicated CRDT-like payload;
- head rows are rebuilt or deterministically projected after import;
- import may refuse to auto-advance conflicting heads unless the conflict is
  semantically identical.

That is a custom semantic protocol using SQLite as storage.
It is not a property SQLite gives for free.

## Exact-Hit And Constructive Recovery Index Design

For parity with the current design, a SQLite authority should expose:

- exact current hit:
  `PRIMARY KEY (semantic_session_key, attr_path_key)` on `current_bindings`
- exact historical hit:
  `PRIMARY KEY (stable_recovery_key, attr_path_key, trace_hash)` on
  `history_members`
- GitIdentity shortcut:
  `(stable_recovery_key, attr_path_key, git_identity_hash)`
- structural candidate enumeration:
  `history_members` joined to `traces(struct_hash)`
- optional grouped candidate table:

```sql
CREATE TABLE recovery_candidates (
  stable_recovery_key BLOB NOT NULL,
  attr_path_key BLOB NOT NULL,
  struct_hash BLOB NOT NULL,
  trace_hash BLOB NOT NULL,
  PRIMARY KEY (stable_recovery_key, attr_path_key, struct_hash, trace_hash)
) WITHOUT ROWID;
```

This gives:

- O(1) current lookup;
- O(1) full-trace membership lookup;
- O(1) GitIdentity lookup;
- bounded structural scan by `(stable_recovery_key, attr_path_key, struct_hash)`.

But deterministic recovery still needs custom logic.
If multiple traces match, SQLite can return rows quickly, but it cannot tell you
the semantic winner unless you define one.

## Multi-Process / Thread Safety

For one DB file on one host, the sane operational rule is:

- WAL mode;
- separate connection per thread or per worker;
- serialized or disciplined multi-thread connection usage;
- one semantic authority DB, not several attached authority DBs.

For read-only imported snapshots:

- copy out a frozen DB image;
- open with `immutable=1` if the caller has already verified immutability.

For large imports:

- use backup API, `VACUUM INTO`, or RBU as operational tools;
- then validate or rebuild semantic heads after ingest.

For network transport:

- never pretend live SQLite WAL/shm state is the network protocol;
- export a DB image or a canonical manifest;
- import into a local DB and re-establish local WAL state there.

## Option Comparison

### Option A: SQLite for everything

Shape:

- one authority DB file;
- semantic objects, current heads, history, vocab, stat-hash, compiled
  accelerators all in SQLite.

Buys:

- simplest local transactional model;
- easiest exact-hit and recovery indexing;
- one coherent local commit boundary.

Fails versus the current proposal:

- no native deterministic portable identity;
- export/import identity is DB-image identity unless custom export is added;
- no native root/pin/GC model;
- merge semantics for mutable heads remain custom.

Custom logic required:

- canonical semantic export manifest;
- deterministic winner policy for recovery and imports;
- head CAS/import conflict policy;
- optional GC / compaction semantics if DB-image exports must dedup by content.

Identity status:

- live identity is mutable DB state;
- deterministic portable identity is reconstructed after export.

### Option B: SQLite mutable authority plus content-addressed export layer

Shape:

- local authority remains one SQLite DB;
- export builds canonical semantic objects or manifests keyed by content hash.

Buys:

- preserves SQLite ergonomics locally;
- gives real portable identity at export boundary;
- makes network transfer and Nix substitution plausible.

Fails versus the current proposal:

- local authority is still not history-invariant by construction;
- exact current-head identity still depends on mutable SQL rows until export;
- imports must translate canonical objects back into mutable heads.

Custom logic required:

- full export/import codec;
- canonical row ordering and manifest hashing;
- explicit coherence object for exported current state;
- deterministic head rebuild on import.

Identity status:

- real and portable in the export layer;
- reconstructed from mutable authority.

### Option C: SQLite shards / union databases

Shape:

- multiple DBs, for example per session, per recovery key, or per namespace;
- union/import combines them into a logical whole.

Buys:

- smaller contention domains;
- simpler selective export of shards;
- some resemblance to content-addressed namespace splitting.

Fails versus the current proposal:

- attached-DB atomicity under WAL is not sufficient for coherent multi-shard
  semantic commits;
- cross-shard coherence object becomes custom;
- union/merge logic becomes harder, not easier.

Custom logic required:

- manifest of shard set and coherent head;
- import-time reconciliation;
- cross-shard CAS or derived-head projection.

Identity status:

- deterministic identity is reconstructed from a canonical shard manifest, not
  native to SQLite.

### Option D: Append-only / event-log in SQLite

Shape:

- SQLite stores immutable events:
  record trace, publish head, add history member, install runtime root;
- current state is a deterministic fold or periodically materialized snapshot.

Buys:

- history is explicit and import/union can become append-union;
- head conflicts can be modeled as event conflicts instead of row overwrites;
- easier auditability.

Fails versus the current proposal:

- read amplification unless snapshots/materialized views are added;
- deterministic fold semantics become the real protocol;
- exported identity is still not the raw DB image.

Custom logic required:

- fold engine;
- snapshotting/materialization rules;
- log compaction;
- deterministic conflict resolution for concurrent head-publish events.

Identity status:

- real identity comes from canonical folded state or canonical event stream
  export, not from the mutable DB file.

## Bottom Line

If SQLite is kept, the honest design is not "SQLite magically solves semantic
identity".
It is:

1. SQLite is the local mutable authority for operational state.
2. Semantic objects are keyed by explicit content hashes inside that DB.
3. Current heads use explicit CAS/winner rules, never row order.
4. Export/import uses a canonical manifest or content-addressed package above
   SQLite.
5. Union is set-union for immutable object rows and custom deterministic
   projection for heads.

That means SQLite can replace the current row-id service topology without
forcing the full block/ref/pin engine, but only if the project accepts a split:

- local authority is mutable SQL;
- portable deterministic identity exists only in a derived semantic export
  layer, or is reconstructed from SQL rows at open/import time.

That is materially weaker than the main proposal's "semantic identity is the
live root".
It is materially stronger than the current implementation.

## Sources

- Main proposal:
  repo-local file:
  `research/eval-trace-order-invariant-persistence-notes.md`
- Live schema and lookup/recovery paths:
  `src/libexpr/eval-trace/store/trace-store-lifecycle.cc`
  `src/libexpr/eval-trace/store/trace-store.cc`
  `src/libexpr/eval-trace/store/trace-store-verify.cc`
- SQLite WAL:
  <https://sqlite.org/wal.html>
- SQLite isolation:
  <https://sqlite.org/isolation.html>
- SQLite URI / immutable mode:
  <https://sqlite.org/uri.html>
- SQLite backup API:
  <https://sqlite.org/backup.html>
- SQLite serialize API:
  <https://sqlite.org/c3ref/serialize.html>
- SQLite ATTACH:
  <https://sqlite.org/lang_attach.html>
- SQLite VACUUM / VACUUM INTO:
  <https://sqlite.org/lang_vacuum.html>
- SQLite session / changeset / changegroup / rebase:
  <https://sqlite.org/session.html>
  <https://sqlite.org/sessionintro.html>
- SQLite RBU:
  <https://sqlite.org/rbu.html>
- SQLite threading modes:
  <https://sqlite.org/threadsafe.html>
- SQLite snapshot API:
  <https://sqlite.org/c3ref/snapshot.html>
