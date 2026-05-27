# Eval-trace SQLite writeback findings

This is a checkpoint for the SQLite eval-trace refactor and writeback-performance investigation.

## Replacement task list

1. Define a content-addressed trace capsule format with scoped dense ids.
2. Add adversarial round-trip, corruption, and conflict tests for capsules and projections.
3. Replace normalized SQLite payload serving with `TraceCapsules` plus candidate/head/event control-plane tables.
4. Rebuild, run `eval-trace-bench`, and compare the new run against the normalized-schema run and `92a3df1ab706cf514357ae57b367050b373ff146`.

Design rule: integer ids remain a performance representation, but their scope is explicit. `Capsule*Id` values are dense ids inside one serialized object. `Process*Id` values live only in the current evaluator. SQLite owns only the transactional control-plane and content-hash indexes.

## Current architecture

SQLite is used only at process boundaries:

1. Startup opens the DB, applies the schema, bulk-loads canonical rows into memory, and closes the connection.
2. Shutdown opens the DB, performs one set-oriented reconciliation transaction, and closes the connection.

Runtime eval/verify/record paths mutate only in-memory state. The process owns local IDs while running; SQLite owns canonical identity. Shutdown writes local rows through temporary staging tables and `map_*` crosswalk tables, then resolves conflicts through SQLite `UNIQUE` constraints and `INSERT OR IGNORE` / lookup joins.

References for the pattern:

- SQLite UPSERT / uniqueness-driven reconciliation: https://sqlite.org/lang_upsert.html
- SQLite transaction writer boundary: https://www.sqlite.org/lang_transaction.html
- SQLite statement status counters: https://sqlite.org/c3ref/stmt_status.html
- SQLite temporary sort b-trees / query-plan diagnostics: https://sqlite.org/eqp.html#temporary_sorting_b_trees
- Kimball surrogate-key pipeline: https://www.kimballgroup.com/1998/05/surrogate-keys/

## Implemented changes in this checkpoint

- Added SQLite statement-status instrumentation for shutdown writeback under `NIX_SHOW_STATS=1`:
  - statement count
  - fullscan steps
  - sort operations
  - automatic indexes
  - VM steps
- Tuned the SQLite boundary connection for the bulk-load/writeback shape:
  - larger page cache
  - disabled cache spilling during the writeback transaction
  - enabled mmap
  - `synchronous=NORMAL`
  - in-memory temp store
  - disabled WAL autocheckpointing for the short-lived writer
- Removed redundant string-domain marker tables from the schema. Domain meaning now lives in the referencing table/column; canonical byte identity lives in `Strings(value)`.
- Kept persistent composite relation tables as `WITHOUT ROWID` after benchmarking showed rowid variants regressed.
- Added replay timing counters used while profiling cold eval overhead.

## Benchmarks run

Workload:

```sh
NIX_SHOW_STATS=1 nix eval -f /home/connorbaker/nixpkgs/nixos/release.nix --json closures
```

Every SQLite schema benchmark cleared the eval-trace cache first so `CREATE TABLE IF NOT EXISTS` could not hide schema-shape changes.

### Temp `WITHOUT ROWID` experiment

Selected temporary staging tables were changed to `WITHOUT ROWID` where their primary key was the tuple identity.

Result: rejected.

- Instrumentation-only writeback: `1.834950s`, `56.19M` SQLite VM steps.
- Temp `WITHOUT ROWID` writeback: `1.847652s`, `63.35M` SQLite VM steps.

Conclusion: the temp-table `WITHOUT ROWID` trial increased VM work and did not improve wall time.

### Persistent `WITHOUT ROWID` experiments

The persistent relation/cache tables that still used `WITHOUT ROWID` were tested as ordinary rowid tables.

| Variant | Wall | Writeback | `tracesUs` | VM steps |
|---|---:|---:|---:|---:|
| Baseline persistent `WITHOUT ROWID` | `90.179s` | `1.835s` | `1.139s` | `56.19M` |
| All persistent relation tables as rowid | `90.015s` | `2.085s` | `1.390s` | `62.93M` |
| Only `Trace*Values` as rowid | `90.109s` | `2.342s` | `1.642s` | `62.92M` |
| Non-trace relation/cache tables as rowid | `90.234s` | `2.213s` | `1.421s` | `62.77M` |

Conclusion: persistent `WITHOUT ROWID` tables are not the bottleneck here. Converting them to rowid consistently made writeback slower.

## Subagent research summary

### Append-only provenance

The strongest remaining implementation target is provenance-aware writeback.

Invariant:

- Canonical tables are append-only.
- IDs loaded at startup are DB-origin and stable for this process lifetime.
- IDs allocated after startup are run-local and must still be reconciled through SQLite natural keys, because another process may append conflicting canonical IDs while this process is disconnected.

Recommended shape:

- Record startup max IDs per canonical table.
- Direct-map DB-origin IDs where `local_id <= startup_max_id`.
- Stage and reconcile only run-local rows where `local_id > startup_max_id`.
- Build `needed_*` sets from dirty candidates, dirty runtime roots, and run-local traces/results/dep keys, so old rows referenced by new rows are mapped but not restaged.
- Add map-count assertions before dependent inserts so missing direct maps fail closed.

Important edge cases:

- A dirty candidate may reference an old DB-origin trace/result/path; direct maps must cover those IDs.
- A run-local trace may resolve to a concurrently appended canonical trace by `full_hash`; SQL conflict resolution must still decide that mapping.
- A run-local data path may have a DB-origin parent; parent maps must be seeded before depth-ordered insertion.
- If canonical rows are manually deleted or rewritten, the provenance assumption is invalid; this should fail through FK/map assertions rather than silently dropping rows.

### `carray`

`carray` is useful for one-column atom feeds but is not a full replacement for staging.

Good targets:

- `Strings(value)`
- session/recovery key hashes
- NAR/git/digest/fetch hashes
- dirset hashes
- `needed_*` ID sets for direct DB-origin maps

Poor targets:

- `DataPaths`
- `DepKeys`
- `Results`
- `Traces`
- `Trace*Values`
- candidates/runtime roots

Implementation notes:

- Use `SQLITE_CARRAY_BLOB` / `struct iovec`, not TEXT, because Nix strings may contain NUL and non-UTF-8 bytes.
- Do not rely on the system SQLite build enabling `carray`; vendor/static-register it or capability-gate with fallback.
- Keep backing storage alive through statement finalization with an RAII wrapper.
- Consider a custom eponymous virtual table later for multi-column in-memory row spans if staging remains dominant.

References:

- `carray`: https://www.sqlite.org/carray.html
- `sqlite3_carray_bind`: https://sqlite.org/c3ref/carray_bind.html
- static extension strategy: https://www.sqlite.org/loadext.html#statically_linking_a_run_time_loadable_extension
- pointer-passing security: https://sqlite.org/bindptr.html
- rowid / AUTOINCREMENT behavior: https://www.sqlite.org/autoinc.html

## Next recommended work

Implement append-only provenance before `carray`.

Expected high-level plan:

1. Promote startup high-water marks to correctness-significant provenance.
2. Add missing result/trace high-water marks if needed.
3. Build `needed_*` local-ID sets from dirty/new rows.
4. Populate `map_*` with DB-origin direct maps and run-local reconciliation maps.
5. Stage only run-local canonical rows.
6. Add cardinality assertions for every required map before dependent inserts.
7. Benchmark against `closures` with fresh cache and warm-cache dirty-candidate cases.

## Append-only provenance direct-map checkpoint

Implemented a high-water-mark provenance split for writeback:

- Rows loaded from SQLite at startup are treated as DB-owned canonical ids.
- Rows allocated above the startup high-water mark are treated as run-local ids.
- Shutdown staging now skips DB-origin rows for strings, data paths, atom hash tables, fetch identities, dep keys, results, traces, and dir sets.
- Direct identity maps seed `map_*` tables for DB-origin ids, so dependent run-local rows can still join through the same SQL crosswalk pipeline.
- Map row-count assertions ensure every in-memory local id has a canonical id before dependent writes run.

This preserves the original design rule: canonicalization and conflict resolution still happen in SQLite through natural-key uniqueness plus UPSERT for run-local rows. The high-water path only avoids sending already-canonical rows back through staging.

Benchmark on `closures` with `NIX_SHOW_STATS=1`, `./result/bin/nix eval -f /home/connorbaker/nixpkgs/nixos/release.nix --json closures`:

- No trace: 45.172s, JSON sha256 `a301a379ed0fa92213849788627f3e8683cd8d7d75b10961011115b52326b93f`.
- Trace, cold cleared eval-trace cache: 90.162s, JSON sha256 matched no-trace.
- Cold writeback: 2.178647s total, 114 SQL statements, 62.8M VM steps, 1.98M fullscan steps.
- Prior cold baseline before direct-map seeding was about 1.834950s writeback, 98 SQL statements, 56.2M VM steps.
- Conclusion: direct-map seeding regresses first cold writeback because there are almost no DB-origin ids to skip and the seeded maps add SQL work.
- Warm traced run immediately after cold population: 2.388s total, no writeback transaction, JSON sha256 matched. This confirms the cache-hit path avoids shutdown writes when no new records are produced.

Build status:

- `nix build -L . --builders ''` compiled the changed SQLite/eval-trace code and passed functional tests, but failed in `nix-expr-tests-run` with 7 eval-trace property failures.
- Failing tests: `EvalTraceProperty_FormalsNoEllipsis.ExtraKeyChange_CacheHit`, `EvalTraceProperty_FormalsEllipsis.UnlistedKeyChange_CacheHit`, `EvalTraceProperty_DepCorrectness.AllTrackedSlots_Present`, `EvalTraceProperty_DepCorrectness.Annotation_Consistency`, `TracedDataTest.TracedJSON_GenericClosure_CacheHit`, `TracedDataTest.PointerEquality_FindAlongAttrPath_SameCommandPath`, `SessionKeyDeterminismTest.SessionKey_PinnedDigest_RegressionGuard`.
- `nix build -L .#nix-cli --builders ''` succeeded and produced the benchmarked `./result/bin/nix`.

Next implication:

The high-water direct-map is only useful for mixed warm writeback where a run references many DB-origin ids and also creates a smaller number of new rows. It should be guarded or redesigned for cold-empty DBs, otherwise it adds overhead to the most expensive cold population case.

## Sparse direct-map refinement

The initial high-water implementation seeded identity maps for every DB-origin id up to the startup high-water mark. That preserves correctness but is the wrong cost model for both cold runs and small partial-hit writebacks: it pays to materialize rows that may not be referenced by any staged data.

Refined implementation:

- Seed sentinel identity mappings once for `Strings(0)` and `DataPaths(0)`.
- Keep staging restricted to run-local ids above each startup high-water mark.
- Seed direct identity mappings only for positive DB-origin ids actually referenced by staged rows.
- Leave run-local ids on the existing SQLite natural-key UPSERT/crosswalk path.
- Remove full map-size assertions, which forced broad identity maps and made sparse mapping impossible.

Expected effect:

- Hot/full-hit runs remain unchanged: no dirty rows means no writeback transaction.
- Cold-empty runs should avoid the broad old-id map materialization and avoid scanning staged refs for only id `0`.
- Partial-hit runs should benefit when a small number of new rows reference many old canonical rows, because only referenced old ids are mapped.

The invariant remains append-only: ids loaded from SQLite at startup are already canonical, while ids allocated during the disconnected run must be resolved by SQLite at shutdown through natural-key uniqueness and UPSERT.

Sparse direct-map cold benchmark after refinement:

- Built `.#nix-cli` locally with `nix build -L .#nix-cli --builders ''`.
- Cold cleared-cache `closures` traced eval succeeded in 90.235s.
- JSON sha256 remained `a301a379ed0fa92213849788627f3e8683cd8d7d75b10961011115b52326b93f`.
- Writeback: 2.049523s total, 99 SQL statements, 62.8M VM steps, 1.98M fullscan steps.
- This improves over the broad direct-map checkpoint (2.178647s, 114 statements) but remains slower than the earlier pre-direct-map cold baseline (1.834950s, 98 statements).

Interpretation:

- The broad identity-map materialization overhead is mostly removed.
- The remaining cold gap is not enough to justify keeping direct-map logic solely for cold population; its purpose is still partial-hit writeback.
- A proper partial-hit benchmark is required before deciding whether to keep this optimization, guard it behind a mixed-writeback heuristic, or replace it with CASE-based canonical-id expressions that avoid temporary old-id maps entirely.

Cold guard/sentinel refinement benchmark:

- Built `.#nix-cli` locally with `nix build -L .#nix-cli --builders ''`.
- Cold cleared-cache `closures` traced eval succeeded in 90.815s.
- JSON sha256 remained `a301a379ed0fa92213849788627f3e8683cd8d7d75b10961011115b52326b93f`.
- Writeback: 2.046824s total, 98 SQL statements, 62.8M VM steps, 1.98M fullscan steps.
- The guard excludes schema sentinel rows (`Strings(0)`, `DataPaths(0)`, `DirSets(0)`) from the old-data test, so a truly cold DB avoids sparse old-id scans and broad identity-map seeding.

Current status:

- Broad direct-map version: 2.178647s cold writeback, 114 statements.
- Sparse direct-map version: 2.049523s cold writeback, 99 statements.
- Sentinel/cold-guard sparse version: 2.046824s cold writeback, 98 statements.
- Recorded pre-direct-map baseline remains lower at 1.834950s cold writeback, but this latest cold path has the same statement count. The remaining difference is in SQLite VM work inside existing reconciliation statements, so the next useful comparison is a controlled A/B build from the pre-direct-map commit and this revision rather than treating the older number as directly comparable.

## Inline append-only canonicalization refinement

The sparse direct-map staging idea was superseded by inline canonicalization in dependent `INSERT ... SELECT` statements:

- Startup-loaded ids are already DB-owned canonical ids, so `local_id <= startup_high_water` can be used directly.
- Run-local ids are the only ids that require the Kimball-style surrogate-key crosswalk through `map_*` tables.
- Dependent SQL now expresses that split with `CASE WHEN local_id <= startup_high_water THEN local_id ELSE map_*.canonical_id END` plus a `LEFT JOIN map_* ... AND local_id > startup_high_water`.
- This keeps SQLite natural-key UPSERT/UNIQUE constraints as the conflict-resolution authority for new rows while avoiding temporary identity-map rows for old rows.

Cold-path refinement:

- A cold database has only sentinel rows at startup, so `startup_high_water == 0` for canonical tables that matter to `closures`.
- In that case the inline `CASE` expression is unnecessary; every staged id is run-local and should use the existing inner-join map path.
- The trace-value insertion path now uses the old inner-join SQL when there are no DB-origin rows, and only uses inline `CASE` / `LEFT JOIN` canonicalization for mixed partial-hit writebacks.

Measured result after the cold-path split:

- Built `.#nix-cli` locally with `nix build -L .#nix-cli --builders ''`.
- Cold cleared-cache traced `closures` eval: 90s wall, JSON sha256 `a301a379ed0fa92213849788627f3e8683cd8d7d75b10961011115b52326b93f`.
- Cold writeback: 2.014306s total, 99 SQL statements, 62.8M VM steps, 1.98M fullscan steps, 1.317301s in trace-table writes.
- Warm traced `closures` eval immediately after population: 3s wall, `42` hits, `0` misses, no shutdown writeback, same JSON sha256.

Interpretation:

- The inline split is semantically cleaner than materialized direct maps because DB-origin ids remain direct canonical ids and only run-local ids flow through SQLite natural-key reconciliation.
- The cold-path split recovers most of the avoidable overhead introduced by inline `CASE`/`LEFT JOIN` on empty databases.
- The remaining cold writeback gap versus the old pre-provenance number is still in trace-table insertion cost, not statement count; further optimization should target trace-value staging/insertion, not broad identity maps.

This is closer to the append-only provenance model than sparse direct-map seeding: the database remains the source of truth for canonical identity, but old DB-origin ids do not need to be re-proven to SQLite during writeback.

## Active task list

Each item must end with an adversarial review before it is accepted or rejected.

- [x] `Trace*Values` write path
  - Investigate staging keys, `ORDER BY`, temp indexes, split old/new insert shape, and `EXPLAIN QUERY PLAN`.
  - Target: reduce trace-table insertion cost without weakening the one-observation-per-trace-slot invariant.
- [x] Partial-hit benchmark
  - Construct a mostly warm cache run that still creates new rows referencing DB-origin canonical ids.
  - Target: measure the actual benefit of append-only provenance and inline canonicalization.
- [x] `carray` atom-feed option
  - Determine whether `carray` can replace one-column temp staging feeds for strings/hashes/needed-id sets.
  - Target: reduce C++ row-by-row temp table insertion where the payload is a single scalar column.
- [x] Custom virtual table option
  - Determine whether an eponymous virtual table over in-memory row spans is justified for multi-column staging.
  - Target: avoid temp table population for large staged rowsets without losing SQL-owned reconciliation.
- [x] Append-only provenance pruning
  - Tighten the dirty/new-row closure so shutdown stages only run-local rows and DB-origin rows actually needed by dirty data.
  - Target: make partial-hit writeback proportional to new work.
- [x] SQLite planner/optimizer pass
  - Use planner facts, statement counters, temp sort counts, and query-shape alternatives to decide what SQLite is optimizing badly.
  - Target: move from wall-time guesses to planner-backed changes.
- [x] Writeback failure semantics
  - Decide whether destructor writeback failures are acceptable as logged cache failures or whether an explicit fatal flush boundary is required.
  - Target: avoid silent cache corruption or silent loss of expected trace-cache publication.
- [x] Session model follow-through
  - Re-review append-only candidate events, exact-session scopes, recovery scopes, runtime roots, and `ExactSessionHeadCache` as a derived projection.
  - Target: ensure fast hits are expressed as disposable projections over immutable observations, not mutable source-of-truth rows.

## Task findings

### `Trace*Values` write path

Accepted implementation:

- The temp `stage_trace_*_values` tables now use `PRIMARY KEY(trace_id, dep_key_id)`, matching the persistent `Trace*Values` relation invariant.
- The temp trace-value stage tables are `WITHOUT ROWID`, but only after narrowing the key to the semantic trace slot.
- The trace-value publication statements no longer force `ORDER BY`; the target tables are keyed relations, and ordering is not semantic.

Rejected/intermediate variants:

- Narrowing the temp key without removing `ORDER BY` reduced VM steps but slightly regressed wall time: 2.051873s writeback, 62.2M VM steps.
- Removing the trace-value `ORDER BY` clauses improved the no-`WITHOUT ROWID` variant: 2.001770s writeback, 57.1M VM steps, `sqlSorts` 6 -> 1.
- The final narrow `WITHOUT ROWID` stage shape improved substantially: 1.632535s writeback, 57.7M VM steps, `tracesUs` 0.838417s.

Adversarial review:

- Correctness improves: the old temp key allowed multiple payloads for the same `(trace_id, dep_key_id)`, while the persistent tables only allow one. `INSERT OR IGNORE` could silently choose one value. The new temp key fails closed at staging time.
- Removing `ORDER BY` does not change observable semantics because relation identity is enforced by target primary keys and uniqueness, not insertion order.
- The final `WITHOUT ROWID` change is safe because the temp table's primary key is now exactly the semantic key; it avoids rowid-plus-unique-index storage for a relation whose key is already the row identity.
- Warm-cache behavior remains intact: immediate warm run after cold population had `42` hits, `0` misses, no shutdown writeback, and the same JSON sha256.

Measured accepted result:

- Cold cleared-cache traced `closures` eval: 90s wall, JSON sha256 `a301a379ed0fa92213849788627f3e8683cd8d7d75b10961011115b52326b93f`.
- Cold writeback: 1.632535s total, 99 SQL statements, 57.7M VM steps, 1.98M fullscan steps, 1 sort, 0 autoindexes.
- Warm traced `closures` eval after population: 2s wall, `42` hits, `0` misses, no writeback, same JSON sha256.

### Partial-hit benchmark

Workload:

1. Clear eval-trace cache.
2. Prime with `closures.gnome.x86_64-linux`.
3. Evaluate full `closures` against that partially populated DB.
4. Evaluate full `closures` once more to confirm the mixed writeback produced a full-hit cache state.

Measured result:

- Prime GNOME run: 9s wall, `5` misses, 0 hits, 0.519609s writeback, 21.2M VM steps.
- Partial full-`closures` run: 82s wall, `5` hits, `37` misses, 1.236577s writeback, 49.6M VM steps.
- Partial writeback split: 0.029336s canonical IDs, 0.008817s stage atoms, 0.079510s dep keys, 0.911364s trace writes.
- Final warm full-`closures` run: 3s wall, `42` hits, `0` misses, no writeback.

Adversarial review:

- This is a real mixed provenance workload: the second run loaded DB-origin canonical ids from the GNOME prime and then published new rows for the remaining `closures` candidates.
- The JSON sha256 for full `closures` remained `a301a379ed0fa92213849788627f3e8683cd8d7d75b10961011115b52326b93f` for both partial and final warm runs.
- The DB after the partial run had the same `Results` and candidate counts as cold full population, but one extra `Traces` row (`36` vs the cold full run's `35`). That is acceptable for an append-only observation log: a GNOME-specific trace can remain in the canonical trace table even if no full-`closures` head points at it.
- The append-only provenance split is doing useful work in partial-hit mode: canonical-id and atom-staging time are much lower than cold full population because DB-origin ids are reused directly and only run-local rows are reconciled.

### `carray` atom-feed option

Finding: defer/reject for the current writeback path.

Current SQLite facts:

- The available SQLite is 3.50.4 and does not have `carray` registered; probing `SELECT * FROM carray(1)` fails with `no such table: carray`.
- Current SQLite documentation says `carray` is built into the amalgamation starting with SQLite 3.51.0, but disabled unless compiled with `SQLITE_ENABLE_CARRAY`.
- Before 3.51.0, `carray` must be compiled independently and added as an extension.
- `sqlite3_carray_bind` supports scalar arrays and `SQLITE_CARRAY_BLOB` through `struct iovec`, which is the only safe representation for Nix strings because strings/paths are byte sequences and may contain NUL/non-UTF-8 bytes.

Why it is not a good immediate implementation:

- The shutdown reconciliation needs `(local_id, natural_key)` rows so SQLite can build `map_*` crosswalks. `carray` exposes one value column, not a typed multi-column row source.
- Using separate `carray` calls for ids and values and joining them by implicit row order would be brittle unless SQLite explicitly documents stable ordinality for that shape. The documented interface is a single value column.
- In the accepted trace-value optimization, cold `stageAtomsUs` is about 82ms and partial-hit `stageAtomsUs` is about 9ms. `carray` cannot touch the dominant trace-value write path and would add extension/linkage complexity for a small ceiling.

Where it can still be useful later:

- One-column `needed_*` id sets if append-only pruning reintroduces materialized needed-id filtering.
- Pure `INSERT OR IGNORE INTO Strings(value) SELECT value FROM carray(?)`-style feeds, but only if map construction still has a separate safe source of `(local_id, value)`.

Adversarial review:

- Do not implement `carray` by dynamically loading extensions in normal eval: extension loading is disabled by default for security and would be a poor cache-write dependency.
- Do not assume the system SQLite has `carray`; this exact environment does not.
- Do not use `SQLITE_CARRAY_TEXT` for Nix strings; it is null-terminated `char *` and is wrong for arbitrary byte strings.
- If implemented later, vendor/static-register it or require a SQLite build with `SQLITE_ENABLE_CARRAY`, keep backing storage alive through statement finalization, and use BLOB/iovec bindings for byte strings.

References:

- `carray` overview and availability: https://www.sqlite.org/carray.html
- `sqlite3_carray_bind`: https://sqlite.org/c3ref/carray_bind.html
- Static extension strategy: https://www.sqlite.org/loadext.html#statically_linking_a_run_time_loadable_extension
- Pointer-passing security model: https://sqlite.org/bindptr.html

### Custom virtual table option

Finding: defer, do not implement in this pass.

What it would look like:

- Register an eponymous-only virtual table module on the short-lived writeback connection.
- Expose C++ in-memory row spans as SQL table-valued functions with hidden pointer/token arguments.
- Implement one module per row shape or one generic module with a schema descriptor: strings, data paths, dep keys, results, traces, trace values, candidates, runtime roots.
- Implement `xBestIndex` so SQLite knows required hidden parameters, row estimates, ordering, and whether constraints/orderings are consumed.
- Mark modules direct-only where appropriate and manage backing-storage lifetime through statement finalization.

Why it is not a good immediate implementation:

- The current temp tables are not just transport; they are also SQL-enforced staging invariants. The trace-value bug found above was exactly a staging-key invariant. A virtual table would need to reimplement equivalent fail-closed uniqueness checks or still materialize into constrained temp tables.
- SQLite cannot add indexes to virtual tables after the fact; any useful lookup behavior must be implemented inside `xBestIndex` and the cursor implementation.
- Our accepted trace-stage temp tables are now `WITHOUT ROWID` and keyed by the semantic slot. The dominant trace writeback cost dropped from about 1.3s to 0.84s without adding a custom extension surface.
- A virtual table would still leave canonical conflict resolution in SQL, but it would move staging validation and row-source correctness into C++ callback code. That is a worse risk profile unless temp staging again becomes the dominant cost.

Adversarial review:

- Soundness risk: a virtual row source that does not enforce duplicate/conflicting local ids can recreate the old `INSERT OR IGNORE` silent-choice failure mode.
- Precision risk: if row lifetime or pointer binding is wrong, SQLite may read stale or moved C++ memory during statement execution.
- Planner risk: bad `xBestIndex` estimates can make SQLite choose worse join orders, and virtual tables cannot be rescued by ad hoc `CREATE INDEX`.
- Security risk: pointer-backed row sources must not be usable from schema objects/triggers/views; use SQLite virtual-table security controls if this is implemented.
- Current measured upside is not enough to justify these risks.

References:

- SQLite virtual table mechanism: https://www.sqlite.org/vtab.html
- Eponymous/eponymous-only virtual tables: https://www.sqlite.org/vtab.html#eponymous_virtual_tables
- Table-valued functions via virtual tables: https://www.sqlite.org/vtab.html#table_valued_functions

### Append-only provenance pruning

Finding: keep the current high-water pruning; defer deeper dirty-closure pruning until there are targeted tests.

What is already implemented:

- Startup-loaded IDs are treated as DB-origin canonical IDs.
- Rows above the startup high-water mark are treated as run-local and reconciled through SQLite natural-key uniqueness.
- Dependent SQL directly uses DB-origin IDs and only joins `map_*` for run-local IDs.
- Cold-empty DBs avoid old-id map materialization.
- Full warm-hit runs skip shutdown writeback entirely.

Measured effect:

- Cold full `closures`: 1.632535s writeback after trace-stage fixes.
- Partial GNOME-prime then full `closures`: 1.236577s writeback with `5` hits and `37` misses.
- Partial-hit `stageAtomsUs` was 8.817ms and `canonicalIdsUs` was 29.336ms, so atom/canonical staging is no longer the dominant partial-hit cost.

Possible deeper design:

- Build explicit writeback roots from dirty candidate rows and dirty runtime-root rows.
- Derive `needed_trace_ids` from dirty candidates.
- Derive `needed_result_ids` from dirty candidates plus needed traces.
- Derive `needed_dep_key_ids`, observation ids, string ids, path ids, hash ids, and dirset hashes from needed traces/results/dep keys/runtime roots.
- Stage only run-local rows in those needed sets.
- Preserve inline direct use of DB-origin IDs; only run-local IDs need `map_*` rows.
- Add cardinality assertions for every required map edge before dependent inserts.

Why this was not implemented now:

- Trace-context observations refer to `referenced_full_hash`, not a trace-id FK. Publishing or pruning a trace that is only reachable through context/session lookup needs tests around `resolveTraceContextHash` semantics.
- A trace without a candidate may still be useful as a canonical observation if another candidate/context can resolve it by hash or session scope. The current append-only model tolerates extra observations; aggressive pruning can silently reduce precision.
- The partial-hit benchmark shows the remaining cost is trace-value publication, not broad canonical atom staging.

Adversarial review:

- Soundness risk: pruning a trace/result/dep key that a dirty candidate or trace context needs would create a cache entry that verifies differently or fails to replay.
- Precision risk: over-pruning may keep correctness but reduce future hit rate by dropping observations that were computed during the run.
- Performance risk: computing and maintaining exact dirty closures in C++ may cost more than the already-small partial-hit staging cost unless the workload has many speculative/unpublished run-local rows.
- The safe path is to keep high-water provenance and add closure-pruning tests before changing publication roots.

### SQLite planner/optimizer pass

Finding: do not add an optimizer/statistics maintenance step to the writeback path right now.

Planner/counter facts after accepted trace-stage changes:

- Cold full `closures` writeback has `sqlStatements=99`, `sqlSorts=1`, `sqlAutoindexes=0`, and 57.7M VM steps.
- The five trace-value `ORDER BY` sort b-trees were real overhead; removing them dropped `sqlSorts` from 6 to 1 and reduced VM work.
- `sqlFullscanSteps` remain, but these are expected scans over TEMP stage tables feeding set-oriented `INSERT ... SELECT` reconciliation.
- No automatic indexes means SQLite is not compensating for missing declared indexes in these statements.

`PRAGMA optimize` check:

- `PRAGMA optimize(-1)` on a populated cache said all persistent tables were candidates for `ANALYZE` because the cache had no `sqlite_stat1`.
- Running `PRAGMA optimize` manually took about 8ms and created 37 `sqlite_stat1` rows.
- A warm full-`closures` run after stats were created remained effectively unchanged: 2s wall, `42` hits, `0` misses, no writeback, same JSON sha256.

Adversarial review:

- Adding `PRAGMA optimize` after shutdown writeback would either add another write transaction or need to be folded into the shutdown transaction. That is extra DB mutation outside the canonical data model and has no measured benefit for the current startup bulk-load path.
- Adding `ANALYZE`/stats can change plans. SQLite documents that this is usually positive but not guaranteed; plan stability can matter for reproducibility.
- The main writeback statements are dominated by temp-stage scans and primary/unique-key probes. Persistent `sqlite_stat1` is unlikely to help those statements because the temp rows are created and consumed inside the same shutdown transaction.
- Keep statement counters in `NIX_SHOW_STATS`; revisit stats maintenance only if a future persistent lookup path or complex join reappears.

References:

- SQLite recommends `PRAGMA optimize` for stats maintenance in general, especially before closing short-lived connections: https://www.sqlite.org/pragma.html#pragma_optimize
- SQLite `ANALYZE` and planner statistics behavior: https://www.sqlite.org/lang_analyze.html

### Writeback failure semantics

Finding: do not try to make destructor writeback fatal in place; redesign around an explicit flush boundary if trace-cache publication must be guaranteed.

Current semantic problem:

- Shutdown writeback happens as object teardown. Destructors cannot safely report ordinary errors by throwing across teardown paths.
- A writeback failure can therefore be logged as ignored while `nix eval` still exits successfully.
- That is acceptable only under a best-effort cache durability model: evaluation output remains correct, but the trace cache may not be populated.

Better model:

- Add an explicit `flush()`/`close()` boundary owned by the evaluation command/session before teardown.
- Make explicit flush failures visible to the caller and decide policy there:
  - fatal if eval tracing was explicitly requested as an output-producing cache publication mode
  - warning-only if eval tracing remains an opportunistic acceleration cache
- Keep destructor writeback only as a best-effort fallback and never rely on it for guaranteed publication.
- Keep the SQLite transaction atomic: if flush fails, no partial canonical graph should be committed.

Adversarial review:

- Making the destructor call `terminate()` would be too blunt: cache write failure should not necessarily kill unrelated eval output, and termination loses diagnostic context.
- Throwing from a destructor is unsafe during stack unwinding and can make failure modes worse.
- Silently ignoring writeback failure is too weak for tests and for users expecting trace-cache publication.
- The right breaking change is architectural: explicit lifecycle ownership with a policy decision at the command/session boundary.

Status:

- No code change in this pass. The accepted SQL changes still fail atomically inside the shutdown transaction; the unresolved issue is how failures are surfaced to callers.

### Session model follow-through

Finding: candidate sessions mostly fit the append-only/projection model; runtime roots need a future redesign.

Candidate model:

- `TraceCandidateEvents` is the append-only observation log: a batch says candidate N was observed for an attr path and points at canonical trace/result ids.
- `ExactSessionCandidateScopes` and `RecoveryCandidateScopes` attach candidate observations to lookup domains.
- `ExactSessionHeadCache` is a fast-hit projection for `(session_key_id, attr_path_id) -> newest candidate`.
- The head projection is mutable, but it is not source of truth if every served candidate is still verified and if candidate scopes/events remain available for fallback/rebuild.

Why `ExactSessionHeadCache` is acceptable as a projection:

- It stores canonical ids after SQL reconciliation.
- The update rule keeps the highest DB-assigned `candidate_id`, which corresponds to the latest committed candidate event under SQLite's single-writer serialization.
- If the projection is stale, missing, or deleted, correctness should not change; only hit startup/ordering can degrade.

Remaining concern:

- A disconnected process can evaluate with an older world view, then commit after another process. The higher candidate id is later publication, not necessarily "more semantically current". This is acceptable only because hits are verified against current dependencies before being trusted.
- If any code path treats `ExactSessionHeadCache` as authoritative without verification/fallback, that is a soundness bug.

Runtime roots:

- `SessionRuntimeRoots` is currently written with replace semantics, which is source-of-truth-shaped mutable state.
- If runtime roots are deterministic facts of `(session_key, source)`, then conflicting values should fail or be represented as distinct observations, not overwrite.
- If runtime roots are observations over time, the schema should mirror candidates: append-only root events plus a derived head projection.

Better runtime-root model:

- Add `SessionRuntimeRootEvents(root_event_id, batch_id, session_key_id, source_id, fetch_identity_id, nar_hash_id, store_path_id)`.
- Add a scope/key table for lookup by `(session_key_id, source_id)`.
- Add `SessionRuntimeRootHeadCache(session_key_id, source_id, root_event_id, fetch_identity_id, nar_hash_id, store_path_id)` as a disposable projection.
- Update the head projection by newest root event, but treat it as rebuildable acceleration only.
- If the intended invariant is "same session/source must always resolve to the same root", enforce a `UNIQUE(session_key_id, source_id, fetch_identity_id, nar_hash_id, store_path_id)` observation plus a conflict-detection query for divergent roots.

Adversarial review:

- Mutable rows are acceptable only for derived projections that can be dropped/rebuilt without changing correctness.
- Candidate source-of-truth rows are append-only and hold up under this rule.
- Runtime roots do not currently hold up as cleanly because replacement can hide divergent observations.
- Do not solve this by adding legacy aliases or shims. The right fix is a schema break: root observations plus projection, or immutable deterministic facts with explicit conflict failure.

### Cold-writeback zero-high-water fast path

The inline `CASE`/`LEFT JOIN` canonicalization is only necessary when a table has startup-loaded rows. When a table-specific high-water mark is zero, all non-sentinel references are run-local and already pass through the normal staging crosswalk, so dependent SQL can use the cheaper inner-join form (`map_*.canonical_id`). The implementation therefore emits the append-only split only for tables with `high_water > 0`, and seeds the stable sentinel crosswalks for `Strings(0)` and `DataPaths(0)`.

The sentinel crosswalk seed must be paired with `INSERT OR IGNORE` when populating `map_strings` from `stage_strings`: cold writeback stages `Strings(0)`, while partial writeback may skip it. The conflict policy is intentionally only for the local sentinel duplicate; non-sentinel stage rows are still unique by construction.

The trace-value inserts keep an explicit cold path. A cold DB has no startup-loaded ids to preserve, so it can use the original inner-join SQL without inline canonicalization. The append-only `CASE`/`LEFT JOIN` form is reserved for partial DB-origin writeback, where old and run-local ids can coexist in the same staged dependency references.

## Regression tests added

- `TraceStoreTest.WriteBack_DuplicateTraceSlot_FailsClosed` injects a corrupted in-memory full trace with two values for the same `(trace_id, dep_key_id)` slot and verifies shutdown writeback fails closed at SQL staging. The public recording path does not naturally create this state; the test exists to protect the staging-table invariant.
- `TraceStoreTest.WriteBack_PartialReload_ReusesDbOriginDeps` records, flushes, reloads, records a second trace sharing a DB-origin dep key, flushes again, reloads, and verifies both traces. This covers the mixed-provenance writeback path where loaded canonical IDs are direct-mapped and new run-local IDs are reconciled through SQL crosswalks.

Validation: `nix build -L .#nix-expr-tests --builders ''` succeeded, then the two targeted tests passed with `./result/bin/nix-expr-tests --gtest_filter='TraceStoreTest.WriteBack_DuplicateTraceSlot_FailsClosed:TraceStoreTest.WriteBack_PartialReload_ReusesDbOriginDeps'`.

## Runtime root session redesign

Implemented the remaining session-model follow-through for runtime roots:

- Replaced mutable `SessionRuntimeRoots` with append-only `SessionRuntimeRootEvents` plus `SessionRuntimeRootHeadCache`.
- Bumped `kSchemaEpoch` to 37 because the cache schema shape changed incompatibly.
- Runtime roots now follow the same source-of-truth/projection split as candidate observations: events are canonical history, the head cache is a disposable fast-start projection.
- Repeated identical observations for `(session, source)` are idempotent in memory; conflicting observations are preserved as dirty rows and rejected by shutdown SQL before the projection can be updated.
- Load still uses the head projection for startup speed, and `loadAndVerifyRuntimeRoots` still performs nar-hash verification before registering a root, so a missing/stale projection can reduce hits but not make a stale trace sound.

Validation: `nix build -L .#nix-expr-tests --builders ''` succeeded after schema generation, then these targeted tests passed:

- `TraceStoreTest.WriteBack_DuplicateTraceSlot_FailsClosed`
- `TraceStoreTest.WriteBack_PartialReload_ReusesDbOriginDeps`
- `SessionConfigTest.TraceStore_RuntimeRootPersistence_RoundTripsTypedRows`
- `SessionConfigTest.TraceStore_RuntimeRootPersistence_ConflictingRootFailsClosed`

Full-build note: `nix build -L . --builders ''` rebuilt the changed libraries and generated schema, but the aggregate build stopped in `nix-expr-tests-run`. After updating the intentional epoch-dependent pinned digest, the targeted schema/session/writeback subset passes. The remaining aggregate failures observed in the full run were outside this runtime-root writeback change: two `EvalTraceProperty_DepCorrectness` RapidCheck failures, `TracedJSON_GenericClosure_CacheHit`, and `PointerEquality_FindAlongAttrPath_SameCommandPath` (`bad_weak_ptr`).

## Startup reverse-crosswalk for same-process reloads

Root cause of the remaining aggregate test failures was stale in-process interning after SQLite canonicalization. The shutdown path correctly stages local ids and lets SQLite assign/resolve canonical ids, but same-process test reloads reused the existing `EvalState` pools. `StringInternTable::bulkLoad` ignored DB rows whose numeric ids were already occupied locally, so DB-owned ids could decode through stale local strings/data paths. That made current-node attr paths and dependency keys miss after reload.

Fix:

- Startup load now builds a DB-id -> process-id crosswalk for `Strings` and `DataPaths` by natural key.
- Loaded dep keys, dirset members, trace-context attr paths, exact-session heads, and recovery scopes are translated through that crosswalk before entering the in-memory snapshot.
- If string/data-path numeric ids are not canonical in the current process, the append-only high-water shortcut for that domain is disabled for the next shutdown writeback, forcing those references through SQL staging/canonicalization again.
- This is the inverse of the shutdown ETL/surrogate-key crosswalk: DB remains canonical source of truth, while a still-running process may use local ids until the next boundary transaction reconciles them.

Validation added:

- `MaterializationDepTest.StartupLoad_TranslatesCanonicalDbIdsIntoExistingPools` records nested child traces, flushes, reopens in the same `EvalState`, and verifies both child `FileBytes` deps survive reload.
- The previous aggregate failures now pass in the targeted subset: `EvalTraceProperty_DepCorrectness.AllTrackedSlots_Present`, `EvalTraceProperty_DepCorrectness.Annotation_Consistency`, `TracedJSON_GenericClosure_CacheHit`, and `PointerEquality_FindAlongAttrPath_SameCommandPath`.

Validation command: `./result/bin/nix-expr-tests --gtest_filter='MaterializationDepTest.StartupLoad_TranslatesCanonicalDbIdsIntoExistingPools:EvalTraceProperty_DepCorrectness.AllTrackedSlots_Present:EvalTraceProperty_DepCorrectness.Annotation_Consistency:TracedDataTest.TracedJSON_GenericClosure_CacheHit:TracedDataTest.PointerEquality_FindAlongAttrPath_SameCommandPath'` passed.
