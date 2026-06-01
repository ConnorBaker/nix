# Eval-trace perf: cold recording + hot verification — avenues (2026-05-31)

Started after the producer-partition arc concluded (derivation-producer-partition-rfc.md §18: the edge
direction is structurally dead). That arc never addressed the branch's ACTUAL problem: cold recording
is slow, and **hot is not instantaneous** (and it should be — a warm cache hit serves an
already-computed result). This doc grounds both costs in code + the §17 bench measurement and lists
the avenues, ranked by leverage.

All numbers from the §17 `.aggr-bench2` run (closures.gnome, 25 commits, no-§3b = the production cache
shape). Median per commit unless noted.

## The two costs, measured

### HOT (warm cache hit) ≈ 1.0s — and it SHOULD be ~instant
On a pure cache hit (`hits=7, misses=0, record.count=0`), the warm serve still spends:
- `verify.timeUs` = **0.40s**, `verifyTrace.timeUs` = 0.39s
- `verify.depsChecked` = **170,670** deps re-verified
- `depHash.cacheHits=56,488 / cacheMisses=29,326` → **~29K file/dir hashes RECOMPUTED from disk**
- `depHash.contentUs` = 0.12s (file re-hashing), `directoryUs` = 0.015s

**Root cause: warm verification re-proves validity by re-reading + re-hashing ~29K files every
process.** The result is stored and served; the cost is purely "are all 170K deps still valid?",
answered by recomputing a large fraction of their hashes from scratch. Two compounding factors:
1. **The content-hash cache is PER-PROCESS, not persisted.** `EvalEnvironmentSharedState::
   fileContentHashCache` (`boost::concurrent_flat_map<SourcePath, DepHash>`, eval.cc:78/156) is built
   fresh in the EvalState ctor and cleared on reset (eval.cc:1584/1598). A fresh warm-eval process
   starts EMPTY → the 29K misses = files hashed in the priming process, thrown away, re-hashed now.
2. **FileBytes verify ALWAYS re-reads+re-hashes.** `dep-resolution-service.cc:375-376`:
   `computePathHashedDep(... [](p){ return depHash(p.readFile()); })` — unconditional `readFile()`.
   There is a `maybeLstat()` (line 291) but it's only for existence/missing handling, NOT a
   content-change shortcut. No mtime/inode fast-path exists anywhere (grep: only the per-process
   content cache; no stat cache).

### COLD (first eval, recording) ≈ 6.7s over the ~1s eval
Recording the trace the first time: dep recording + 3 hashes + serialize + SQLite flush, per trace.
The §3b/#4 data put this at ~726µs–1.5ms/producer × thousands. The eval THREAD pays it:
- `backend.record(ctx,…)` (trace-session.cc:62) → `store->record` via
  `ctx.syncAwait(coroBlock(blockingPool, …))` (context.cc:340-342). **`syncAwait` BLOCKS the eval
  fiber** until the record completes (eval-context.hh:5: "blocking the eval thread to wait for an async
  operation"). So recording runs on a thread pool but the eval WAITS for each one — serialized, not
  fire-and-forget. Layer 1/2a (deferFlush) batched the SQLite flush (~−9% wall) but the
  hash/serialize/await is still inline on the eval thread.

## Avenues — HOT (highest leverage; hot should be ~instant)

### H1. PERSIST the content-hash cache across processes (biggest, most direct win)
The 29K re-hashes exist only because `fileContentHashCache` dies with the process. Persist
`SourcePath → DepHash` (keyed by a stable path identity + a cheap freshness token — see H2) so the
warm process reads hashes instead of recomputing. The eval-trace SQLite store already persists far more;
a `(path_identity, freshness) → content_hash` table is a small addition. Caveat: the key must include a
freshness discriminator or it's unsound (serving a stale hash for a changed file) — which is H2.
- Leverage: removes ~0.12s+ of the 0.40s verify directly; more on file-heavier workloads.
- Risk: soundness of the freshness key (H2). Storage growth (bounded by distinct source files).

### H2. ~~mtime/inode fast-path~~ — REJECTED (2026-05-31). Use the git blob OID instead (H2').

**mtime is DEAD for this workload, two independent reasons (user critique, confirmed):**
1. **`git checkout <commit>` rewrites mtime on every touched file.** The bench (and the real "evaluate a
   run of commits, then evaluate them again" scenario) checks out each commit → the 2nd pass sees fresh
   mtimes on files whose CONTENT is byte-identical across commits → ZERO reuse, which is the entire
   point. Confirmed: a checkout of a different commit rewrites the working-tree mtimes.
2. **Portability/granularity:** mtime granularity is 1–2s on some filesystems (sub-second edits
   invisible → unsound) and mtime is unreliable/absent on others. A content cache keyed on mtime is
   both unsound (coarse granularity) and useless (checkout churn) here.

So mtime fails on exactly the reuse case we need. The freshness token must be CONTENT-DERIVED but
cheap-to-obtain (not a re-hash by us).

### H2'. git blob OID as the freshness token (the right design — content-addressed, checkout-stable)
Git already content-addresses every file: the blob OID (`git rev-parse <rev>:<path>` / index entry) is
a hash of the file content, **identical across commits when content is identical**, and git computed it
at checkout (we read it from the index/tree, O(1), NO content hashing by us). VALIDATED: `lib/default.nix`
has blob OID `15949d1c…` at BOTH HEAD and HEAD~5 (unchanged content → same OID → reuse mtime can't give);
`git ls-files --stage` / `git rev-parse <rev>:<path>` return it from the index without reading the file.

Design (two-level token, because the OID ≠ the dep hash):
- The FileBytes dep hash is `depHash(p.readFile())` (blake3/sha256 over raw content,
  dep-resolution-service.cc:376) — NOT the git blob OID (sha1/sha256 over `"blob <len>\0"+content`, a
  different preimage). So we can't store the OID AS the hash.
- Instead: persist a `(path_identity, blob_OID) → depHash` mapping. On verify: get the file's current
  blob OID cheaply (git index/tree lookup); if it matches the recorded OID, TRUST the recorded `depHash`
  (no read, no content-hash). Re-read+re-hash only on OID mismatch (genuine content change). The OID is
  the FRESHNESS KEY; the depHash is the cached VALUE. Survives checkout (content-addressed, not mtime).
- Cross-commit reuse: this is what makes "evaluate N commits, then again" fast — identical files across
  commits share an OID → share the cached depHash → no re-hash on the 2nd pass.

**LOAD-BEARING PREREQUISITE (confirmed blocker for the bench's path, viable for flakes):** getting the
OID cheaply requires the source be read through a GIT-AWARE accessor.
- The bench uses `nix eval -f /abs/path` → `getFSSourceAccessor()` = plain `PosixSourceAccessor`
  (eval.cc:464), wrapped in a union with `storeFS`. A posix accessor has NO git awareness;
  `SourceAccessor::getFingerprint` returns the default `nullopt` (source-accessor.hh:221). So at read
  time there is NO OID — getting one would require shelling to `git` per file (defeats "cheap"). For the
  raw `-f /path` and dirty/non-git cases, NO git-OID token → must fall back to content-hash (H1
  persistence is the only lever there).
- The FLAKE path (`git+file://` input, locked rev) reads via `GitSourceAccessor` (git-utils.cc), which
  IS git-aware (has the repo object DB, looks up blob OIDs per path). For locked-git-input flake evals —
  the production-relevant repeated-eval case — the OID IS available. This is where H2' pays off.
- So H2' applicability = "source read through a git-aware accessor" (flake/git+file, lazy-trees). The
  bench's `-f /abs/nixpkgs` does NOT qualify as-is — which means to even MEASURE H2' we must bench a
  flake-shaped workload (git+file input), not the `-f` shape. Note this before implementing.

Implementation site: the FileBytes verify lambda (dep-resolution-service.cc:375-376) gains an OID
fast-path; the dep record carries the blob OID (schema addition); the accessor must expose a
`getBlobOID(path)`-style cheap lookup (GitSourceAccessor has it internally; needs surfacing on the
SourceAccessor interface or via the SemanticRegistry/input-resolution path that already resolves the
dep's source identity). The GitRevisionIdentity dep machinery (types.hh) is the existing precedent for
git-identity-keyed verification — but it's per-REPO (the rev), not per-FILE (the blob); H2' is the
per-file blob granularity, finer than the existing GitRevisionIdentity.

**Soundness:** the blob OID is a content hash → OID match ⇒ content unchanged (modulo git's
sha1-collision resistance, same trust class as any content cache). No mtime forgeability. Sound by
construction; no setting/conservatism needed (unlike mtime).

### H3. Don't re-verify the whole closure on every hit: a trace-level validity short-circuit
Even with H1+H2, warm verify still WALKS 170,670 deps (per-dep stat/lookup). The deeper question: can a
trace prove validity WITHOUT touching all its deps? Options:
- (a) A trace-level "all-inputs-fingerprint": one hash over the trace's full dep set + a single check
  that no input changed (e.g. a generation counter / a top-level source fingerprint that, if unchanged,
  certifies the whole subtree). This is the "certificate-before-load" idea the earlier research heavily
  refuted for SOUNDNESS (a single fingerprint can't be recomputed without reading the inputs it
  summarizes) — but combined with H2's mtime tokens it becomes tractable: the fingerprint is over the
  (path, mtime-token) pairs, checkable by N cheap stats, short-circuiting to ONE compare if all stats
  match. Needs design (this is the real architectural lever for "instant hot").
- (b) Skyframe-style: only re-verify deps reachable from a changed file (reverse-dep invalidation).
  Bigger change; Nix's per-eval model doesn't keep a persistent reverse-dep graph across processes.
- Leverage: highest ceiling (true ~instant hot) but highest design cost. H1+H2 are the tractable
  first wins; H3 is the follow-on if 170K stats is still too slow.

## Avenues — COLD (recording overhead)

### C1. Make recording FIRE-AND-FORGET (don't block the eval thread on syncAwait)
Today `backend.record` block-waits via `syncAwait` (context.cc:340). The record's RESULT
(traceId) is only needed to wire the TracedExpr's lazy traceId — which is consumed lazily/later. Avenue:
hand the (pathId, value, deps) to the blocking pool and DON'T await — let eval continue while the
record happens concurrently. Eval finishes; a barrier at session end joins outstanding records.
- This is the real "async recording" the §3b notes gestured at but never built for the MAIN path (only
  the producer recordSync got Layer 1/2a, and even that block-waits). It directly removes the cold
  record cost from the eval critical path.
- Risk: the traceId back-reference (TracedExpr::ensureLazy().traceId) and ordering (a later eval step
  that reads a just-recorded trace within the SAME session). Needs the within-session-read barrier the
  §3b async plan analyzed. Soundness: a crash mid-eval loses unflushed records = future miss, not wrong
  (cache semantics) — same as Layer 1's analysis.
- Leverage: cold recording is ~6.7s; moving it off the critical path could approach the no-trace eval
  time (~1s) for the COMPUTE part, leaving only the unavoidable I/O.

### C2. Layer 2b — move hash + serialize off the eval thread (the deferred §3b item)
Even if C1 fire-and-forgets the SQLite write, the 3 hashes + 2 serializes currently run inline. Layer 2b
(async-producer-recording-plan §6) moves them to a worker. BLOCKER (already analyzed): `DataPathPool`
is not thread-safe (interning-pools.hh; single-threaded-writer invariant). Either make it concurrent
(hot-path-read structure → risky) or eager-collectPath on the eval thread before handoff (shrinks the
win). C1 is the bigger, cleaner win; C2 is the residual.

### C3. Record LESS: the 170K deps are largely redundant
The closure flattens the same leaf deps into many traces (the 607× — the producer-partition arc tried
and failed to fix this via edges). A different angle: don't RECORD the redundant flattened deps in the
first place (dedup at record time by interned dep-set, store a reference). This is storage + record-cost
both, and avoids the edge's hot-verify penalty (the dep set is still inline-resolvable, just stored
once). Distinct from the producer edge (which moved verification into a separate trace); this is pure
record-time dedup of identical dep VECTORS across traces. Unexplored.

## Recommended order (tractable → architectural) — REVISED after the mtime rejection
1. **H1 (persist the content-hash cache across processes)** — now the most tractable hot win that works
   on EVERY accessor (incl. the bench's `-f /path` posix path, where H2' can't get an OID). Persist
   `(path_identity → depHash)` keyed by a content-stable key. The key question H1 must answer is the
   SAME one mtime failed: what makes the cache entry reusable across processes/checkouts? For git-aware
   accessors the key is the blob OID (H2'); for posix it's… the content hash itself, which is circular
   (you'd have to read+hash to know the key → no saving). **So H1 alone does NOT help the posix `-f`
   path** — its reuse key needs H2' (git OID) or a flake/store content-address. H1+H2' together are the
   real hot fix, scoped to git-aware/flake reads.
2. **H2' (git blob OID freshness token)** — the content-addressed, checkout-stable token. Scoped to
   git-aware accessors (flake/git+file). Requires benching a FLAKE-shaped workload (not the `-f` bench).
3. **C1 (fire-and-forget recording)** — the cold fix; removes recording from the eval critical path.
   Accessor-independent (helps every workload).
4. **H3 / C2 / C3** — architectural follow-ons.

REVISED framing: "hot instant" for REPEATED FLAKE evals (the production-relevant case) = H1+H2' (git-OID
content cache). The bench's `-f /abs/path` posix path is a measurement artifact that CANNOT benefit from
any content-stable token (no git awareness, mtime-churned) — to measure the hot win we must use a
flake/git+file workload where the accessor exposes OIDs. C1 (cold) is the accessor-independent win.

OPEN QUESTION before implementing (the load-bearing uncertainty): does a locked-git-input flake eval
actually read each source file through GitSourceAccessor with a cheap per-file OID lookup, on the hot
path, WITHOUT having already copied the whole tree to the store (in which case the read is from a store
path and the "OID" is the store path itself)? If lazy-trees reads blobs on demand from the git ODB, H2'
is a cheap ODB lookup. If the flake source is eagerly copied to /nix/store first, then the source reads
are store reads (immutable, already content-addressed by store path) and the cache key is the store
path — EVEN SIMPLER, no git needed, and H1 keyed on store path works directly. MUST determine which,
because it changes H2' from "surface a git OID API" to "key the content cache on the already-present
store path." NEXT STEP: instrument/trace a locked-flake hot eval to see whether closures.gnome source
reads come from the git ODB or from a store-copied tree — that single fact decides the whole hot design.

## C1 — fire-and-forget cold recording: implementation sketch (2026-05-31)

Grounded in: `TraceBackend::record` (context.cc:333-345), `TracePublishScope::publish` +
`evaluateResolvedTarget` (trace-session.cc:59-188), `Recorder::record` (recorder.cc), the blocking pool
(context.cc:247-261, `kBlockingThreads=2`, `BlockingThreadPool::post` already exists, blocking-scope.hh:69).

### What blocks the eval thread today, and what's actually needed synchronously
`publish()` → `backend.record` → `ctx.syncAwait(coroBlock(pool, …store->record…))`. `syncAwait`
block-waits the eval fiber until the whole record (CPU: sort+3 hashes+serialize+intern; I/O: SQLite
write under `storeMutex_`) completes. Of `publish`'s outputs:
- **`result->traceId`** (set into `expr.ensureLazy().traceId`, trace-session.cc:63) — VERIFIED NOT
  consumed on the cold path. The only reads of `lazy.traceId` (trace-session.cc:619-620, 670) are the
  WARM-HIT branch, which gets `cachedTraceId` from the warm lookup, not from this cold record. So the
  traceId is write-only cold → does NOT need to be available synchronously.
- **`publish()`'s bool** (`hasBackendRecord`, trace-session.cc:175) — IS consumed synchronously: it
  gates `materializeResult(v, attrValue)` vs the passthrough `v = *target` (line 183-187). BUT the bool
  is `depCapture.isStable() && (record succeeded)`, and "record succeeded" only fails when no real
  backend is bound (NullTraceBackend) — knowable WITHOUT doing the DB write. And `materializeResult`
  builds `v` from `attrValue` (already in hand, line 173), NOT from the record result.

So the synchronous needs are: (a) is a backend bound? (b) are deps stable? — both known pre-write.
Everything else (hash/serialize/intern/SQLite) can move off the critical path.

### The sketch (C1, env-gated `NIX_TRACE_ASYNC_RECORD=1`, default off)
1. `publish()` computes `bool willRecord = depCapture.isStable() && backend.hasBoundBackend()`
   synchronously (no DB). Returns `willRecord` for the materialize gate. (Matches today's bool for the
   common case; the only behavioral diff is it no longer waits for the write.)
2. Instead of `syncAwait`, hand `(pathId, std::move(value), std::move(deps))` to a new
   `backend.recordAsync(...)` that `pool.post(...)`s the `store->record` work (fire-and-forget). The
   eval continues immediately.
3. **Barrier at session end:** `TraceSession::flush()`/`releaseBackend()` (trace-session.cc:792/803)
   already run "after all eval work completes" (the shutdown comment, context.cc:369). Add a join:
   block until all posted records drain before the final teardown flush. The pool join + a posted-task
   counter (or `asio` work-tracking) is the barrier.
4. `expr.lazy.traceId` is left unset cold (it's write-only cold anyway). If a future cold consumer ever
   needs it, the async result can backfill it — but today nothing does.

### Why this is C1 and not Layer 2b
Layer 2b tried to move the CPU (hash/serialize) off-thread and hit the `DataPathPool` non-thread-safety
wall (`feedKey`→`collectPath` mutates shared pools, recorder.cc:42 + interning-pools.hh single-writer
invariant). C1 has the SAME wall IF the posted task touches the pools concurrently with the eval thread.
So C1 is NOT free of the race — see the adversarial pass. The naive "just post it" is unsound.

## C1 — ADVERSARIAL PASS on the sketch (2026-05-31). Naive fire-and-forget is UNSOUND; refined below.

Hammered the sketch against the code. Findings, severity-ranked:

### BLOCKER — ADV-1: the posted record races the eval thread on the shared InterningPools.
`makeTraceBackend` passes `tracingPools()` BY REFERENCE into the `SqliteTraceStorage`/`Recorder`
(context.cc:179-182). It is the SAME `InterningPools` instance the eval thread keeps interning new deps
into as eval continues (`recordInterned` → `pools.intern`). The posted record does `feedKey` →
`collectPath(key.dataPathId)` (input-resolution.cc:121) and `encodeCachedResult` — `collectPath` is
`const` (reads `dataPathPool.nodes`, a `std::vector`), but a concurrent eval-thread `push_back` can
REALLOC that vector → the record thread reads freed memory. `DataPathPool` is documented "NOT
thread-safe … single-threaded-writer invariant" (interning-pools.hh:296,314). So "just `pool.post` the
record and continue eval" is a DATA RACE / UAF — the SAME wall that blocked Layer 2b. The sketch's
step-2 as written is unsound. THIS is the crux and the sketch's step-2 must change.

### MAJOR — ADV-4: the exception/failed-eval publish (trace-session.cc:139) must NOT be async.
On `EvalError`, `publish(failed_t…)` runs then `throw`. If async, the posted task captures `deps` while
the stack unwinds and the DepCaptureScope/pools may be torn down → use-after-free during unwind. The
failed-eval record is rare (not a hot cost) — keep it SYNCHRONOUS. C1 applies only to the
success-path publish (line 176).

### MINOR — ADV-3: within-session ordering is SAFE (verified).
A later step re-reading a just-recorded cold trace would race an in-flight async write. But: the cold
traceId is write-only (established); within-session re-forces hit the IN-MEMORY `epochMap`/`replayBloom`
(context.cc:982/1067), NOT the store; warm-hit reads come from a PRIOR process. So no within-session
store re-read of a freshly-async-recorded trace. The barrier at session end (sketch step 3) covers
cross-process durability. Ordering is not a blocker — but the barrier MUST run before the teardown
flush, and the teardown flush must see all posted entities (so post into the SAME store the teardown
flushes — it does).

### MINOR — ADV-5: the payload is movable but pool-relative.
`Dep` = interned IDs + owned `DepHashValue` bytes (types.hh:794-804) — no live `Value*`/`Bindings*`, so
`std::move(deps)` into the task is memory-safe. BUT resolving those IDs to bytes (serialize) still needs
the pool → doesn't escape ADV-1.

### REFINED DESIGN (resolves ADV-1) — two options, recommend B.

**Option A — snapshot the pool-dependent material on the eval thread, post only pool-FREE work.**
Before posting, do the pool-touching steps (feedKey/collectPath/serializeKeys/serializeValues/
encodeCachedResult — everything that reads `pools`/`vocab`) SYNCHRONOUSLY on the eval thread, producing
a fully self-contained `RecordPayload{traceHash, fullHash, keySetHash, keysBlob, valuesBlob, payload,
resultHash}` (all owned bytes, no pool refs). Post ONLY the SQLite write (getOrCreate* + publish) — which
touches the DB under `storeMutex_`, not the pools. This removes the I/O + flush (the §3b-measured ~44%
flush + ~24% second-txn) from the eval critical path while keeping the pool-racing CPU on the eval
thread. Net: partial win (removes I/O wait, keeps hash/serialize inline). SOUND (no pool race; the
posted task is pure DB). Smaller than full fire-and-forget but no thread-safety work needed.

**Option B (recommended) — single-writer record queue drained by ONE dedicated thread, with the pool
made safe for one concurrent reader.** Keep ALL record work (incl. pool reads) off the eval thread, but
serialize records onto ONE record thread (not the 2-thread pool) and make `DataPathPool` reads safe
against eval-thread appends. The minimal safe structure: `DataPathPool.nodes` is append-only; a
concurrent reader is safe IF the vector never reallocates under it. Two sub-options:
  (b1) `std::deque` (stable element addresses on append) instead of `std::vector` for `nodes` → a reader
       holding an index is never invalidated by an append. Cheap, surgical, removes the UAF. (collectPath
       walks parent links by id → index access; deque keeps those valid.)
  (b2) reserve-and-cap / chunked storage (the `ChunkedVector` the StringInternTable already uses,
       interning-pools.hh) — same stable-address property.
  With stable addresses, ONE record-reader thread racing the eval-writer is safe for the append-only
  pools (the eval thread only ever APPENDS; it never mutates existing nodes). `governingRepoCache_`
  (the other non-thread-safe member) is written during dep RESOLUTION (verify), not record — confirm it
  is not touched on the record path (it isn't: record reads via collectPath, not resolveRepoRoot). 
  Net: full fire-and-forget cold recording, sound. Larger change (data-structure swap + a record thread
  + the session-end join), but it's the real "cold off the critical path" win.

**Recommendation: B (with b1, the deque swap)** is the real fix; A is the safe partial fallback if b1's
append/read concurrency proves to have a sharp edge (e.g. collectPath touches more than nodes[]).
Validate b1 FIRST with a focused TSan run: eval-thread appends + concurrent collectPath reads on a
deque-backed DataPathPool, under `-Db_sanitize=thread`. If clean, B; else A.

### Adversarial pass on the REFINEMENT itself (don't trust B blindly)
- B's soundness rests on "eval thread only APPENDS to DataPathPool, never mutates existing nodes." If
  any record-relevant pool (Strings, the DepKeySet/Trace/Result intern maps inside the store) is mutated
  by BOTH threads, the deque swap is insufficient. The STORE's own intern maps (getOrCreateDepKeySet/
  getOrCreateTrace) are under `storeMutex_` already (only the record thread touches them) — safe. The
  shared one is `tracingPools` (eval appends, record reads) — the deque covers it. But VERIFY there is
  no OTHER shared-mutable touched by record: `vocab` (AttrVocabStore) — does record WRITE it
  (internName) or only read? `internName` for `__ca:` is §3b; the main record path's `encodeCachedResult`
  → `hasherVocab()` — must confirm read-only. THIS is the remaining unknown to check before B.
- A is unconditionally sound but its win is bounded by how much of the 6.7s is I/O-vs-CPU. The §3b
  split (flush ~44% + 2nd-txn ~24% = ~68% I/O-ish, hash ~26% + serialize ~5% CPU) suggests A removes
  ~68% of the per-record cost from the critical path — already most of it. So A may be the
  better effort/risk trade: ~2/3 of the win, zero thread-safety work. Re-measure the split on the
  MAIN record path (not the §3b producer path) to decide A-vs-B.

### Revised C1 plan
1. Measure the I/O-vs-CPU split of the MAIN `backend.record` path (the §3b split was the producer path;
   confirm it holds for the consumer-trace record). Decides A-vs-B leverage.
2. Implement A first (post-only-the-DB-write): sound, no data-structure change, removes the I/O wait.
   Bench. If A captures most of the 6.7s → done.
3. Only if CPU (hash/serialize) is still a critical-path bottleneck after A → B (deque swap + record
   thread), gated behind the vocab-write check + a TSan validation.
4. Exception-path publish stays synchronous (ADV-4). Session-end barrier before teardown flush (ADV-3).

### C1 refinement — the vocab-write check KILLS Option B's "surgical" framing → recommend A decisively.
Checked the open question (ADV-pass-on-refinement: does the main record path WRITE shared mutable
state beyond DataPathPool?). It does: `encodeCachedResult` for an `attrs_t` result →
`encodeAttrEntries` → `vocab.internName(entry.name)` (trace-result-codec.cc:327) is a MUTATING intern
on the shared `AttrVocabStore`, concurrent with the eval thread's own vocab writes. So Option B is NOT
just "swap DataPathPool's vector for a deque" — it ALSO needs a thread-safe `AttrVocabStore` (or
pre-interning all result attr-names on the eval thread before handoff). That is materially more work +
risk (a second shared-mutable structure on the hot path, the vptr/lock-on-hot-loop hazard class).

CONSEQUENCE: Option A is now the clear recommendation. A does ALL pool/vocab-touching work
(feedKey/collectPath/serialize/encodeCachedResult/internName) SYNCHRONOUSLY on the eval thread —
producing a self-contained `RecordPayload` of owned bytes + already-interned ids — and posts ONLY the
SQLite write (getOrCreate* + publishStateChange), which touches the DB under `storeMutex_`, never the
pools/vocab. No thread-safety work on pools or vocab; sound by construction.

A's win, quantified against the §3b record split (flush ~44% + 2nd-txn ~24% = ~68% is the SQLite I/O
that A moves off-thread; hash ~26% + serialize ~5% stays on the eval thread). So A removes ~2/3 of the
per-record cost from the eval critical path with ZERO concurrency risk. The residual ~1/3 (hash+
serialize CPU) only comes off-thread via B, which now requires the thread-safe-vocab work — defer B
until A is measured and only if the residual CPU is shown to still matter.

FINAL C1 recommendation: implement Option A (post-only-the-DB-write, sync everything pool/vocab-touching),
exception-path stays sync, session-end barrier joins outstanding DB writes before teardown flush.
Env-gate `NIX_TRACE_ASYNC_RECORD=1`, default off. Then bench the cold delta; pursue B only if the
hash+serialize residual is still a measured critical-path cost AND the thread-safe-vocab cost is
justified.

## STEP 1 RESULT — the split INVERTS the C1 plan. The cold cost is HASHING, not I/O. (2026-05-31)

Measured the MAIN record path (cold no-§3b = pure consumer/root traces). Genuinely-cold single
closures.gnome eval (record.count=7 traces, record.timeUs=1.55s):
- **record.hashUs = 57%** (0.89s) — the dominant cost
- record.flushUs = **4%** (I/O)
- serialize = 3%
- 2nd-txn/other = 35% (publishStateChange txn + interning)

This INVERTS the §3b producer-path split (flush ~44%) that the Option-A recommendation rested on. On
the consumer path, the cost is CPU hashing, not SQLite I/O. WHY: ownDepsTotal=172,670 across 7 traces
(max 48,745 deps in one trace) — the 607× flattening, now as a RECORD-side cost. recorder.cc:45-47
computes THREE hashes (traceHash + fullHash + depKeySetHash) each in a SEPARATE full pass over the
~20K-dep sorted vector, and each pass calls feedKey → feedCanonicalDepKeyMaterial → collectPath (pool
resolution) PER DEP. So the expensive pool resolution runs 3× per dep (3 × 48,745 = 146K resolutions
for one trace).

### CONSEQUENCE: Option A is nearly worthless (moves the 4% I/O); the real lever is the HASHING.
- Option A (off-thread the SQLite write) addresses ~4% flush + part of the 35% txn — NOT the 57% hash.
  ABANDON A as the first move.
- Option B (off-thread the hashing) addresses the 57% but (a) hits the pool+vocab thread-safety wall
  and (b) only HIDES the latency — the CPU work still happens, competing with eval. Not the first move.
- **NEW first move — H-cold-1: dedup the 3× redundant key resolution into ONE pass.** The 3 hashes
  feed the SAME key material into 3 separate builders (different domains/ordinals — can't merge the
  builders), but the EXPENSIVE part (feedCanonicalDepKeyMaterial: pools.resolve + collectPath, the
  documented-costly pool walk) is IDENTICAL across all three and currently runs 3×. Resolve each key's
  pool-dependent material ONCE into a reusable `ResolvedKeyMaterial` (resolved source string, collected
  path nodes, resolved hasKey/dirSet), then feed it cheaply into all 3 builders. Eliminates 2/3 of the
  collectPath/resolve work. BYTE-PRESERVING: the same bytes are fed in the same order into each builder;
  only the resolution is deduplicated → the 3 hashes are unchanged → NO schema break, NO cache
  invalidation. Pure CPU win, no async, no threading, no soundness surface. THIS is the tractable cold
  win step-1 revealed.

### ADVERSARIAL on H-cold-1 (before implementing)
- MUST stay byte-identical (the hashes are persisted content addresses; any change invalidates every
  cached trace). The per-hash DIFFERENCES are all cheap and stay per-builder: traceHash skips
  `!contributesToTraceHash(kind)` deps + its own ordinal sequence (TraceHashV2 domain); fullHash = all
  deps + value; depKeySetHash = all deps, no value. The SHARED expensive part is feedKey(dep.key). So
  the refactor is: compute ResolvedKeyMaterial once per dep; each builder still makes its own
  builder.field() calls (preserving domain/ordinal/value framing) but from the pre-resolved material
  instead of re-walking the pool. Verify by a golden-hash test: same (traceHash, fullHash, keySetHash)
  before/after on a fixture trace.
- RISK: feedCanonicalDepKeyMaterial interleaves pools.resolve(...) with builder.field(...) — the
  resolution and the framing are entangled in the current code. The refactor must SPLIT them: a
  `resolveKeyMaterial(pools, key) -> ResolvedKeyMaterial` (pool reads only) + a `feedResolved(builder,
  material)` (builder.field only, no pool). Mechanical but touches the hot hash path — measure it
  didn't regress single-hash cost.
- RISK: the 35% "2nd-txn/other" includes interning (getOrCreateDepKeySet/getOrCreateTrace/doInternResult)
  which ALSO walks the deps — H-cold-1 doesn't touch that. After H-cold-1, re-measure; the 35% may
  become the new dominant term (then C1-async or interning dedup is the next lever).

### REVISED cold plan (replaces "implement A then B")
1. DONE: measure the split → it's hashing (57%), not I/O.
2. **H-cold-1: single-pass key resolution** (split resolve from feed; resolve once, feed 3×). Sound,
   byte-preserving, no threading. Golden-hash test + cold re-bench.
3. Re-measure. If the residual (the 35% txn+interning, or remaining hash) still dominates → THEN
   consider C1-async (Option A for the I/O txn) or interning-dedup. Decide from data, not the §3b split.
