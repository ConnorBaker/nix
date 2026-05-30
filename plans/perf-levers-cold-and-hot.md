# Making cold AND hot faster — grounded lever analysis (2026-05-30)

Based on measured cost structure of the deep-attrset workload (`python3Packages`
outPaths via `mapAttrs`, release binary, 10,397 traces). Each claim ties to a
counter or DB query, not speculation.

## The single root cause both cold and hot share

Measured facts:
- **36.5 M deps recorded across 10,397 traces, but only ~60 K distinct**
  (`Strings` table). **Each distinct file dep is recorded ~607× on average** —
  once per package whose closure transitively reads it (stdenv, glibc, the
  python interpreter, setup hooks, …).
- **DepKeySet sharing = ZERO**: 10,397 traces → 10,397 distinct `DepKeySets`.
  Closures overlap massively but are never byte-identical, so whole-set
  content-addressing never dedups.
- `values_blob` = 343 MB, `keys_blob` = 52 MB (avg ~34 KB values + ~5 KB keys per
  trace).

**Cold cost** (`record.hashUs` 30 s of 65 s CPU): for every trace, `recorder.cc`
does THREE full hash passes over its entire sorted dep vector
(`computeTraceHashFromSorted` + `computeFullTraceHashFromSorted` +
`computeDepKeySetHashFromSorted`, recorder.cc:44-46), plus `sortAndDedupDeps`
(step 1) over ~3,500 deps. That is 3 × 36.5 M = ~110 M dep-hash-feeds, most of
them re-hashing the same ~60 K deps over and over.

**Hot cost** (`verify.timeUs` 4.85 s, `loadTrace` 1.25 s): `verify.depsChecked =
36.9 M` — re-verify every dep of every trace. The L1 dep-hash cache absorbs most
recompute (`depHash.cacheHits = 21.3 M`, only 61 K misses), so hot is NOT
recompute-bound; it is bound by the **bookkeeping of walking 36.9 M dep entries +
loading 10,397 trace blobs**.

So: **both cold and hot scale with total-deps-recorded (36.5 M), which is ~607×
the distinct-deps (60 K).** Every lever below attacks that multiplier or the
per-dep constant.

## Levers, ordered by (payoff × tractability), with the pattern each helps

### L-A. Shared-sub-closure dep factoring (biggest structural win; helps cold + hot + storage)
The 607× duplication is the disease. Most of any package's ~3,500 deps are its
**shared build closure** (stdenv + toolchain + common python deps), identical
across thousands of packages. Today each trace re-lists and re-hashes them.

Idea: factor common dep *subsets* into shared, content-addressed **dep-fragment**
rows (a middle layer between `Dep` and `DepKeySets`). A trace's key-set becomes
"fragment refs + its own unique tail." Recording hashes each fragment ONCE;
verification checks each fragment ONCE per session and reuses the verdict across
all traces that reference it.
- **Cold:** hash 60 K distinct deps once (grouped into fragments) instead of
  36.5 M dep-feeds → the 30 s `record.hashUs` could drop toward ~1 s.
- **Hot:** `verify.depsChecked` collapses from 36.9 M to (distinct fragments +
  per-trace tails); the per-session "fragment X verified" memo means stdenv is
  checked once, not 10,397×.
- **Storage:** `values_blob` 343 MB → mostly shared fragment blobs.
- Cost: real design work (a new storage layer + fragment-extraction at record
  time). This is the structural fix the whole arc keeps pointing at; it is the
  high-payoff item.
- **Premise CONFIRMED (2026-05-30):** 36.5 M total deps from ~60 K distinct atoms
  across 10,397 traces ⇒ each distinct dep appears in **~607 traces on average** —
  arithmetically forced, massive cross-trace reuse is real. `keys_blob` length
  clustering corroborates near-identical closures (266 traces at exactly 6,857 B,
  251 at 7,019 B, …).
- **Hard part:** the 10,397 keysets are all DISTINCT (whole-set dedup gets zero
  hits), so L-A needs **sub-set / fragment** factoring (find recurring dep
  *subsets* within differing keysets), not whole-keyset sharing. That is the
  harder design — fragment boundary selection (by source? by closure component?)
  is the open question. A naive "longest-common-prefix of sorted deps" may capture
  much of it given the length clustering, but needs prototyping.

### L-B. Session-level dep-verdict memo (hot; cheap; partial overlap with L-A)
Hot re-verifies the same file dep across thousands of traces. The L1
`depHash` cache already memoizes *hash recompute* (21.3 M hits), but
`verify.depsChecked` still WALKS all 36.9 M entries and compares. A
per-session `Set<DepKeyId>` of "already-verified-this-session, unchanged" deps
would let verification skip the compare entirely for the ~60 K distinct deps
after first sight.
- **Hot only.** Smaller than L-A (doesn't cut cold or the 36.9 M walk itself,
  just the per-entry work), but far cheaper to build and independently shippable.
- Soundness: a dep verified once in a session stays valid for that session (same
  filesystem snapshot) — the existing L1 invariant already assumes this.

### L-C. Cheaper recording hash: one pass, not three (cold; cheap; safe)
`recorder.cc:44-46` computes three hashes over the same sorted vector. They feed
different domains (trace / full / key-set) but traverse identical data. Fuse into
a single pass that emits all three digests (one walk, three running hash states),
or skip the ones not needed for a given trace.
- **Cold only**, ~up to 2/3 off `record.hashUs` in the limit (less in practice —
  the walk/feed is shared but the hash finalize differs). Low risk, localized to
  the recorder. Good first increment because it is independent of L-A.

### L-D. Don't record traces for trivially-cheap leaves (cold + storage; needs a gate)
10,397 traces for 11,061 packages ≈ one per package. Many package `outPath`
evals are cheap; recording a 3,500-dep trace to cache a sub-millisecond eval is
net-negative per-leaf. A heuristic "only record if this trace's own eval cost
exceeded T" would skip the long tail.
- **Cold + storage.** Risk: needs a cost estimate at record time (thunk-count
  delta or wall delta for the trace), and it trades away some hot hits. Measure
  the eval-cost distribution per trace first; if it is bimodal (few expensive,
  many trivial) this is a clean win, if flat it is not worth it.

### L-E. Lazier / batched DB writeback (cold; medium)
Cold `record.flushUs` is small here (~few s) but `record.serialize*` + per-trace
`flush(ea)` (recorder.cc:77) runs per trace. Batching the SQLite writeback
(accumulate N traces, one transaction) and the 343 MB values_blob writes amortizes
syscall/transaction overhead. Already partially in place; verify whether per-trace
flush is the residual after L-A/L-C.

### L-F. Access-pattern guidance (both; zero-code, already half-proven)
Measured: `mapAttrs` records 3× fewer deps than `attrNames`+per-key-select for
the SAME result (110.9 M → 36.5 M), making cold 2× cheaper and hot a clear win.
So: **document that consumers should iterate via C++-level mechanisms
(`mapAttrs`, direct attr iteration) rather than `attrNames`+string-select**, and
check whether `nix-eval-jobs` already does. Zero engine change; pure consumer
guidance. It does not reduce the 607× (that is closure overlap, not access
pattern) but it removes the *additional* keyset+per-select over-recording layer.

## Recommended sequence
1. **L-C (fused recording hash)** — cheap, safe, cold-only, independent. Ship first.
2. **L-B (session dep-verdict memo)** — cheap, hot-only, independent. Ship second.
3. **L-A (shared-closure dep fragments)** — the structural win for BOTH; design
   it properly (it is the real project), informed by 1–2's measurements.
4. L-D / L-E / L-F as opportunistic add-ons.

## Caveats (honest)
- All magnitudes from n=1 on `python3Packages` (mapAttrs). The 607× and zero-
  keyset-sharing are DB facts (robust); the wall-time projections for each lever
  are estimates until prototyped.
- L-A is the only lever that addresses the root cause; the rest are constant-
  factor. If only one thing is built, it is L-A — but L-C/L-B are the cheap
  down-payments that also de-risk L-A's design.
- Every lever must pass the standing gate: 10-commit byte-identical correctness +
  the keyset-escape/soundness tests; recording changes must preserve
  `served == truth` (already the harness check).
