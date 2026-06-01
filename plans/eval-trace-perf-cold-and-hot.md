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

### H1. PERSIST the content-hash cache across processes (biggest, most direct win — and the CONFIRMED hot design as of 2026-06-01)
The 29K re-hashes exist only because `fileContentHashCache` dies with the process. Persist
`SourcePath → DepHash` (keyed by a stable path identity + a freshness token) so the warm process reads
hashes instead of recomputing. The eval-trace SQLite store already persists far more; a
`(path_identity, freshness) → content_hash` table is a small addition.

**The freshness key is RESOLVED (see the "RESOLVED 2026-06-01" block at the end of the HOT section):**
flake source is eagerly store-copied to `/nix/store/<narhash>-source` (no lazy-trees), read through
`storeFS`/`LocalStoreAccessor`. **The store path is ITSELF the content address** — so the key is the
DepSource-root store path + relative subpath (exactly what the dep keys already record: `dsrc2root` +
`/data.txt`), and freshness is automatic: a content change ⇒ a different store path ⇒ a cache miss by
construction. No mtime (H2, rejected), no git OID (H2', unnecessary on this fork), no schema OID field.
For the dirty/`-f /abs` non-store path the store-path key is absent → fall back to keying on the
content-hash itself (still removes the cross-process redundant re-hash within a stable tree).
- Leverage: removes ~0.12s+ of the 0.40s verify directly; more on file-heavier workloads.
- Risk: storage growth (bounded by distinct source files). Soundness is by-construction for the
  store-path-keyed case (store path = content address); the only care is the non-store fallback key.

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

### H2'. git blob OID as the freshness token — SUPERSEDED 2026-06-01 by the store-copy finding (see "RESOLVED" block below)
**On this fork H2' is unnecessary: flake source is store-copied (no lazy-trees), so the store path is
already the content address — H1 keyed on it gets cross-commit reuse for free, no git OID needed.** H2'
would only apply on a lazy-trees fork or the dirty/`-f /abs` non-store path. Retained below as the
analysis of why a git-OID token *would* be the answer IF reads came from the ODB — they don't. Original
framing (content-addressed, checkout-stable):
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

## H1 — LANDED 2026-06-01 (commit on `vibe-coding/file-based-eval-cache`)

**Status: implemented, tested, adversarially reviewed, committed + pushed.** The design below was
realized exactly as specified. Summary of what shipped:
- New SQLite table `FileContentHashes(store_path TEXT PRIMARY KEY, content_hash BLOB)` mirroring the
  `DirSets` shape (additive, no `kSchemaEpoch` bump — pure accelerator, not trace identity).
- `SqliteTraceStorage::{lookupFileContentHash,putFileContentHash}` + in-memory
  `fileContentHashByStorePath` (bulk-loaded at open) + `pendingFileContentHashes` (drained in flush).
- `h1StorePathKey` gate + the H1 block in `resolveCurrentDepHash` (verifier.cc) — in FRONT of the L1
  miss path; ZERO change to the free `resolveDepHash`. On a hit it mirrors `cacheComputedHash` into L1.
- Counters `depHash.fileContentCache{Hits,Stores,Eligible}` in NIX_SHOW_STATS.
- Unit tests `store/file-content-cache.cc` (3, NON-VACUITY PROVEN by neutering the lookup) + functional
  Test 5 in `flakes/eval-trace-soundness.sh` (pins store-path immutability: a source edit → new store
  path → clean miss, no stale serve).
- **Measured firing** on a real flake hot eval: `fileContentCacheHits=2, eligible=2, stores=0` on the
  warm run (both flake source files served from the persisted cache, zero re-reads), output
  byte-identical to `--no-eval-trace`. Adversarial review: no soundness hole (every Registered key
  resolves to a NAR-content-addressed flake path or narHash-verified runtime root).
- **POPULATION TIMING (empirical, important):** H1 is a VERIFY-side cache. Cold RECORDING does NOT go
  through `resolveCurrentDepHash` (it records via `recordFileBytesDepViaCache`), so cold stores NOTHING
  into H1. Per-process counters on a fresh flake:
  - process 1 (cold record): `eligible=0 stores=0 hits=0` — verify path not exercised.
  - process 2 (FIRST warm verify): `eligible=2 stores=2 hits=0` — first run of the verify path
    computes + STORES the hashes (populating the table), no hit yet.
  - process 3+ (subsequent warm verify): `eligible=2 stores=0 hits=2` — served from the persisted cache.
  So **H1 pays off from the 2nd warm verify onward**, not the 1st. For the bench (cold→hot, 2 processes)
  this means the FIRST hot process still recomputes (it's the populating run); a 2nd hot process would
  show the win. The functional test (Test 5) pins exactly this: store on warm-1, hit on warm-2.
- **H1b — LANDED 2026-06-01 (closes the warmup lag; makes the FIRST hot eval hit).** The cold RECORD
  path already computes `depHash(readFile())` (the dep carries it), so H1b populates `FileContentHashes`
  from there for store-resident Registered-source FileBytes/RawBytes deps — the entry exists after cold,
  so the FIRST warm verify HITS instead of recomputing. Wiring: `Verifier::
  populateFileContentCacheFromRecordedDeps(ea, deps)` (uses the bound `registry_`/`state_` + the SAME
  `h1StorePathKey` gate the verify path uses, so keys match by construction) called from
  `TraceBackend::record` (context.cc) inside the same `withExclusiveAccess` after `store->record`. New
  counter `depHash.fileContentCachePopulated` (record-side inserts; disjoint from the verify-side
  Eligible/Stores/Hits). `putFileContentHash` now returns `bool` (inserted) for honest counting.
  - **MEASURED per-process** (release binary, 2-file flake): process 1 cold → `populated=2`; process 2
    FIRST warm verify → `hits=2, stores=0` (previously `stores=2, hits=0` — needed a 3rd process). The
    cold→hot transition now wins on the first hot eval. Soundness preserved: a source edit (new store
    path) still cleanly misses (Test 5 step 4).
  - **ADVERSARIAL REVIEW: SOUND, no holes.** The crux (record-side key/hash must match the verify side)
    is closed: (A) same registry/pools/store instances within a process → byte-identical keys, and
    cross-process keys are globally-unique store paths (no content collision); (B)+(C) the round-trip
    `registry.resolve(resolveDepPathKey(path)) == path` is content-EXACT for store-resident Registered
    sources — carrier root is required store-backed (flake.cc throws otherwise), phase-2 reads
    physically from `evaluationRoot` via the same storeFS accessor (flake.cc:474-484), and no lazy-trees
    on this fork → record-side `sp` and verify-side `resolve(key)` read byte-identical content;
    immutability closes the rest. The dep.hash recorded into the trace is unchanged (H1b is purely
    additive after `store->record`). One noted non-issue: the DEFAULT-OFF §3b CA-producer `recordSync`
    path doesn't pre-populate H1 (effectiveness gap, not soundness).
  - Test 5 (functional) updated to the H1b shape: cold record `populated>=1`, FIRST warm verify
    `hits>=1`. Unit + full suite (1858 pass / 4 documented skips / 0 fail) green.
- **NEXT (still open):** bench H1+H1b on the Ledger-D `closures.gnome` flake workload to quantify the
  hot-wall-time Δ at 170K-dep scale. The unit/functional firing proves correctness + that it fires on
  the first hot eval; the aggregate wall Δ on the real workload is the remaining measurement.
- **NOT yet bench-measured on the Ledger-D `closures.gnome` flake workload** — the next step to quantify
  the hot-wall-time win at scale (the unit/functional firing proves correctness + that it fires; the
  aggregate Δ on 170K-dep traces is unmeasured). See "Recommended order" below.

Original design (realized as-is):

### H1 — IMPLEMENTATION DESIGN (2026-06-01, after the store-copy resolution + 3-agent code map)

**The gap, pinned in code.** The warm-verify FileBytes/RawBytes path does NOT consult ANY content-hash
cache. `dep-resolution-service.cc:371-377` calls `computePathHashedDep(... [](p){ return
depHash(p.readFile()); })`, and `computePathHashedDep` (`:281-299`) is an unconditional
`resolve→maybeLstat→readFile→depHash` — no cache lookup. The per-process `fileContentHashCache`
(eval.cc:83-107) is consulted ONLY on the RECORD path (`recordFileBytesDepViaCache`,
`EvalEnvironment::readFile`); it is structurally unreachable from the verifier. The 29K hot
"cacheMisses" are L1 (`VerificationSession::currentDepHashes_`, keyed on the LOGICAL dep key) first-touch
misses (`verifier.cc:223`); each FileBytes one among them ⇒ exactly one disk re-read at
`dep-resolution-service.cc:294`. L1 only dedups repeat lookups of the same key WITHIN one session; it's
empty each process and never content-addressed. **⇒ H1 = a PERSISTED, content-addressed
store-path→depHash table that the FileBytes verify path consults before reading.**

**The key (RESOLVED, with a correction to the agent's first cut).** Key = the resolved **store-path
string** `p.path.abs()` (= `/nix/store/<narhash>-source/<relpath>`), where `p =
resolver.resolve(source, key)` at the verify site. NOT `(DepSource, key)`: a `fromNodeKey` DepSource is
the lockfile node NAME (`"nixpkgs"`), stable across content edits (types.hh:535-541) → not
content-addressed → unsound as a cache key. The store-path string embeds the NAR content hash → changes
iff content changes → content-addressed AND immutable.

**GATE — must be "path is under /nix/store", NOT getFingerprint().** The agent suggested gating on
`p.accessor->getFingerprint(p.path).second.has_value()` (the `srcToStore` precedent). That is WRONG
here: storeFS returns `nullopt` for getFingerprint (verified this session — MountedSourceAccessor →
LocalStoreAccessor → posix → base default nullopt), so that gate would reject exactly the store paths we
want. The correct gate is **`state.store->isInStore(p.path.abs())`** — store objects are immutable by
Nix's core invariant, so a hash keyed on the full store-path string is sound FOREVER (no freshness
token, ever; a content change yields a different store path = a different key = a clean miss; stale
entries are harmlessly orphaned). Non-store paths (AbsoluteDepSource `getFSSourceAccessor()`, dirty
`-f /abs`) are NOT cached — they fall through to the existing read path (same conservatism as
`srcToStore`'s dirty-accessor bypass). This is why H1 helps the FLAKE/store workload (the
production-relevant repeated-eval case) and is a safe no-op for the `-f /abs` bench — which is correct,
not a limitation.

**Persistence layer.** New SQLite table `FileContentHashes(store_path TEXT PRIMARY KEY, content_hash
BLOB NOT NULL)` in the eval-trace store, mirroring the `DirSets` shape (text PK + blob, `INSERT OR
IGNORE`, eager bulk-load into an in-memory map at open, drained in `flush()`). **No kSchemaEpoch bump:**
the table is purely additive (schema is `CREATE TABLE IF NOT EXISTS`, runs every open,
lifecycle.cc:284) AND correctness-independent — it is a pure accelerator (a miss re-reads; an entry is
never stale because store paths are immutable), so it does NOT participate in trace identity / the
session-key fold-in. An old DB simply gains an empty table; a new DB read by old code ignores it. (Bump
would be required only if it fed verification OUTCOME — it does not; it only feeds the HASH VALUE, which
is then compared exactly as before.)

**Wiring (minimal blast radius — the cleanest seam).** All H1 logic lives in
`SqliteTraceStorage::resolveCurrentDepHash` (verifier.cc:212) — a METHOD ON THE STORE, so it already has
the new cache methods, `registry`, `pools`, `state.store`, and runs inside `ExclusiveTraceStorageAccess`.
ZERO changes to the free `resolveDepHash`, its friend decl, or template instantiations. Shape: for a
FileBytes/RawBytes dep, resolve the SourcePath, if `isInStore` → check the persisted map (hit: return
stored depHash, no read); on miss, compute as today via `resolveDepHash`, then if `isInStore` persist
`(abs, hash)`. Non-FileBytes and non-store deps are untouched. The L1 (`currentDepHashes_`) and
subsumption logic in `resolveDepHash` stay exactly as-is — H1 sits in FRONT of L1's miss path, keyed on
the physical store path instead of the logical dep key, and is the cross-process layer L1 never was.
- Leverage: removes the ~29K re-reads on the production flake hot path (the `depHash.contentUs` ~0.12s
  plus the readFile syscalls + L1-miss recompute behind them). Bench on a FLAKE workload (the `-f` bench
  won't show it — store-copy only happens for flake/git inputs; `-f /abs/nixpkgs` reads posix, not
  store). Soundness: store-path immutability (no token), pinned by a record→evict-L1→verify test.

## Recommended order (tractable → architectural) — REVISED 2026-06-01 (H1 LANDED)
1. **H1 (persist store-path→depHash, gated on isInStore)** — ✅ LANDED + committed (correctness proven,
   fires on real flakes). NEXT for H1: bench it on the Ledger-D `closures.gnome` flake workload to
   quantify the hot-wall-time Δ at 170K-dep scale (warm verify re-hashed ~29K source files/process; H1
   should erase most of `depHash.contentUs` + the readFile syscalls behind it on the 2nd+ process).
2. ~~H2' (git blob OID)~~ — RETIRED on this fork (no lazy-trees; store-copy makes the store path the
   token). Only relevant to a lazy-trees fork or the dirty/`-f /abs` path (where H1's isInStore gate
   declines and the read path stands).
3. **C1 (fire-and-forget recording)** — the cold fix; removes recording from the eval critical path.
   Accessor-independent (helps every workload).
4. **H3 / C2 / C3** — architectural follow-ons.

REVISED framing: "hot instant" for REPEATED FLAKE evals (the production-relevant case) = H1+H2' (git-OID
content cache). The bench's `-f /abs/path` posix path is a measurement artifact that CANNOT benefit from
any content-stable token (no git awareness, mtime-churned) — to measure the hot win we must use a
flake/git+file workload where the accessor exposes OIDs. C1 (cold) is the accessor-independent win.

~~OPEN QUESTION before implementing~~ — **RESOLVED 2026-06-01 (from code + strace; the answer kills H2'
and confirms H1).** The question was: does a locked-git-input flake hot eval read each source file
through `GitSourceAccessor` with a cheap per-file OID lookup (→ H2' ODB lookup), or from an eagerly
store-COPIED tree (→ store path IS the content address, H1 keyed on store path, no git)? **Answer: the
latter — store-copied, read through `storeFS`, NOT GitSourceAccessor, NOT the bare PosixSourceAccessor.**

Resolution chain (all grounded in code; the user's framing question was "is it using the posix accessor
or something else?"):
1. **No lazy-trees on this fork** (`nix config show` shows no such setting). The `git+file://` flake
   source is EAGERLY `Input::fetchToStore`'d (fetchers.cc:202) into `/nix/store/<narhash>-source` — a
   real materialized dir (verified: `/nix/store/asi31…-source/{data.txt,flake.nix}`, mode `r--r--r--`,
   mtime epoch-1). strace of `nix eval git+file://$repo#val` showed 0 non-`.git` workdir source opens
   and a `/nix/store/*-source` copy → eval reads the COPY, not the workdir, not lazily from the ODB.
2. **The eval reads `./data.txt` through `storeFS`, not posix.** For a flake, `rootFS` takes the
   pure-eval branch `accessor = storeFS` directly (eval.cc:463) — flake eval is always `pureEval`
   ([[project_locked_rev_deferral_design]]). The `getFSSourceAccessor()` (= bare `PosixSourceAccessor`)
   upper union layer (eval.cc:464) is ONLY used for impure `-f /abs/path` (what the `-f` bench used).
   `storeFS = makeMountedSourceAccessor({…, {storeDir, store->getFSAccessor(pureEval)}})` (eval.cc:431/451);
   `LocalFSStore::getFSAccessor` returns a **`LocalStoreAccessor`** (local-fs-store.cc:127) — posix is
   the bottom I/O layer, but the accessor the eval HOLDS is the mounted `LocalStoreAccessor`, gating
   every read through `requireStoreObject` (store-path validity), wrapped in `makeCachingSourceAccessor`
   + (pure) `AllowListSourceAccessor`.
3. **No accessor fingerprint, but none is needed.** `MountedSourceAccessor::getFingerprint`
   (mounted-source-accessor.cc:100) forwards to `LocalStoreAccessor::getFingerprint` → inner posix →
   base default `nullopt` (source-accessor.hh:221). The `accessor->fingerprint = getFingerprint(store)`
   writes (fetchers.cc:329/364) land on the FETCHER's accessor DURING fetch, not on the eval's `storeFS`
   view of the copied result. So there is NO git-OID token at FileBytes-verify time through this
   accessor. **BUT the store path is itself the NAR-hash content address** — `/nix/store/<narhash>-source`,
   identical across commits for identical content. The eval-trace dep keys already record source as a
   `dsrc2root` DepSource + relative subpath (`/data.txt`, `/flake.nix`), resolved by SemanticRegistry to
   that store path — NOT a raw posix path, NOT a git OID.

**Consequence — H2' is UNNECESSARY (retire it), H1 is the design:** the hot fix is a PERSISTED
content-hash cache keyed on the store-path/DepSource content identity `(DepSource-root-store-path,
relative-subpath) → DepHash`. Stable across commits for unchanged content FOR FREE, because the store
path already is the content address — no git-OID API, no `GitSourceAccessor` surfacing, no schema OID
field, and the git ODB is never on the hot read path. H2' would only matter on a lazy-trees fork (we
don't have one) or the dirty/`-f /abs` non-store path (where H1-keyed-on-content-hash is the only lever
anyway). NEXT: design H1's persisted table + the verify-time freshness check (store-path identity is the
freshness key; the recorded depHash is the cached value).

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

## H-cold-1 IMPLEMENTED + MEASURED (2026-05-31) — real but small (−10% cold); the bulk is BLAKE3, not resolution.

Implemented: split `feedCanonicalDepKeyMaterial` into `resolveDepKeyMaterial` (pool reads: pools.resolve
+ dataPathPool.collectPath + encoded-blob build) + `feedResolvedDepKeyMaterial` (builder.field only),
and a fused `computeRecordHashesFromSorted` that resolves each dep key ONCE and feeds all 3 record
builders (traceHash/fullHash/depKeySetHash) in a single pass. `recorder.cc` uses it. Extracted
`DataPathNode` to a minimal `data-path-node.hh` to break an include cycle
(interning-pools.hh→input-resolution-internal.hh→input-resolution.hh).

BYTE-PRESERVATION: VALIDATED — 1349 eval-trace unit tests + 5 cross-process warm-hit functional suites
pass. Warm verify recomputes hashes and compares to the recorded ones; any byte drift → universal warm
miss → test failures. None. (The fused output is byte-identical to the 3 separate passes by
construction: same builders, same field order, same per-hash filtering/ordinals/value-inclusion.)

PAYOFF (genuinely-cold closures.gnome, 2 trials): record.hashUs **0.889s → 0.75s (−15%)**, total
record.timeUs **1.55s → 1.39s (−10%)**. Real, sound, zero-risk.

### ADVERSARIAL on the payoff — why only −15%, and what it means
hashUs = resolution (collectPath+pools.resolve, now 1× — H-cold-1's target) + builder.field framing +
BLAKE3 finish (still 3×, irreducibly). The −15% IS the deduped resolution; the remaining ~85% is the
3× BLAKE3 over ~146K framed fields (48,745 deps × 3 builders). Fusion CANNOT cut that — the 3 hashes
are distinct (different domains/content) and each must be computed. So H-cold-1 captured its target but
the target was a minority of hashUs.

CONSEQUENCE: H-cold-1 is worth keeping (−10% cold, byte-identical, no risk) but is NOT the cold
breakthrough. The bulk cold cost is BLAKE3 throughput over the FLATTENED 48K-dep vectors — the 607×
again, now on the record/hash side. To cut it materially you must either:
- **C3: hash LESS — reduce the flattened dep COUNT** (record-time dedup of identical dep sub-vectors
  across the 7 traces; the 172,670 total deps over 7 traces means massive overlap). This attacks the
  root (count), not the constant. Biggest remaining cold lever; unexplored; needs design.
- **C1/B: move the 3× BLAKE3 off the eval thread** — the pool+vocab thread-safety wall (now also the
  vocab.internName write, established earlier). Hides latency, doesn't reduce work; harder.

H-cold-1 is the cheap sound first cut; the real cold win is C3 (reduce dep count) — which is the SAME
flattening the producer-partition arc failed to fix via edges, approached differently (record-time
sub-vector dedup, not consumer edges). That's the next design.
