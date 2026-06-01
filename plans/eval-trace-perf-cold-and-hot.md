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

### H2. mtime/inode/size fast-path: skip the re-read when the cheap signal is unchanged (the core fix)
This is what makes hot ~instant and is the standard trick (Shake/Bazel/ccache all do it). Store, with
each FileBytes dep, a cheap freshness token — `(mtime, size, inode)` (a single `lstat`, ~ns) — alongside
the content hash. On verify: `lstat` the file; if `(mtime,size,inode)` matches the recorded token,
TRUST the recorded content hash (no read, no hash). Only re-read+re-hash when the token differs.
- This collapses the 29K content re-hashes (0.12s + the readFile I/O) to 29K stat()s (microseconds
  total) for the overwhelmingly-common unchanged case.
- Soundness: mtime can be forged (mtime-preserving edits). Two postures: (a) trust mtime (Bazel/Shake
  default — fast, standard, accepts the adversarial-mtime edge as out-of-scope, which Nix's eval model
  already does for file-changes-during-eval); (b) mtime as a NEGATIVE filter only (mtime changed ⇒
  definitely re-hash; mtime same ⇒ still re-hash but this is useless). (a) is the real win and matches
  every production build cache. Gate behind a setting if conservatism is wanted.
- Implementation site: `dep-resolution-service.cc:375` (the FileBytes computePathHashedDep lambda) +
  the FileBytes dep record needs to carry the token (schema bump: add a stat-token column or fold into
  the dep value). The verify path already has `maybeLstat()` in hand (line 291) — the lstat is ALREADY
  being done for existence; this reuses its result instead of discarding it.
- Leverage: this is THE hot fix. Combined with H1's persistence it makes warm verify O(stat per file)
  not O(read+hash per file). Hot should drop from ~1.0s toward the stat-floor.

**H2 PREMISE VALIDATED (adversarial pass, 2026-05-31).** Confirmed the 29K expensive content re-hashes
are on mtime-STABLE files, not mtime-useless store paths:
- The interned dep-key strings (Strings table, hot-stats/1 DB) point at nixpkgs SOURCE files
  (`/home/cbaker2/ext-sources/nixpkgs/pkgs/top-level/all-packages.nix`, `lib/default.nix`, …) — read
  through the source accessor. `/nix/store` builds are the CHEAP `StorePathAvailability` existence
  checks (`storePathUs`=0.6ms), NOT the expensive FileBytes (`contentUs`=0.12s). So the expensive
  re-hashes are SOURCE files.
- Those source files have real, stable mtimes (set at git-checkout, unchanged across read-only eval
  processes). So mtime IS a trustworthy freshness signal for exactly the files that dominate hot cost.
  My initial worry (store paths have epoch-1 normalized mtimes → mtime useless) does NOT apply — store
  paths aren't the expensive deps.

**Accessor nuance (design must handle):** eval reads source via a `SourceAccessor` (FS accessor for
dirty `git+file` worktrees; possibly content-addressed/store-backed for locked flake inputs). The
freshness token must come from the ACCESSOR's `getFingerprint`/lstat, not a raw POSIX `lstat` on a
reconstructed path — `SourceAccessor::getFingerprint` already exists (used by the `srcToStore` guard,
eval-environment.cc) and returns nullopt for unfingerprinted (dirty) accessors. Design: token =
accessor fingerprint if present, else `(mtime,size,inode)` via the accessor's lstat. For the
content-addressed-source case the accessor fingerprint IS the content identity → even cheaper (no
stat). This aligns the token source with how the dep's path identity is already resolved
(`SemanticRegistry`/`input-resolution.cc`).

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

## Recommended order (tractable → architectural)
1. **H2 (mtime fast-path)** — the core hot fix, standard, highest leverage/cost ratio. Reuses the
   already-performed `maybeLstat`. Start here.
2. **H1 (persist content-hash cache)** — complements H2 (persist the (path, token, hash) triple).
3. **C1 (fire-and-forget recording)** — the core cold fix; removes recording from the eval critical path.
4. **H3 / C2 / C3** — architectural follow-ons once 1-3 are measured.

H2+H1 target "hot should be instant"; C1 targets "cold shouldn't block eval". Both are independent of
(and more valuable than) the dead producer-partition direction. NEXT: validate H2's premise with a
measurement — what fraction of the 29K content misses are on files whose mtime is genuinely unchanged
across the prime→hot boundary (i.e. how much H2 would actually save). Then implement H2 behind a
setting + bench.
