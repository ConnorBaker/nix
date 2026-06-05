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

## ADVERSARIAL PASS + ARM RE-RANKING (2026-06-01) — H1-for-`-f` is REFUTED; the hot cost is verify COORDINATION, not file hashing

**Trigger.** After H1/H1b landed, the plan was "bench H1 on a flake workload." User pushback: *"Why do
you need a flake-shaped workload? Our evaluation cache should work for all inputs."* Correct — H1's
store-path gate (`isInStore`) means it fires ONLY for store-resident (flake) paths; on `-f /abs/nixpkgs`
(posix accessor, mutable path) it is inert. The proposed fix was H2' (git blob OID freshness token,
already in this doc) extended to `-f` git trees. This section adversarially tests that proposal with
measurement. **Verdict: the proposal targets ~0.3% of the hot cost and is dropped. The real lever is the
verify execution model.**

### Measurement 1 — H1 is provably inert on `-f` (confirms the gap, refutes "flakes only" as the answer)
`-f closures.gnome`, HEAD binary, cold→hot→hot, isolated cache:
- EVERY run: `fileContentCacheEligible = 0` (H1 never fires on posix paths — gate declines).
- Warm runs: `depHash.contentUs ≈ 0.11s`, `cacheMisses = 23,192`, `verify.depsChecked = 128,028`,
  `hits=7 misses=0`. The overall cache SERVES correctly on `-f`; only the file-rehash *avoidance* is absent.

### Measurement 2 — matched hot decomposition (wall + cpuTime + verify, one run)
`-f closures.gnome` warm: **wall = 0.96s, cpuTime = 0.47s ⇒ ~0.49s (51%) is NON-CPU wait.**
- `verify.timeUs = 0.30s` (≈64% of CPU); within it `contentUs = 0.125s` (the H1/A target), `structOuterUs
  = 0.047s`, `directoryUs = 0.015s`. `loadTrace = 0.0045s`, `db.init = 0.024s`, `record = 0`, `recovery
  = 0`. Eval itself is trivial (`thunks.forced = 7`, `nrThunks = 1`) — the cache serves the value; the
  0.96s is almost entirely verify + coordination, not evaluation.
- `--no-eval-trace` forces release.nix to its top attrset in **0.01s** vs **0.63s** with eval-trace —
  i.e. the eval-trace machinery, not parsing, owns the non-verify time too.

### Measurement 3 — `strace -f -c` decomposes the 0.49s wait (THE decisive datum)
| syscall | %time | calls | meaning |
|---|---:|---:|---|
| futex | 73.7% | 874 | blocking/contention on the verify pools (few calls, long waits) |
| sched_yield | 16.5% | **26,334** | **spin-waiting** in the async machinery |
| newfstatat | 3.5% | **177,290** | per-dep `lstat` (the 128K-dep walk + path-resolution probing) |
| getdents64 | 3.3% | 75,298 | `DirectoryEntries` deps |
| pread64 | **0.28%** | **8,167** | **the actual file-content reads — all H1/A can ever remove** |

Caveat: `strace` serializes threads and INFLATES futex/sched_yield (unstraced wall is 0.96s; strace shows
5.86s futex alone). So the 74%/16% are distorted upward. **What is NOT inflated: the call counts.** File
content I/O is 8,167 reads (`pread`) — tiny. The walk is 177K stats. Spinning happens 26K times. The
qualitative split is robust regardless of strace distortion: **file hashing is a rounding error; the cost
is the verify walk executed through a multi-thread async machinery.**

### Architecture (code-confirmed) — why coordination dominates
`SqliteTraceStorage::verifyTrace` (verifier.cc:2192-2280) does ~7-8 `coroBlock` calls PER TRACE (×7
traces ≈ ~50 handoffs); inside the big `verified = co_await coroBlock(…)` the 128K-dep walk runs
SYNCHRONOUSLY on one BlockingThreadPool thread (`runPass1`/`runPass2` loops over `fullDeps` calling
`resolveCurrentDepHash` per dep — verifier.cc:685/728/775/801…). Execution model (context.cc:247 +
eval-context.hh:169 + blocking-scope.hh:124): eval thread → `syncAwait` (`co_spawn` onto a 2-thread
`io_context`, then `future.get()` blocks) → verify coroutine on an io_context worker → `coroBlock` posts
each blocking op to a 2-thread `BlockingThreadPool` → result `post`ed back to the io_context executor.
So FOUR thread groups (eval + 2 io_context + 2 pool) coordinate around a walk whose real I/O is 0.28%.
The 26K `sched_yield` is the io_context/asio scheduler spinning while idle workers await cross-thread
completion posts. (Doc inconsistency found: fiber-scheduler.hh:8 says syncAwait "polls inline" but the
code is `future.get()` (blocking) — the comment is stale; the spin is in the asio worker idle/handoff,
not syncAwait. Pin precisely with an off-CPU profile.)

### Arm re-ranking (measured ceilings, confidence)
- **A — H1/H2' for `-f` (git-OID freshness): DROPPED.** Ceiling = the 8,167 `pread`s + `contentUs`
  (0.125s CPU) = **~0.3% of hot syscalls / ~13% of CPU**, but the wall is dominated by coordination it
  does not touch. PLUS the soundness refinement I claimed is wrong: the git index OID is the STAGED
  blob; for a DIRTY work-tree file it is stale, so trusting it requires git's racy-clean check
  (mtime/size/inode vs the index's cached stat) — which RE-INTRODUCES exactly the mtime trust class H2
  was rejected for. So A is *both* low-value *and* not "sound by construction" on a work tree. (H1 for
  store/flake paths already landed and is fine; do NOT extend to `-f`.) Confidence: HIGH.
- **H4 — reduce the verify EXECUTION-MODEL coordination overhead.** The real hot lever, input-agnostic
  (flake + `-f`), pure perf (no cache-semantics/soundness surface). Two shapes:
  - **H4a (synchronous/low-thread hot verify):** on a warm hit the I/O is page-cached (0.28%), so the
    async-overlap machinery's coordination cost exceeds the I/O it hides. A synchronous (or 1-thread)
    verify path for warm hits should erase most of futex+sched_yield. Hypothesis; needs the A/B below.
  - **H4b (collapse handoffs):** fewer io_context/pool threads, or coalesce the ~50 per-trace coroBlocks.
  Confidence the lever is here: HIGH. Confidence on the specific fix/size: MEDIUM (needs profile + A/B).
- **H3 — short-circuit the 128K-dep walk.** Attacks the dep COUNT, which removes the 177K stats AND
  shrinks the coordination (fewer items). Complementary to H4. Bigger ceiling but soundness-hard
  (certificate-before-load refuted in naive forms; needs the per-current-node eligibility-bit shape from
  README lever-3). Confidence: MEDIUM; effort: HIGH.

**Priority: H4a > H3 > A(dropped).** H4 is pure-perf, input-agnostic, and attacks the measured 90%; it
also helps COLD (record uses the same syncAwait/coroBlock machinery). H3 is the deeper but harder
count-reduction. A is closed.

### Additional analysis REQUIRED before committing to H4
1. **Off-CPU profile WITHOUT strace** (`perf record -g` / `perf sched` / off-CPU flamegraph) of the warm
   `-f` hot run, to get the true futex-vs-sched_yield-vs-stat split and pin the spin (asio idle-worker
   spin? cross-thread post wakeups? io_context queue-mutex contention?). strace's inflation makes the
   absolute split unreliable.
2. **H4a A/B spike:** an env-gated synchronous-verify path (walk deps + stat/read/hash inline on the
   eval thread, no io_context/BlockingThreadPool/coroBlock) vs the current async path, measured on the
   warm `-f` hot. Directly sizes the coordination overhead and tests "the async machinery is net-negative
   on warm hits." Cheapest decisive experiment; also try `kBlockingThreads=1` / io_context single-thread
   as a one-constant probe.
3. **Cold check:** confirm record's syncAwait/coroBlock pays the same coordination tax (it should), so
   H4 is scored against cold (6.7s) too, not just hot.
4. Resolve the fiber-scheduler.hh "polls inline" vs eval-context.hh `future.get()` doc inconsistency.

**Net:** the "make the cache work for all inputs" answer is NOT extending H1 to `-f` (that optimizes
0.28%); it is fixing the verify execution model (H4), which is input-agnostic and attacks the measured
bottleneck. Pending the off-CPU profile + the synchronous-verify A/B before committing.

### EXPERIMENT 1 RESULT (2026-06-01) — perf CORRECTS the strace section: cost is STATS + HASHING; concurrency is ZERO-benefit
perf 6.19 (paranoid lowered via sudo), warm `-f closures.gnome`, RELEASE binary (libnixexpr symbols
resolved from the in-store `-debug` outputs). **This supersedes the strace-based ranking above on two
counts — the strace split was a thread-serialization artifact.**

**On-CPU `perf record` self-time (real, un-inflated):**
| theme | on-CPU | strace had |
|---|---:|---:|
| blake3 hashing | 13.6% | (invisible — pure CPU, no syscall) |
| tbb:: (parallel-hash coordination) | 13.2% | — |
| malloc/gc/memcpy | 13.0% | — |
| stat/getdents | 4.6% | 6.8% |
| futex/mutex | **2.6%** | 73.7% |
| sched_yield | **0.1%** | 16.5% |
| asio/io_context | 0.2% | — |

On-CPU coordination (futex+yield+asio) ≈ **3%**, NOT 90%. strace inflated the syscall WAITS by
serializing threads. blake3 caller (resolved): `blake3_hash_many_avx2 ← TBB ←
BlockingThreadPool::Work<coroBlock<verifyAttrImpl::lambda#4>> ← SqliteTraceStorage::verifyTrace` — the
verify re-hashing content/trace, TBB-parallelized inside the pool task; TBB burns ~as much CPU
coordinating (13.2%) as hashing (13.6%).

**`time -v` decomposition (no idle):** wall 0.94s = **user 0.45s + sys 0.40s**. The earlier "0.49s
off-CPU wait" was a MISREAD — the matched run's `cpuTime`=0.47 was USER-only; the "gap" is SYS time
(0.40s ≈ the 177,290 `newfstatat` + 8K reads + getdents). Minimal true idle.

**Concurrency delivers ZERO wall benefit (decisive `taskset`):**
| config | wall |
|---|---:|
| NORMAL (32 CPU: asio pool + BlockingThreadPool + TBB blake3 + parallel GC) | 0.94–0.97s |
| `taskset -c 0` (1 CPU, fully serial) | **0.92–0.94s** (slightly FASTER) |
| `taskset -c 0-1` (2 CPU) | 0.92–0.93s |

The entire multi-thread machinery is net-neutral-to-negative on warm hot — coordination overhead ≈ the
parallel speedup.

**CORRECTED arm ranking (supersedes the strace-based ranking):**
- **H3 (reduce the dep walk): #1.** `sys` 0.40s (≈43% of wall) is 177,290 `newfstatat` — per-dep stat
  + **35,931 ENOENT** path-resolution probing. The single biggest cost. Cutting the walk attacks it
  directly; the 35K failed path-probes are a separately-cacheable sub-lever. Soundness-hard, biggest payoff.
- **H4 RE-SCOPED to "REMOVE the async machinery," not "fix coordination": #2, cheap.** Since 1-CPU ==
  32-CPU wall, a SYNCHRONOUS single-thread verify is as fast (likely faster), simpler, and deletes the
  futex/TBB/asio overhead + off-thread complexity. Low-risk simplification. Bundle with **serial blake3**
  (disable TBB for hashing — 13.2% coordination is wasted; serial is same-speed at these sizes).
- **H1/A (file-hash cache): #3, smaller than H3 but NOT 0.28%.** strace's `pread=0.28%` hid that the
  file cost is the blake3 HASHING (CPU, ≈13% of user), which H1/A removes — but it is smaller than the
  177K-stat `sys` cost, and the `-f` freshness complication (dirty-file → stat trust class) stands.
  Reconsider after H3/H4.

**My strace-driven adversarial pass was wrong on two counts:** (1) it dismissed H1/A on `pread=0.28%`,
but the file cost is CPU hashing (invisible to strace); (2) it proposed "fix the coordination," but
on-CPU coordination is ~3% and the concurrency is *removable* (zero benefit). The real levers are **H3
(the 177K-stat walk)** and a **synchronous-verify simplification (+ serial blake3)**. Experiment 2
(synchronous-verify A/B) is now strongly pre-justified by the `taskset` result — it should match wall
while removing the entire async/TBB apparatus.

### EXPERIMENTS 1+2 (the spikes) — DONE 2026-06-01. See `plans/verify-execution-model-spike.md`.
- **Spike 1 (synchronous verify, `NIX_EVAL_TRACE_SYNC_VERIFY=1`):** byte-identical output +
  identical counters + SAME wall (0.93 vs 0.94s) as async. The io_context/coroBlock/prefetch
  apparatus is **removable with zero warm regression** — confirmed as a real code path, not just
  a `taskset` artifact. Win = simplicity, not wall.
- **Spike 2 (parallel ceiling, proxy micro-bench):** stat+read+hash parallelizes ~5–8x, but the
  dominant STAT cost plateaus at **~4x** (VFS contention) → realistic **~2x hot ceiling**
  (0.94→~0.45s), gated on a concurrent-L1 thread-safety refactor (the L1 dedups the 607×
  duplicates, so it can't be skipped). Real but bounded; H3 (cut the dep count) is more
  fundamental.
- **Synthesis:** async pays only for COLD disk I/O; the WARM path wants data-parallelism (or
  fewer deps). Current design applied the cold tool to the warm path. Decision point (uncommitted):
  (1) land Spike-1 simplification, (2) H3 root-cause, (3) parallel-verify refactor for ~2x warm.

### STAT-SOURCE PROFILE (2026-06-01) — the 177K stats are GIT, not verify. CORRECTS Experiment 1; introduces G1.
`perf record -e syscalls:sys_enter_newfstatat -g` (under sudo for complete tracefs metadata; the
unprivileged record produced "broken trace data"), warm `-f closures.gnome`, 176,935 stat samples:

| call site | % of stats |
|---|---:|
| `getOrCreateTraceCache -> observeGitIdentity -> computeGitIdentityHash -> GitRepoImpl::getWorkdirInfo -> libgit2 git_status` | **89.4%** |
| `TracedExpr::eval -> TraceBackend::verify -> verifySync -> SqliteTraceStorage::verify(Trace)` | **~5–8%** |

**CORRECTION to EXPERIMENT 1 above:** I attributed `sys` 0.40s (the 177K `newfstatat` + 35K ENOENT
+ 75K getdents) to "the per-dep verify stat walk." **WRONG.** It is the eval-trace SESSION-KEY's
git-dirty marker: `InstallableAttrPath::getOrCreateTraceCache` calls `observeGitIdentity ->
computeGitIdentityHash`, which runs libgit2 `git status` (diff index↔workdir) over the **42K-file
nixpkgs working tree** — stat + getdents every file/dir + .gitignore/attr lookups (the 20% ENOENT
are ignore/attr probes) — ONCE per eval. The verify dep-walk does only ~9K stats. So H3-stat (cache
verify canonicalization) is REFUTED; the verify walk's real cost is hashing/CPU, not stats.

**This is the single biggest hot-STAT lever: G1 — the git-identity worktree scan (~0.35s ≈ ~38% of
the 0.94s wall).** It is:
- **eval-trace-specific** (the session key's dirty-marker; `--no-eval-trace` skips
  `getOrCreateTraceCache` → skips it) — part of eval-trace's "always-on" tax.
- **`-f`-git-worktree-specific / "works for all inputs":** a FLAKE input (locked rev / store-copied,
  immutable) has no worktree to dirty-check → pays ~0 here. So the COMMON interactive `nix eval -f
  /git-repo` dev path pays a ~0.35s/eval scan the flake/CI path does not. (Connects to the user's
  "should work for all inputs" — this is a real per-input asymmetry.)
- **uncached cross-process:** `computeGitIdentityHash` (dep-hash-fns.cc:57) calls `getWorkdirInfo()`
  DIRECTLY, bypassing the per-process `workdirInfoCache_` (git-utils.cc:1515); and it's called once
  per eval, so the within-process `session.gitIdentityCache` (verification-session.hh:106) doesn't
  help it. Every eval re-scans the whole worktree.

**G1 lever options (uncommitted; scope before building):**
1. **Cross-process cache** the identity keyed on a cheap freshness token (`.git/index`
   mtime+size + HEAD oid) → reuse if unchanged. SAME freshness hazard as H1/H2: an UNSTAGED edit
   doesn't touch `.git/index`, so an index-mtime token is UNSOUND (a dirty edit wouldn't
   invalidate). Needs a token that catches unstaged edits → likely back to a scan or git fsmonitor.
2. **git fsmonitor** (libgit2 `core.fsmonitor` / a daemon) → `git status` becomes O(changes) not
   O(42K worktree). The proper fix for repeated evals on a stable tree; nontrivial integration.
3. **Trim the scan** — libgit2 `git_status_options` flags to skip untracked-file detection +
   ignore processing (~20% of the stats are .gitignore/attr). Partial, cheap, sound.
4. **Opt-out fast path** — `--eval-trace-assume-clean` (key on HEAD oid only) for clean CI; unsound
   for dirty worktrees, so gated/explicit.

**Needed analysis before committing G1:** confirm the scan is O(worktree) wall (time
`getWorkdirInfo` directly) and that a flake input skips it (measure `nixpkgs?rev=X#…` stat count);
then prototype option 3 (trim flags, cheapest+sound) and measure the wall delta vs option 2
(fsmonitor, bigger). **Ranking update: G1 now leads the hot levers for the `-f /git` workload**
(~30%), ahead of H3-arch (the verify/cold root fix) and parallel-verify (~2x warm). They are
orthogonal — G1 is session-key setup, H3/H4 are verify.

#### CONFIRMATION (2026-06-01) — G1 is eval-trace's, `-f`-git-only; flakes skip it (and their cost is nix-core)
Trivial-eval A/B (`lib.version`, isolates session setup from eval), `newfstatat` via `strace -c`:
| eval | WITH eval-trace | `--no-eval-trace` |
|---|---:|---:|
| `-f /abs/nixpkgs lib.version` (git worktree) | **159,545** | **1,848** |
| flake `git+file://…?rev=X#lib.version` (store-copied) | 1,585,273 | 1,585,236 |

- **G1 is 100% eval-trace's:** `--no-eval-trace` collapses the `-f` scan 159,545 → 1,848. It is the
  session-key git-dirty marker, fully removable (it's not nix-core eval cost).
- **Flakes skip G1 — definitively:** the locked flake source in the store
  (`/nix/store/…-source`) has **no `.git`** (verified `ls`), so libgit2 `git_status` is impossible.
- **The flake's own 1.58M stats are nix-CORE, not eval-trace** (1,585,273 vs 1,585,236 —
  identical): flake narHash / store-accessor machinery scanning the 86,753-entry store tree per
  eval (`getdents64` 716K, `openat` 359K). eval-trace adds ~0 stats on flakes. (A separate, larger
  nix-flake perf issue — OUT OF SCOPE for eval-trace, but worth flagging: the flake path is NOT
  cheaper than `-f`; it's heavier, just not because of eval-trace.)
- `git status --porcelain` on the worktree = **0.27s** (50,931 tracked files) — the G1 magnitude.

**Net "works for all inputs":** eval-trace's per-eval STAT tax is entirely the `-f`-git-worktree G1
(~0.27–0.36s ≈ ~30% of the closures.gnome hot wall); on flakes eval-trace is stat-free. So **G1 is
the lever for the common interactive `nix eval -f /git-repo` dev path.** Options unchanged (trim
libgit2 status flags = cheapest sound; fsmonitor = biggest; cross-process cache = freshness-hazard;
`--assume-clean` opt-out).

#### G1 PROTOTYPE (2026-06-01) — skipping the scan is a 2.8× warm win, correct + sound. `result-g1/bin/nix`.
Two env-gated prototypes in `GitRepoImpl::getWorkdirInfo` (git-utils.cc, default-off):
- `NIX_EVAL_TRACE_GIT_HEAD_ONLY=1` — skip the `git_status` scan, return `headRev` + empty
  dirty/deleted (== a CLEAN full-scan's identity, so caches interchangeable on a clean tree).
- `NIX_EVAL_TRACE_GIT_NO_UNTRACKED=1` — drop `GIT_STATUS_OPT_INCLUDE_UNTRACKED`.

**Measured (release `result-g1/bin/nix`):**
| metric | baseline | NO_UNTRACKED | HEAD_ONLY |
|---|---:|---:|---:|
| `-f lib.version` newfstatat | 159,545 | **159,545 (no-op)** | **1,105** |
| closures.gnome COLD wall | 11.80s | — | 10.74s (−9%) |
| closures.gnome WARM wall | **0.94s** | — | **0.34s (−64%, 2.8×)** |

- **HEAD_ONLY removes the scan entirely** (159,545→1,105 stats) and the git scan was **~0.60s =
  64% of the 0.94s warm hot wall** — bigger than the ~30% estimate. closures.gnome WARM
  0.94→**0.34s**, byte-identical to `--no-eval-trace`. Cold −9% (scan paid there too; ~1.06s cold).
- **NO_UNTRACKED is a NO-OP** — my hypothesis was wrong: the `.gitignore`/attr work in the profile
  is part of the filesystem *iteration* (statting/ignore-classifying every dir during the
  index↔workdir diff), not gated on untracked *reporting*. Dropping `INCLUDE_UNTRACKED` doesn't
  skip it. (Still safe to drop — `untrackedFiles` is dead — but it buys nothing.)
- **SOUNDNESS (edit-detection test, `/tmp/g1-edittest`):** cold-eval `val`=v1; unstaged edit
  `data.txt`→v2 (HEAD unchanged → HEAD_ONLY keeps the session key stable, so the git-identity does
  NOT see the edit); warm-eval returned **v2** — the FileBytes backstop caught it. Both DEFAULT
  (via session-key change) and HEAD_ONLY (via the backstop) are SOUND for this case.
- **Residual soundness caveat:** HEAD_ONLY trades the git-identity coarse backstop for the
  FileBytes/DirectoryEntries backstops. Normal `.nix` eval is covered (every parsed file → FileBytes;
  dir listings → DirectoryEntries). The narrow OR-1 (toFile literal) / OR-4 (untracked-file-import
  keyset add) theoretical edges — "not demonstrably reachable" per the OR docs — lose their extra
  net. Making HEAD_ONLY the DEFAULT needs the full test suite + the user's risk call.

**Production options (ranked):**
1. **HEAD_ONLY as default** — biggest win (2.8× warm), simplest; relies on FileBytes/DirEntries
   backstops; the edit-heavy nixpkgs-dev user loses dirty-partition precision (cache churn on edit,
   self-healing). Needs suite + risk sign-off.
2. **Cross-process cache** the identity keyed on (`headRev` + `.git/index` mtime/size) — skip the
   scan when nothing staged changed; keeps dirty-precision for staged changes; unstaged edits still
   rely on the FileBytes backstop (same floor as #1) but only between index changes. More
   conservative, keeps the backstop for the common git-operation cases. Bigger change (persist).
3. fsmonitor — O(changes) scan; biggest infra lift; orthogonal (helps even if we keep the scan).
NO_UNTRACKED is dropped (no-op). Recommend prototyping #2 next (keeps soundness margin) OR landing
#1 default-off→opt-in first and measuring real-world churn.

#### G1 #2 IMPLEMENTED + VALIDATED (2026-06-01) — cross-process clean-marker cache. The SOUND production design.
`NIX_EVAL_TRACE_GIT_CACHE=1` (dep-hash-fns.cc `computeGitIdentityHash`, default-off): a cross-process
cache of the "worktree clean" verdict, marker = a 0-byte file under `getCacheDir()/
eval-trace-git-clean-v1/<sha(repo+token)>`, token = `HEAD oid + .git/index mtime+size`. On a clean
hit, build the IDENTICAL clean identity (synthetic clean `WorkdirInfo` → same `buildGitIdentityFromWorkdirInfo`)
and skip the scan. `getWorkdirInfo` was refactored: the identity builder is extracted so cache and
scan produce byte-identical clean identities.

**Measured (`result/bin/nix` = nix-cli with #2):**
| | baseline | HEAD_ONLY | **#2 cache** |
|---|---:|---:|---:|
| `-f lib.version` newfstatat, eval1 / eval2 (new process) | 159,545 / 159,545 | 1,105 / — | **164,912 / 1,120** |
| closures.gnome cold | 11.80s | 10.74s | 11.52s (scan+mark) |
| closures.gnome WARM | 0.94s | 0.34s | **0.35s** |

- **Cross-process cache works:** eval-1 scans + writes the marker (164,912 stats); eval-2, a SEPARATE
  process, reads the marker and skips the scan (1,120). closures.gnome warm 0.94 → **0.35s** (same win
  as HEAD_ONLY), byte-identical to `--no-eval-trace`. One marker for the clean nixpkgs token.
- **SOUND + more conservative than HEAD_ONLY:** the token is HEAD+index-sensitive (2 markers across
  v1/v3 commits prove it). A `git add`/commit bumps the token → marker miss → re-scan → precise
  dirty-partition. Only an UNSTAGED edit between index changes reuses a stale "clean" marker, and the
  FileBytes backstop catches it (edit-test: unstaged v1→v2 returned **v2**). Dirty worktrees never cache.
  So #2 leans on the backstop strictly LESS than HEAD_ONLY (which never scans), at the same warm speed.
- **Cost vs HEAD_ONLY:** #2's first eval per (HEAD,index) token scans (cold ≈ baseline 11.52s);
  HEAD_ONLY never scans (cold 10.74s). #2 trades ~0.8s cold for keeping the soundness margin.

**(c) — HEAD_ONLY does NOT regress the unit suite.** `nix-expr-tests` with
`NIX_EVAL_TRACE_GIT_HEAD_ONLY=1`: **1859 passed / 4 skipped / 0 failed** (identical to baseline). All 24
git-identity tests pass under HEAD_ONLY, incl. `Recovery_GitIdentity_StaleHash_FileMatches_
ValidViaContentDep` — the test that literally encodes the backstop HEAD_ONLY relies on. (Caveat: these
are synthetic `TraceStoreTest` fixtures; they validate the GitRevisionIdentity-DEP recovery, not
necessarily the session-key `getWorkdirInfo` scan path. The integration evidence — asciidoc
byte-identical + the `-f /git` edit test — is the real soundness check, and both pass.)

**RECOMMENDATION:** ship **#2** — it delivers the full 2.7× warm win, is cross-process, preserves the
git-identity dirty-partition for git operations, and narrows the backstop reliance to unstaged edits
only (vs HEAD_ONLY dropping the git-identity entirely). It is the sound production design and a viable
DEFAULT (env-gated now; flipping to default-on needs a broader edit-case soundness pass + sign-off).
HEAD_ONLY stays as the aggressive max-speed (cold too) opt-in. NO_UNTRACKED dropped (no-op). fsmonitor
is OUT (not acceptable for this deployment).

#### G1 #2 SOUNDNESS PASS + made DEFAULT (2026-06-02)
**Broader edit-case pass** — every dep kind, UNSTAGED change (so the #2 marker HITS and the per-file
backstop is what must catch it, not the coarse session-key change), baseline as control. CAUGHT =
warm reflects the change; a "regression" = #2 STALE where baseline CAUGHT (the only thing that would
block defaulting):
| case | dep kind | baseline | #2 | 
|---|---|---|---|
| readFile content edit | FileBytes | CAUGHT | CAUGHT |
| import content edit | FileBytes | CAUGHT | CAUGHT |
| readDir add untracked file | DirectoryEntries (OR-4 keyset-add) | CAUGHT | CAUGHT |
| store-copy `${./asset}` content | DerivedStorePath (narhash) | CAUGHT | CAUGHT |
| delete imported file | (import error) | CAUGHT | CAUGHT |
| toFile literal in imported file | FileBytes (OR-1) | CAUGHT | CAUGHT |
| pathExists create | ExistenceCheck | CAUGHT | CAUGHT |
| dynamic-path readFile | FileBytes | CAUGHT | CAUGHT |
| symlink target content | FileBytes | CAUGHT | CAUGHT |
| deep-nested file | FileBytes | CAUGHT | CAUGHT |

**0 regressions across 11 cases — #2 is byte-identical to baseline on every reachable shape.** (The
OR-1 toFile and OR-4 keyset-add "theoretical edges" are CAUGHT here, via the imported file's FileBytes
and the readDir's DirectoryEntries respectively.) Made #2 the **default path** (dep-hash-fns.cc):
`computeGitIdentityHash` caches by default; `NIX_EVAL_TRACE_GIT_NO_CACHE=1` forces the always-scan
path (escape hatch). HEAD_ONLY/NO_UNTRACKED prototypes reverted from git-utils.cc.

#### ADVERSARIAL PASS over the G1 work + the whole arc (2026-06-02)
Self-interrogation, harshest-first:

1. **The soundness equivalence is EMPIRICAL, not a proof — and #2 *does* shrink defense-in-depth.**
   The honest mechanism: for an UNSTAGED edit, the always-scan path invalidates the WHOLE session
   (worktree dirty → git-identity changes → new session key → the cold trace is in another partition,
   never consulted → fresh eval). #2 keeps the session key and relies on per-dep verify. These are
   equivalent ONLY IF per-dep capture is complete. The OR-4 residual (a key change where the
   `ExprParseFile` FileBytes-backstop does NOT fire) is exactly the case where the always-scan's
   COARSE net catches it and #2's FINE net might not — for unstaged edits. Mitigations that make it
   acceptable: (a) it is "not demonstrably reachable" (OR-4 doc); (b) the closest reachable shape
   (readDir keyset-add) is CAUGHT; (c) it only applies to unstaged edits (staged/committed bump the
   token → re-scan → identical to baseline). But it is a real reduction in coarse safety margin, not
   zero. A defensible default given (a)–(c); NOT a proof of equivalence.
2. **Does the coarse net even add value, or is it illusory?** Baseline's git-identity only changes for
   WORKTREE-FILE changes — which are ALSO caught by FileBytes. So the coarse net adds soundness ONLY
   where a worktree-file change escapes FileBytes (OR-4 condition 1). So #2 loses nothing for every
   case where FileBytes fires (≈ all normal eval), and loses the net only for the unreachable OR-4
   shape. This narrows the risk but confirms it is non-zero.
3. **The win is REPEATED-eval / amortized, not universal.** #2 skips the scan only on a token HIT
   (same HEAD+index). One-shot eval of a FRESH commit (CI walking commits) → new token → scan every
   time → no win (no worse than baseline). It wins the dev loop (re-eval same commit) and CI that
   evaluates many attrs of one commit (first attr scans+marks, rest hit). Framed honestly: a fixed
   ~0.6s removed from REPEATED `-f /git` evals; 2.8× on closures.gnome, more on smaller evals.
4. **The marker cache has rough edges for a production default:** (a) UNBOUNDED growth — one 0-byte
   file per (repo, HEAD, index) ever seen; needs a TTL/GC (follow-up; space is trivial but inodes
   aren't). (b) token = `.git/index` mtime+size — a coarse-mtime FS + a same-size index rewrite within
   one mtime tick could collide (rare; git itself trusts index-stat for racy-clean; add ctime if
   paranoid). (c) `.git` as a FILE (linked worktree/submodule) → `repoRoot/.git/index` absent →
   `:noindex` token → HEAD-only (re-scans only on commit; sound, less precise). (d) concurrent writers
   → idempotent 0-byte marker, fine.
5. **I OWN that the git-scan finding INVALIDATES my own earlier priorities.** H1/H3/H4 and the
   parallel-verify "~2× ceiling" were built on a WRONG attribution: I called the 177K stats "the verify
   dep-walk" when they were 89% the git scan. Consequences I must propagate: (a) H3-arch's HOT payoff
   is far smaller than scoped (the dep walk is ~5% of stats, not the bottleneck); the H3 doc's
   stat-based framing is RETRACTED for hot (it stands only for COLD recording / the flattening). (b)
   The parallel-verify spike measured git-inflated stats → its ceiling claim is suspect for hot. (c)
   Spike-1 (sync verify) was always going to be ~0 wall win because the git scan dominated, not the
   async machinery. The earlier "verify is the hot cost" framing was wrong end-to-end; G1 is the hot
   cost, and it is now addressed.
6. **Process critique.** Four times I formed a lever hypothesis from PARTIAL signal (strace-inflated
   futex; counters; code-reading; an untested flag) and was WRONG until I MEASURED the attribution
   (perf on-CPU, A/B, the stat-source profile). The recurring failure mode: scoping a lever before
   profiling where the cost actually is. The git scan was the FIRST thing every `-f /git` eval does and
   sat unmeasured for the whole arc until the stat-source profile forced it out. Lesson, now applied:
   attribute cost by direct profiling before designing the fix.
7. **Revised next steps (the old ones are demoted):** (a) COLD recording (~6.7s) is the real remaining
   eval-trace cost — UNTOUCHED by G1 (the git scan is ~1s of cold) and the ORIGINAL concern; it is the
   next lever, via record-time dep dedup (C3) or the compositional DAG (H3-arch, whose payoff is now
   COLD not hot). (b) The nix-CORE flake narHash/store-tree scan (1.58M stats/eval, LARGER than the
   git scan, affects ALL flake evals, identical with/without eval-trace) is a real nix-flake perf bug —
   out of eval-trace scope but worth flagging upstream. (c) Marker GC/TTL for the #2 default. H3-arch
   and parallel-verify are NOT the priority; cold recording is.

### ADVERSARIAL PASS ON (a) "COLD RECORDING" (2026-06-02) — (a) IS MIS-SCOPED. Recording is 0.2s; cold is GC-bound (inherent).
Per the standing discipline (measure the attribution before scoping the lever), I re-measured cold
BEFORE touching it. Three findings, each overturning the prior framing:

1. **"~6.7s recording" is WRONG. `record.timeUs` (the store write) = 0.20s** (hashUs 0.11, flush 0.06,
   serialize 0.01, count=7), on a 11.98s cold closures.gnome. The 6.7s figure was stale (old docs /
   different baseline). Recording the trace to SQLite is cheap; it is NOT a lever.
2. **Cold decomposition (current binary):** pure eval (merge-base, no eval-trace) 4.5s; HEAD
   `--no-eval-trace` 7.5s ⇒ **always-on tax ≈ 3s** (EvalEnvironment routing + semantic objects, paid
   even DISABLED); HEAD cold 12.0s ⇒ +4.5s = git scan ~1s (now #2-cached) + recording-path eval ~3.3s
   (dep-capture building the 131,782 flattened deps + TracedExpr wrapping) + store-write 0.2s.
3. **~54% of cold CPU is `GC_add_to_black_list_normal` (Boehm conservative-GC black-listing) — and it
   is INHERENT, not eval-trace's.** perf self-time `GC_add_to_black_list_normal`: merge-base **54.1%**,
   HEAD `--no-eval-trace` **59.9%**, HEAD cold **53.2%** (all-GC ~110-120% via parallel GC-marker
   threads). SAME in plain pre-eval-trace nix. It is the Boehm conservative GC scanning the hash-/
   string-heavy nixpkgs heap and black-listing integers that look like pointers — a NIX-CORE pathology
   on closures.gnome, the single biggest cost of ANY closures.gnome eval. (My "eval-trace hash data
   causes the black-listing" hypothesis was FALSIFIED by the merge-base profile.)

**Consequence — (a) re-scoped:**
- The biggest cold cost (~54% GC black-listing) is **nix-core, out of eval-trace scope** — flag
  upstream alongside flake-narHash. (Fixing it — precise GC, black-list tuning, or GC_malloc_atomic for
  blob/string data — would ~2x ALL large nix evals, but is an RFC-scale nix-core change.)
- The eval-trace-SPECIFIC cold overhead is the **always-on tax (~3s, paid even `--no-eval-trace`) +
  the recording-path dep-capture (~3.3s)** — both ALLOCATION-bound, and ~½ of each is the proportional
  inherent GC. The tractable eval-trace lever is **reduce allocation**: (i) the always-on tax's
  per-observation/semantic-object allocation on the DISABLED path (a regression for ALL users: ~0.9s
  non-GC + ~2.1s the GC it induces); (ii) the 607× dep flattening (compositional DAG / record-less,
  RFC-scale). Recording (0.2s) is a non-lever.
- **Do NOT pursue "(a) cold recording" as named.** Honest re-scope: the cheapest eval-trace cold win is
  trimming the ALWAYS-ON TAX (disabled-path allocation, also helps `--no-eval-trace` users); the deep
  win is the dep-flattening (now a COLD lever, ex-H3-arch). The dominant cost is nix-core GC, to flag.
  Needs the user's call on which — or whether the cold cost (half unfixable-here GC) is worth it at all.

### VERIFICATION OF SUB-AGENT FINDINGS (2026-06-02) — two sub-agents ran (1)=trim-always-on-tax, (3)=flag-nix-core; parent re-verified everything. Corrected BOTH agents AND the parent's own "54% GC" finding.
- **CONFIRMED: the "54% GC" was a hybrid-CPU artifact.** This box is a 13th-gen i9-13900K (P+E cores;
  `/sys/devices/cpu_core` + `cpu_atom`). `perf report --sort=symbol` emits TWO ~100%-summing PMU blocks;
  the parent's awk summed both (→ the bogus "107.8%") and the "54%" was the cpu_atom/E-core block where
  the GC mark thread is pinned. **The GC is PARALLEL, ~1s, and NOT on the wall critical path** —
  re-measured: baseline cpuTime 6.17s/wall 4.87s (gap 1.3s), HEAD `--no-eval-trace` 8.82s/7.94s (gap
  0.88s); the +3.07s WALL regression ≈ the +2.65s cpuTime regression with the parallel gap SHRINKING,
  so the cold-tax wall is **SERIAL eval-trace work, not the inherent GC.** ⇒ the parent's earlier "half
  the cold is unfixable GC, leave it" was WALL-MISLEADING; the ~3s tax is mostly RECOVERABLE serial work.
- **Agent (1) — trustworthy; refuted the parent's own hypothesis.** The "per-observation EvalEnvironment
  routing/semantic-object allocation" hypothesis is REFUTED: an env-gated routing fast-path
  (`NIX_EVAL_TRACE_FASTPATH`) gave ZERO win (counters identical). The real tax: (a) **`Bindings` grew +16B
  — CONFIRMED** (`valueIdentityStamp_` attr-set.hh:126 + `publication_` :135) × 4.07M attrsets = +65MB,
  one extra GC cycle; a side-table off the hot 24B struct recovers ~1.16s cpuTime but only **~0.3s wall**
  (the GC is parallel). (b) **diffuse SERIAL instrumentation ~2s** — import classifying sources via
  throwing `parseStorePath` (primops.cc:386-388; 4× exception-unwinding), extra MountedSource/Caching
  accessor layers, `forceListObserved`/`coerceToStringWithProvenance`. NO landed fix (the prototype is a
  no-op by design); concrete file:line targets only. The agent's worktree patch is a NO-OP — do NOT apply.
- **Agent (3) — GC + upstream issues VERIFIED; the flake "cold-only" claim REFUTED.** GC mechanism
  (eval-gc.cc:53/57 conservative-GC config) and the cited issues are real + relevant: **#3121** "Copy
  local flakes to the store lazily" (open), **#13225** "Lazy trees v2" (open), **#14088** "Managed Heap"
  (open) — all WebFetch-confirmed. BUT agent 3's claim that the flake 1.58M-stat scan is COLD-CACHE-ONLY
  (warm=3.6K via `sourcePathToHash`) is **REFUTED**: re-measured **1.58M newfstatat on EVERY eval** here —
  cold isolated 1,589,837 / warm-2nd-eval isolated 1,585,273 / warm SHARED `~/.cache` (fetcher-cache-v4
  present) 1,585,277. Agent 3's 3.6K is NOT reproducible in this environment; the flake-scan cacheability
  is now UNCERTAIN (is the fetch/eval cache not hitting on this fork? agent 3 may have hit the flake
  EVAL-cache, not the source cache). The flake upstream draft must NOT assert "cold-only" until this is
  resolved. The GC upstream draft is sound.
- **Net corrected next-steps:** (1) the always-on tax (~3s) is mostly RECOVERABLE serial work, not GC —
  quick win = move the 2 Bindings fields to a `Bindings*`-keyed side-table (~0.3s wall, +1.16s cpuTime);
  bigger = the diffuse serial instrumentation (import-throwing first — non-throwing `maybeParseStorePath`).
  (3) GC draft ready to flag (#14088 etc.); flake draft BLOCKED on resolving the 1.58M-every-eval-vs-3.6K
  cacheability discrepancy. Do NOT trust the sub-agents' un-reverified specifics — esp. agent 3's flake numbers.

### (a)+(b) ATTEMPTED + ADVERSARIAL PASS (2026-06-02) — both (a) fixes collapse on inspection; cold has NO clean lever. STOP.
**(a) — both concrete fixes REFUTED by code inspection (before building):**
- (a2) "import throws via parseStorePath, fix = maybeParseStorePath": REFUTED. `isStorePath` ALREADY
  calls `maybeParseStorePath` (store-dir-config.cc:39). The throw is INSIDE `maybeParseStorePath`
  (`try { parseStorePath } catch`, store-dir-config.cc) — nix-CORE, present in the merge-base, and the
  import primop only `parseStorePath`s AFTER `isStorePath` is true (primops.cc:385-388), so no
  throw-as-control-flow there. The unwinding is real for `-f` (non-store import paths hit
  maybeParseStorePath's internal throw) but it is nix-core; a real fix = a non-throwing StorePath
  parser (~0.14s, broad, libstore-scope), NOT eval-trace's tax. Where eval-trace ADDS the 4× is unidentified.
- (a1) "move the +16B Bindings fields to a side-table (~0.3s)": NOT a clean win. The footprint win is
  ~0.3s but PARALLEL-GC (largely wall-hidden, per the cpuTime/wall verification). And `Bindings::
  publication()` is READ on the DISABLED path: `Value::publication()` (eval-inline.hh:155) ← `EvalState::
  lookupSemanticHandle` (eval.cc:3293) ← coercion/primops (eval.cc:3025, primops.cc:2420), the single
  provenance entry point that runs on string coercion even with `--no-eval-trace`. A side-table turns
  each into a (missing-key) concurrent-map lookup → adds disabled-path cost that likely NEGATES the
  parallel footprint win. Net ≈ wash. (Inferred from call-sites, not measured — see self-critique #6.)
**(b):** flake mechanism REFUTED (`sourcePathToHash` cache HITS per `--debug`, yet 1.58M happens —
NOT the NAR walk; source unidentified). Upstream drafts in `plans/upstream-nix-core-findings.md`: GC
READY (magnitude caveated — the "54%" was a hybrid-CPU/E-core PMU artifact; GC is parallel, ~20% wall);
flake NOT READY (needs the stat-source profile to pin the 1.58M).

**ADVERSARIAL PASS over the cold arc:**
1. **Meta-pattern: SIX consecutive refutations.** "6.7s recording"→0.2s; "54% GC, leave it"→hybrid
   artifact + parallel GC; routing-hypothesis→no-op fast-path; Bindings side-table→disabled-path read,
   wash; import-throwing→already maybeParseStorePath, nix-core; flake NAR-walk→cache hits. **The cold
   path has NO clean, high-value eval-trace lever.** The +3s always-on tax is genuinely DIFFUSE serial
   instrumentation (no single function dominates the non-GC serial time) + ~1s parallel inherent GC.
2. **Was it worth it?** No landed cold win — but real VALUE: the cold path is now RULED OUT with
   evidence (vs the prior "6.7s recording" framing that would have optimized a 0.2s non-cost), and the
   GC/flake framing is corrected. Knowing a path is a dead-end is a result. The eval-trace WINS stand:
   #2 git-identity cache (2.7× warm, f615911dc + GC 3a4c5a46d) and Spike-1 sync-verify (7f44193fb).
3. **Process failure I own:** I let the sub-agents PROTOTYPE on hypotheses (agent-1 fast-path, agent-3
   NAR-walk) before profile-confirming the mechanism — the same "scope before profiling" error, just
   delegated. Verification caught it, but at the cost of build cycles. The discipline holds:
   profile-confirm the mechanism BEFORE prototyping.
4. **Honest residual:** the only quantified eval-trace-specific cold allocation is the +16B Bindings
   (~0.3s parallel GC, side-table = wash); the deep lever is the 607× dep flattening (RFC-scale
   compositional DAG, payoff bounded by the GC floor). Neither is high-value.
5. **RECOMMENDATION: STOP chasing cold.** Declare it investigated + dead-end. Hot is solved (#2).
   Nix-core findings flagged (GC ready-with-caveat; flake needs one more profile). Spend effort
   elsewhere or wrap up.
6. **Self-critique of THIS pass:** "no clean lever" is well-supported but NOT exhaustively proven — I
   INFERRED the Bindings publication-read is hot-on-disabled from call-sites (didn't measure read
   frequency under `--no-eval-trace`), and agent-1's diffuse +0.67s "other-eval" residual was never
   bisected. A determined bisection + a measured Bindings-read-frequency could still surface a small
   lever. But expected value is low (a ~3s, half-parallel-GC cold path on a research branch), so
   stopping beats the opportunity cost. If cold ever matters: bisect the +0.67s with profile-confirmed
   attribution first; do not prototype on a hypothesis.

## (b) FLAKE FIX — PROTOTYPED + MEASURED (2026-06-02): 9.95× fewer stats, **~9× WALL**. + ADVERSARIAL PASS re-opening cold & hot.

### (b) result — the redundant-`getRepoInfo` fix WORKS, and the win is WALL, not just stats
`git.cc` `getRepoInfo` workdir token-cache (env `NIX_GIT_WORKDIR_CACHE=1`, default-off, `result-flakefix`),
keyed on `HEAD oid + .git/index mtime+size` (= eval-trace #2's `gitCleanToken` pattern). Warm
`git+file://~/nixpkgs?rev=HEAD#lib.version`:

| | newfstatat | ENOENT probes | WALL (warm) |
|---|---:|---:|---:|
| gate OFF (baseline) | 1,585,265 | 358,204 | 5.8–6.8s (~6.5s) |
| gate ON (cached) | 159,327 | 35,977 | **0.71s** |
| ratio | **9.95×** | 9.96× | **~9×** |

Output byte-identical. The wall win (6.5→0.71s) **exceeds** the stat ratio because each `git status` scan is
~0.6s of real work (stat + 716K getdents + 359K openat + libgit2 diff), not just stats — so ~9 redundant
scans ≈ 5.8s of a 6.5s warm flake-eval wall. **A trivial `lib.version` flake eval of nixpkgs spends ~90% of
its wall re-scanning the worktree ~10×.** nix-CORE (libfetchers); `--no-eval-trace` identical.

**Soundness.** Within ONE eval (one-shot `nix eval`) the ~10 calls share a token (worktree provably stable
mid-eval) → collapse to 1 → trivially sound; this is the dominant case and the entire measured win. The
process-local static cache resets per process, so a one-shot CLI eval never reuses a token across a worktree
change. CROSS-eval reuse (repl / `nix develop` / daemon) is governed by the token: HEAD-change + staged/removed
(.git/index mtime) correctly invalidate — exactly the git.cc:790-793 concern — leaving only
unstaged-content-edit-without-index-change as the residual (identical trade-off to eval-trace #2). The
fully-sound production form is a WITHIN-EVAL memoization (clear at the eval boundary; the redundancy only
exists within one eval, so no token + no cross-eval residual at all). Prototype committed default-off as the
measurement artifact; productionization (within-eval memo) is a nix-core change.

### ADVERSARIAL PASS (2026-06-02) — the user is right: cold is NOT a dead-end, hot is NOT solved. I shipped both conclusions on inference, not profiling.

The discipline I keep re-owning ("profile-confirm the mechanism before scoping a lever") applies
**symmetrically to NEGATIVE conclusions**. "Dead-end" and "solved" are attribution claims and deserve the same
profile-confirmation as a lever. I shipped both on inference + low-EV reasoning. Both re-open.

**COLD is under-investigated, not dead.** The "SIX refutations" refuted six specific FIXES, not the existence
of a lever. The always-on tax is real and LARGE: merge-base 4.5s → `--no-eval-trace` 7.5s = **+3s / +67% cold
regression, paid by every user even cache-OFF.** A 67% regression is not a dead-end; it's an unattributed cost
I stopped bisecting. Three unprofiled pieces:
1. **The +0.67s "other-eval" residual — NEVER BISECTED** (my own self-critique #6, line ~1122). The largest
   unexplained serial component of the tax. Declaring dead-end with the biggest piece unprofiled IS the
   meta-pattern — 7th instance.
2. **The +16B Bindings side-table — INFERRED a wash, never measured.** "`publication()` read on the disabled
   path negates the win" ignores that reading a null ptr is ~free; the real cost is CACHE FOOTPRINT on the
   multi-million-`Bindings` nixpkgs working set (unmeasured), and "parallel-GC hides it" is the SAME
   hybrid-CPU reasoning that already burned me once.
3. **Source-accessor layers (+0.29s, a NAMED subsystem) — dismissed as "diffuse."** MountedSource/CachingSource
   wrapping + FD-cache rb-tree + path-splitting is a specific stack eval-trace interposes, not diffuse.

Verdict: cold EV is lower than hot (a half-parallel-GC path) but "low EV" ≠ "dead-end" — I conflated them.
Re-open with the **+0.67s bisection FIRST** (profile-confirmed attribution), then measure the Bindings footprint.

**HOT is not solved — #2 fixed the git-scan; the verify-rehash is wide open.** Post-#2 warm = 0.35s; the verify
dep-walk is ~85% (≈0.30s; #2 removed only the 0.60s git scan, not the 0.30s verify). The verify cost is NOT
stats (the stat-source profile proved those were GIT, now cached) — it is **blake3 re-hashing**: per
`src/libexpr/eval-trace/CLAUDE.md`, warm-verify FileBytes "re-reads + re-hashes UNCONDITIONALLY." The cache
that would skip it (H1) fires ONLY for Registered store-path sources (`isInStore` gate) → **inert on
`-f /git-repo` posix paths** = the user's "should work for all inputs," undone:
- **HOT-1 (the headline): extend the content-hash cache to posix `-f` paths**, freshness token =
  mtime+size+inode (standard Nix; this fork's #2 git cache already uses the identical HEAD+index-mtime token
  pattern). Removes the blake3 re-hash on the dev/`-f` warm path. Literally "make the cache work for all inputs."
- **HOT-2: serial blake3 on verify.** taskset proved 1-CPU == 32-CPU wall → the TBB-parallel blake3 coord
  (~13% wall) is pure overhead at per-file sizes. Spike-1 landed sync-verify (removed io_context) but
  blake3-in-verify may still pay TBB coord. Cheap, untested in isolation.

**Meta-lesson (owned, again).** A dead-end is a claim about attribution; profile-confirm it like a lever.
Corrective: ground HOT-1 (re-measure the post-#2 verify-rehash share) BEFORE prototyping; bisect cold's +0.67s
BEFORE concluding. NEXT: ground HOT-1 (the user's stated goal + the 85%-of-hot lever) and re-open the cold
+0.67s bisection.

### COLD BISECTION DONE (2026-06-02) — the +0.67s residual is NOT diffuse: ~40% source-accessor layers + ~23% Bindings footprint. COLD HAS A REAL LEVER.

Reproduced the always-on tax on a NEW workload (`firefox.drvPath`, not closures.gnome) with both binaries
(`result-mergebase` 616df97 = pre-eval-trace, `result` = HEAD #2): median merge-base **1.015s** → HEAD
`--no-eval-trace` **1.726s** = **+0.711s / +70% tax, paid cache-OFF**. Proportional + real, not a
closures.gnome artifact (the merge-base evaluates current nixpkgs fine; firefox-147 drv byte-matches HEAD).

**perf stat A/B** (cpu_core = P-core eval thread, 98.76% of work; cpu_atom/GC = 1.3%, barely grew):

| cpu_core | merge-base | HEAD-disabled | Δ |
|---|---:|---:|---:|
| instructions | 8.74B | 12.19B | **+3.45B (1.39×)** |
| cache-references | 47.1M | 113.9M | **+66.8M (2.42×)** |
| L1-dcache-misses | 57.3M | 94.0M | 1.64× |
| LLC-load-misses | 0.69M | 0.81M | 1.17× |

⇒ the tax is on the EVAL THREAD, NOT GC — **refutes the "diffuse half-parallel-GC dead-end."** It is +3.45B
real instructions (~0.49s) + a footprint component (2.4× cache-refs, 1.64× L1 misses; LLC barely moves → the
extra footprint is L1→L2, still cache-resident).

**perf-report bucket attribution** (% cpu_core samples; HEAD pie is 1.74× bigger, so the abs Δ ≈ %·wall):

| bucket | mb % | HEAD % | ~abs Δ |
|---|---:|---:|---:|
| **source-accessor layers** (MountedSource / PosixDirectory FD-cache / FdSource / CanonPath) | 3.73% | **19.13%** | **~+0.30s (~40%)** |
| **Bindings +16B footprint** (ExprAttrs::eval / BindingsBuilder / __adjust_heap / intersectAttrs) | 12.64% | 17.31% | ~+0.18s (~23%) |
| GC (proportional to the added allocation) | 3.96% | 10.25% | ~+0.14s |
| EvalEnvironment routing (`copyPathToStoreImpl(EvalEnvironmentState&)` — eval-trace-ONLY, **absent in mb**) | 0% | 3.69% | ~+0.07s |
| coercion/publication (eval-trace-ONLY) | 0% | 1.79% | ~+0.03s |

Σ Δ ≈ 0.69s ≈ the +0.711s tax (buckets account for the regression).

**RETRACTIONS (the user was right both times):**
1. **"+0.67s residual is diffuse" — FALSE.** Dominated by the source-accessor layers (5.1× explosion,
   3.73→19.13%) = ~40% of the tax in ONE named subsystem. eval-trace routes every FS observation through
   `EvalEnvironment`, which wraps the accessor in Mounted/Caching layers + an FD-cache rb-tree + CanonPath
   ops — active even `--no-eval-trace` (same files read, 5× the accessor cost = pure wrapping/routing overhead).
2. **"+16B Bindings side-table = a wash" — FALSE.** Measured +4.67pp / ~+0.18s footprint (the 2.4× cache-refs
   + 1.64× L1 misses corroborate). Measured, not inferred.

**COLD LEVERS (real, ranked):**
- **COLD-1: source-accessor layers (~+0.30s).** Make the EvalEnvironment FS routing zero-overhead when no
  session is active — skip the Mounted/Caching wrap + FD-cache rb-tree on the disabled path, or hoist accessor
  resolution out of the per-access hot path. Biggest single recoverable bucket; benefits ALL users cache-OFF.
- **COLD-2: Bindings +16B side-table (~+0.18s).** The footprint lever, now measured-real (not a wash).

Cold is NOT a dead-end — it's a **70% always-on regression** with two named, recoverable subsystems. LESSON
(owned): a dead-end is an attribution claim; I shipped it without the perf-stat A/B that would have shown
+3.45B instr / 2.4× cache-refs on the eval thread. The discipline applies to negative conclusions too.

### HOT-1 IMPLEMENTED + VALIDATED (2026-06-02) — posix content cache: warm-verify FileBytes rehash on `-f` ELIMINATED (contentUs 22765→0), cross-process, sound. [⛔ "sound"/"default-on" RETRACTED 2026-06-04 — see banner ↓]

> **⛔ RETRACTED 2026-06-04 — HOT-1 is now DEFAULT-OFF; the "sound" / "VALIDATED
> default-on" / "SOUND (same-size edit invalidate)" claims in THIS section and in the
> "#4 (default-on) DONE + VALIDATED" section below are WRONG.** HOT-1's freshness token
> (mtime+ctime+size+inode) has a real stat-race: on a COARSE-mtime filesystem (ZFS-on-Linux
> rides the kernel coarse realtime clock, ~10ms tick at HZ=100; tmpfs is fine), two same-size
> writes within a tick produce a BYTE-IDENTICAL token, so the cached hash stale-serves.
> Measured: ZFS 1888/2000 same-size-rewrite collisions, tmpfs 0/2000. The
> `EvalTraceProperty_FilterList/SortList` tests caught exactly this and were wrongly dismissed
> as "expectation bugs." **Item 5 below ("Soundness caveats … (a) coarse-ctime filesystems …
> → stale … Default-on needs a bench soundness pass") was CORRECT and was overridden by the
> #4 default-on flip** — the "#4 VALIDATED SOUND (same-size edit invalidate)" pass must have
> run on a fine-grained FS (or dismissed the failing property tests). Cross-process eval is
> sound (an edit gets a strictly-later mtime than the recording process); only intra-process
> re-eval within a tick collides (`nix repl :reload`, watch). It is a cache CLASS — the #2
> git-clean marker shares the assumption and is coupled (HOT-1 voided the FileBytes content
> backstop the marker relies on). FIX: default-off (commit `1c667c664`). Re-enable path =
> git-style racy-clean guard (window ≥ the tick, ≥1s safe — the granularity IS boundable by
> HZ). Full analysis: `doc/eval-trace/measurements/property-test-overinvalidation-2026-06-04.md`.

Extends H1 to NON-store posix paths (the `-f` / dirty-worktree case H1's `isInStore` gate leaves on the
read path). Sibling gate `hot1PosixKey` in `verifier.cc`, REUSING H1's `FileContentHashes` table / map /
flush (store-path and `"posix:"`-prefixed keys are disjoint). Key = `"posix:" + len(abs) + ":" + abs + ":"
+ mtime + ":" + size + ":" + ctime + ":" + inode` — a freshness token, because posix paths are MUTABLE
(unlike content-addressed store paths). Env-gated `NIX_EVAL_TRACE_POSIX_CONTENT_CACHE=1`, default OFF.

A/B (warm `asciidoc.nativeBuildInputs`):

| | eligible | hits | depHash.contentUs |
|---|---:|---:|---:|
| gate OFF (H1 only) | 0 | 0 | 22765 (re-hashes every FileBytes on every warm eval) |
| gate ON, hot1 (FRESH process) | 220 | **220** | **0** (rehash ELIMINATED) |

Cross-process: cold populates 219 (H1b-style, from the record path), and the FIRST warm verify in a fresh
process hits 220/220. Byte-identical to `--no-eval-trace`. **SOUND**: a SAME-SIZE in-place edit (AAAA→BBBB)
correctly invalidates (afterEdit=BBBB) — the ctime/mtime token catches a content change a size-only token
would miss.

**Two bugs found + fixed during validation** (the "verify, don't trust" discipline earned its keep):
1. Copied H1's `isAbsoluteDepSource` exclusion — but `-f /abs` FileBytes ARE Absolute-source, so eligible
   stayed 0. Removed it; the `isInStore` check still excludes absolute STORE paths (H1's input-addressed
   hazard), so only non-store posix paths are cached.
2. The token used a NUL separator (copied from `gitCleanToken`, where it feeds a HASH). A NUL TRUNCATES a
   SQLite TEXT key on `column_text` load, so the cold-stored full token loaded back truncated and every
   cross-process lookup missed (cold pop=219 but hot hits=1). Fixed: length-prefixed path, NO NUL
   (SQLite-safe + collision-free for any path bytes; a collision would be a stale serve).

This IS the user's "works for all inputs": the content-hash cache now fires on `-f` posix paths, not only
store-resident sources. Covers FileBytes/RawBytes; DirectoryEntries (`directoryUs`) is a follow-up (a dir
freshness token + dir-listing cache). Default-ON flip is gated on a full functional-test + bench soundness
pass (the freshness-token policy is a soundness decision — though the same-size-edit test + ctime semantics
make it robust for normal operation).

### productionize-(b) DONE (2026-06-02) — path-only `getRepoInfo` split: 9.97× stat / ~9× wall, DEFAULT-ON, provably sound (no caching at all).

The token-cache (b24c2f105) proved the win but carried a cross-eval soundness caveat (unstaged edits in a
long-running process). The SOUND production fix is simpler AND cache-free: `getSourcePath` does
`getRepoInfo(input).getPath()`, and `getPath()` (git.cc:651) reads ONLY `repoInfo.location` — NEVER
`workdirInfo` (the `isDirty` check lives in a separate `warnDirty()` that `getSourcePath` never calls). So
`getSourcePath` does not need the O(worktree) git_status scan **at all**. Added a `needWorkdirInfo` param to
`getRepoInfo` (default true); `getSourcePath` passes false → skips the scan. The stat-source profile showed
`getSourcePath` drives ~all the ~10 redundant scans, so this alone gets the full win:

| warm `lib.version` flake eval | newfstatat | wall |
|---|---:|---:|
| baseline (token-cache off) | 1,585,265 | ~6.5s |
| **path-only split, DEFAULT (no env)** | **158,911** | **0.716s** |
| + NIX_GIT_WORKDIR_CACHE=1 | 158,915 | — |

9.97× stat / ~9× wall, byte-identical. The token-cache adds nothing now (the residual ~159K is the ONE
genuine-workdir fetch scan, which the path-only split doesn't touch and the token-cache can't reduce further
in one process). **PROVABLY SOUND** — no token, no staleness, no cross-eval residual: `getPath()` is
independent of `workdirInfo`. The token-cache stays env-gated default-off as an opt-in for genuine-workdir
callers (inert here). This is the cleanest upstream fix — better than memoization (no cache at all). The
`getPath()`/`workdirInfo` independence makes it a near-trivial, obviously-correct upstream PR.

### HOT-2 SUBSUMED (2026-06-02) — no wasted TBB coordination remains on the warm path; serial-blake3 would be a no-op or a regression.

HOT-2 targeted the "13.2% TBB coord wasted at these sizes" from the OLD hot profile. That TBB was the verify
POOL (`BlockingThreadPool` / `coroBlock`), NOT blake3's internals. Two committed changes already removed it:
- **Spike-1 (7f44193fb):** `verifySync → verifyAttrSync` is the DEFAULT and does NOT use `coroBlock`/`blockingPool`
  (those are record-side only — context.cc:353/373/384). The async verify pool is off the warm path.
- **HOT-1 (f4527def4):** the warm-`-f` FileBytes blake3 rehash is eliminated (contentUs→0).

And blake3's OWN TBB is threshold-gated to ≥128KB (hash.cc:320, `blake3TbbThreshold=128000`) — i.e., only where
parallel hashing IS faster; most nixpkgs files are smaller and already hash serially.

**VERIFIED:** a perf profile of 20 warm `-f` evals (HOT-1 on) shows NO tbb / NO blake3 / NO BlockingThreadPool /
NO coroBlock on the warm path — the only hash symbol is `sha256_block_data_order` at 0.14% (nix-core store
hashing, negligible). So there is no wasted coordination left to remove; forcing blake3 fully serial would
REGRESS large-file (≥128KB) hashing on the cold/record path where the threshold currently picks the faster mode.
**HOT-2 is done-by-subsumption — correctly NOT implemented.**

---

## ALL FOUR LEVERS RESOLVED (2026-06-02) — summary

| Lever | Outcome | Commit |
|---|---|---|
| **HOT-1** posix content cache | warm-verify FileBytes rehash on `-f` ELIMINATED (contentUs 22765→0, 220/220 cross-process hits). ⛔ "sound same-size-edit" RETRACTED 2026-06-04 — coarse-mtime stat-race (see banner above); now DEFAULT-OFF (`1c667c664`) | f4527def4 |
| **Productionize (b)** | path-only `getRepoInfo` split: 1.585M→158,911 newfstatat (9.97×), ~6.5s→0.716s wall, DEFAULT-ON, provably sound (cache-free) | 8fa80ce14 |
| **Cold bisect** | +0.711s/+70% always-on tax = source-accessor layers ~40% (COLD-1) + Bindings +16B footprint ~23% (COLD-2), on the eval thread (NOT GC). Real levers identified | bc91b4d1a |
| **HOT-2** serial blake3 | SUBSUMED by Spike-1 (verify pool) + HOT-1 (rehash) + the 128KB blake3-TBB threshold; would regress if forced | (none) |

Adversarial pass (both user pushbacks VINDICATED): cold is a 70% always-on regression with named recoverable
subsystems (NOT a dead-end); hot's verify-rehash was wide open (NOT solved by #2 alone). Remaining implementable
work, ranked: COLD-1 (source-accessor zero-overhead-when-disabled, ~+0.30s, all-users), HOT-1 default-on flip
[⛔ was flipped on (#4) then REVERTED 2026-06-04 — coarse-mtime stat-race; needs a racy-clean guard first, see
the RETRACTED banner], HOT-1 DirectoryEntries extension, COLD-2 (Bindings side-table, ~+0.18s).

## QUANTIFICATION ON THE RELEASE BINARY (2026-06-02, ab175bc / result rebuilt) + ADVERSARIAL PASS + PLAN

### Quantified wins (release `result/bin/nix`)

**productionize-(b)** — flake `lib.version`, DEFAULT (no env): **158,954 newfstatat (9.97×), 0.728s wall median (~9×)**. Solid, headline, default-on. ✓

**HOT-1** — measured on TWO heavy `-f` workloads (closures.gnome is gone from release.nix's evaluated
top-level at HEAD — `hasAttr "closures"` is false; used `firefox.drvPath` and a gnome NixOS-container
system closure, the closures.gnome equivalent at HEAD):

| workload (warm hot) | contentUs off→on | posix hits | hot wall off→on |
|---|---|---|---|
| firefox.drvPath | 55013→92 | 1050/1050 | 0.292→0.272s (**~7%**) |
| gnome system closure | 79243→150 | 3877/3877 | 0.296→0.262s (**~11.5%**) |

The FileBytes rehash is ELIMINATED (contentUs→~0, all eligible deps hit cross-process). Syscall trade
(gnome, strace): `close` 8033→2516 (~5500 file reads skipped); newfstatat UNCHANGED (11631→11630 — the
read path already stat'd, so HOT-1 adds NO stat overhead). Symlink soundness RE-TESTED: a symlink-target
content change is correctly detected (the FileBytes dep resolves to the target, so the token tracks it).

### ADVERSARIAL PASS over the quantification

1. **HOT-1's WALL win is modest (~7–11%), NOT the large number "verify is 85% of hot" implied.** Honest
   decomposition (gnome hot 0.296s): verifyTrace=219ms, of which FileBytes contentUs=79ms ≈ **36% of the
   verify, ~27% of the wall**. HOT-1 removes the FileBytes rehash but the wall drops only ~34ms because (a)
   path-resolution/stat still runs (newfstatat unchanged), (b) page-cached small-file read+hash is cheap,
   (c) the OTHER ~64% of the verify — `directoryUs`, `structuredNixUs`/`structuredOuterUs`, `loadTrace` — is
   untouched. So HOT-1 is a **~10% hot-wall win**, useful but not transformative. I'd implied bigger.
2. **The `contentUs` counter OVERSTATES the win — lead with WALL.** "contentUs 79ms→0" reads like a 79ms
   win; the wall moved 34ms. The counter times code (resolve/stat) that partially still runs. Report the wall.
3. **HOT-1 and productionize-(b) are DIFFERENT workloads — don't conflate.** (b) = flake fetcher (git+file),
   9×. HOT-1 = `-f` hot verify, ~10%. A combined "9× + HOT-1" number would be wrong.
4. **The bigger lever is COLD-1, not HOT-1.** The cold bisection's source-accessor bucket is ~+0.30s and paid
   by ALL users on EVERY eval (cache on or off) — an order of magnitude more impact than HOT-1's ~34ms hot
   win on a cache hit. The quantification reinforces: COLD-1 is where the leverage is.
5. **Soundness caveats that keep HOT-1 env-gated:** (a) coarse-ctime filesystems (network/old FS w/o ns
   ctime) → a same-second same-size edit could collide → stale; (b) extends the record-side "file-change-
   during-eval is UB" contract cross-process (a policy choice). Symlink hole REFUTED by test. Default-on
   needs a bench soundness pass.
6. **Workload caveat:** gnome-container ≠ closures.gnome (isContainer), HEAD ≠ the bench's pinned nixpkgs, so
   these are NOT comparable to the historical 0.35s hot. The ~7–11% RELATIVE win (same binary, off vs on) is
   the honest, cross-binary-consistent number.

### PLAN — remaining work, ranked by value/effort

1. **COLD-1 (HIGHEST VALUE) — source-accessor zero-overhead-when-disabled (~+0.30s, ALL users).** Profile-
   confirm WHY eval-trace's EvalEnvironment FS routing adds ~5× accessor cost even `--no-eval-trace`
   (MountedSource/CachingSource wrap vs FD-cache rb-tree vs CanonPath ops — the perf-report buckets), then
   make the routing a zero-cost pass-through when no session is active. Biggest impact; needs a careful
   investigation + a targeted fix. **Recommended next.**
2. **HOT-1 DirectoryEntries extension (moderate, cheap) — cache `directoryUs` too.** Same freshness-token
   pattern keyed on the dir; ~doubles HOT-1's hot win by covering the second-biggest verify bucket.
3. **Upstream productionize-(b)** — clean, obviously-correct nix-core PR (getPath/workdirInfo independence).
   Outward-facing → user sign-off on the patch + text first.
4. **HOT-1 default-on flip** — ⛔ was done (#4) then REVERTED 2026-06-04 (coarse-mtime stat-race; now
   default-off, `1c667c664`). A re-flip REQUIRES a git-style racy-clean guard first (persist each posix
   entry's write time; re-read files whose mtime is within the FS granularity / coarse-clock tick of it —
   window ≥1s is safe) AND a soundness pass run ON A COARSE-MTIME FS (ZFS), not just tmpfs.
5. **COLD-2 (Bindings +16B side-table, ~+0.18s)** — lower priority; more invasive (touches the Bindings
   layout + every accessor); footprint win partially GC-proportional.

Net: HOT-1 + productionize-(b) are landed + quantified (b is a real 9× nix-core win; HOT-1 a ~10% hot win,
env-gated). The remaining leverage is COLD-1 (the all-users always-on tax), then HOT-1's DirectoryEntries
extension. Hot is genuinely faster (HOT-1) and the flake path much faster (b); cold is the next frontier.

### HOT-1 REFACTORED + #2 (DirectoryEntries) + #4 (default-on) — DONE + VALIDATED (2026-06-02, 47ecfdfd4) [⛔ #4 default-on REVERTED 2026-06-04 — see banner at section end]

Adversarial pass over the IMPLEMENTATION (the user flagged it wasn't concise/principled — correctly) →
cleaned up and landed #2/#4:
- **DELETED the dead git.cc token-cache.** productionize-(b)'s path-only split measured it adding NOTHING
  (158,911 vs 158,915), yet it was 24 env-gated lines + a cross-eval soundness caveat + a `static Sync<map>`
  (process-global mutable state) + a 2nd env var + a 3rd hand-rolled token. Only the cache-free path-only
  split remains (−33 lines + 2 orphaned includes removed).
- **HOT-1 (verifier.cc):** one `posixFreshnessToken(abs)` helper (was a 3rd hand-rolled, NUL-bug-prone token),
  reused for files AND dirs; `posixContentCacheEnabled()` — **DEFAULT-ON** [⛔ reverted to default-off
  2026-06-04, opt-in `NIX_EVAL_TRACE_POSIX_CONTENT_CACHE=1`] + escape hatch
  `NIX_EVAL_TRACE_NO_POSIX_CONTENT_CACHE=1` (#4, was a default-off getEnv read in TWO places);
  `posixHashCacheKey` extended to **DirectoryEntries** (#2; "pfb:"/"pdir:" class prefixes keep the two hashFns'
  keys disjoint); unified the 2 duplicate store blocks; fixed the populate hook's `h1Key`-misnaming.
- Counters kept UNIFIED (`nrFileContentCache*` span all tiers — rename = ~30-site churn + the H1 test, no gain);
  DDL documents the shared key space (store-path | pfb:token | pdir:token).

Net **−24 lines while ADDING #2+#4**. VALIDATED default-on: warm asciidoc `contentUs 29455→0 AND
directoryUs 11204→0` (987/987 hits); escape hatch restores both; byte-identical to `--no-eval-trace`; SOUND
(file same-size edit, dir entry add/remove all invalidate; file-content-inside-dir still hits — unit
`EvalTraceProperty_ReadDir`); **nix-expr-tests 1859 pass/4 skip**; **all 16 eval-trace FUNCTIONAL tests pass**
(cross-process pipeline incl. soundness/impure-soundness/recovery/deps). #2 + #4 COMPLETE. NEXT: #1 COLD-1.

> **⛔ #4 (default-on) REVERTED 2026-06-04 (`1c667c664`).** The "SOUND (file same-size edit …
> invalidate)" claim above is WRONG on a coarse-mtime filesystem: that soundness pass evidently
> ran on a fine-grained FS (tmpfs: 0/2000 collisions) — on ZFS (1888/2000) a same-size edit
> within the ~10ms coarse-clock tick produces an identical freshness token and stale-serves.
> `EvalTraceProperty_FilterList/SortList` were failing on this exact case and I wrongly dismissed
> them. HOT-1 is back to DEFAULT-OFF. See the RETRACTED banner under "HOT-1 IMPLEMENTED" above and
> `doc/eval-trace/measurements/property-test-overinvalidation-2026-06-04.md`.

### #1 COLD-1 REFUTED + COLD BISECTION ATTRIBUTION CORRECTED (2026-06-02) — the buckets were OVER-COUNTED; the tax is DIFFUSE, no clean lever.

Started #1 (COLD-1 = "source-accessor zero-overhead, ~+0.30s") by profiling the source-accessor on
`--no-eval-trace` firefox — and found my OWN COLD BISECTION's bucket attribution was WRONG:
- The bisection regex (`...|CanonPath|SourceAccessor|readFile|...`) SIGNATURE-MATCHES: `CanonPath` matches
  EVERY function taking a `CanonPath` arg (readFile, lstat, resolve, …), not just CanonPath operations.
  Re-running that exact regex on a fresh HEAD profile sums to **4.82%** (not 19.13%), and the matches are all
  genuinely small REAL accessor ops (`resolve` 0.42%, `insertIntoDirFdCache` 0.49%, FD-cache rb-trees ~1.9%).
  The earlier "19.13%" was inflated — signature over-match + a likely hybrid-PMU/inclusive-time artifact (the
  SAME hybrid-CPU trap that already burned the "54% GC").
- RIGOROUS re-measurement (`-e cpu_core/cycles/`, leaf-EXACT accessor matching, HEAD-disabled vs merge-base,
  converted to ABSOLUTE via %·wall on the 1.0s→1.7s evals):

  | bucket | merge-base | HEAD-disabled | ~abs Δ |
  |---|---:|---:|---:|
  | source-accessor (real) | 1.1% | 2.9% | **+0.04s** (NOT +0.30s) |
  | exception-unwinding | 0.4% | 2.1% | +0.03s (5× relative — the only concentrated eval-trace signal) |
  | operator new (alloc) | 2.9% | 3.4% | +0.03s |
  | **Bindings::get** | 2.4% | 1.3% | **~0** (NOT the +0.18s COLD-2 footprint lever) |
  | __tls_get_addr | 1.6% | 1.5% | ~0 |

  These named buckets sum to ~0.10s of the +0.71s tax. The REST (~0.6s) is a **~1.7× PROPORTIONAL increase
  across ALL eval functions** (cpu_core top-10 are core eval — ExprVar/callFunction/Bindings::get/yylex — at
  similar % in BOTH binaries). The +3.45B instructions / 2.4× cache-refs are SPREAD, not concentrated.

**Honest synthesis (cold arc, fully corrected).** The +70% always-on tax is REAL (perf stat, reproducible —
the user was right it isn't "dead"), but it is DIFFUSE pervasive integration overhead (routing + the +16B
footprint's diffuse cache cost + per-op checks), NOT a targeted subsystem. The COLD BISECTION buckets
(source-accessor 40%, Bindings 23%) were a FLAWED measurement; **COLD-1 and COLD-2 are NOT the +0.30s/+0.18s
levers I claimed (real: +0.04s / ~0). There is NO clean high-value cold lever** — the original "diffuse, no
clean lever" was correct, and the bucket attribution under pushback was the error.

**META-LESSON (owned):** re-opening under valid pushback does NOT excuse a sloppy measurement. A
signature-matching regex over-counts (every `f(CanonPath)` is not an accessor op), and the hybrid-PMU trap
recurs if you don't pin cpu_core. Attribute on cpu_core, leaf-exact, converted to absolute — every time.

**Cold options (all low-EV):** (a) the exception-unwinding (+0.03s, 5× growth) is the only concentrated
eval-trace signal — if a throw-as-control-flow eval-trace adds on the disabled path, removing it is ~2% +
a code-smell fix, but small; (b) a pervasive trim of the always-on routing/footprint (many ~1% changes);
(c) accept the tax (research branch; cache-ON is the point, where HOT-1/#2/(b) offset it). RECOMMENDATION:
do NOT chase a targeted cold lever — there isn't one. Addressing cold is an architectural "compile/route the
integration out when `--no-eval-trace`" project, not a lever.
