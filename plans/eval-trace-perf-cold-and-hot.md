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
