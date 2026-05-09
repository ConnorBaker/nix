# compare-runs / compare-logs analytics consolidation

During the round-5 eval-trace SV audit (`doc/eval-trace/claude-md-audit.md`
§4 Round-5, §5) I ran ~25 ad-hoc Python scripts and shell one-liners
against the per-commit `stats.json`, `timing.json`, `debug.log` files
and the eval-trace SQLite DB (`~/.cache/nix/eval-trace-v<kSchemaEpoch>-<hash-algorithm>.sqlite`
— v20 when these analyses were originally written; `kSchemaEpoch` in
`trace-store.hh` is the source of truth for the current version).
Every one of those analyses should be a built-in flag or subcommand of
`compare-runs` (or `compare-logs`).  The data sources the scripts
consult are limited; the analyses are repeatable.  This document
enumerates the analyses and the queries they rely on, as a reference
for building out the tools.

Goal: after this consolidation, a future benchmark investigation
should be answerable by `compare-runs --flag=X` or `compare-logs
--flag=Y` without writing throwaway scripts.

---

## Data sources

Every analysis below pulls from a subset of these sources.  Any
consolidation should expose them uniformly.

1. **Per-commit `stats.json`** (emitted by `nix eval` via
   `NIX_SHOW_STATS=1`).  Contains the `evalTrace` counter subtree
   plus standard nix eval stats (`nrThunks`, `cpuTime`, etc.).  Per
   run name × commit.
2. **Per-commit `timing.json`** (emitted by `generate-runs`).  Single
   field `wallTime`.
3. **Per-commit `debug.log`** (emitted by `nix eval --debug`).  Text
   log of `eval-trace/*` diagnostic lines tagged by subsystem
   (`eval-trace/recovery`, `eval-trace/verify`, etc.).  Typically
   15-30 MB per commit on SV-heavy commits.
4. **Eval-trace SQLite DB** at
   `~/.cache/nix/eval-trace-v<kSchemaEpoch>-<hash-algorithm>.sqlite`.
   Schema in `src/libexpr/eval-trace/store/trace-store-lifecycle.cc`;
   `kSchemaEpoch` in `trace-store.hh` is the source of truth for the
   version number.  Captures the cache state *at the end of the cold
   run* — it is not per-commit, but it reflects the cumulative state
   the benchmark ended with.
5. **`~/.cache/nix/attr-vocab.sqlite`**.  `AttrNames` and `AttrPaths`
   tables used to resolve attr_path IDs back to human-readable
   names (e.g., `closures.gnome.x86_64-linux`).
6. **Commit-order context from `git log`** on the nixpkgs checkout.
   `generate-runs` iterates `git log -N --format=%H` (newest first);
   any per-commit indexing needs this order to align time series.

---

## Analyses I ran, grouped by theme

Each numbered item is a question I answered with a script; they
overlap significantly across investigations and should consolidate
into fewer built-in reports.

### A. Per-commit time-series aggregates (degradation shape)

**A1. Cold-wall trend over processing order.**  Builds a dataframe of
`(proc_idx, commit, cold_wall, ref_wall, cpuTime, ...stats)` sorted by
generate-runs processing order, then bucketed into 50-commit windows.
Asks: does mean cold wall grow monotonically with processing order?
Reports slope, intercept, and Pearson r of `cold_wall` vs `proc_idx`.

**A2. Per-bucket SV activity growth.**  For each 50-commit bucket,
report mean `sv_candidates`, mean `sv_deps_resolved`, max candidate
count, max wall.  Shows the "sv scales with DB size" story.

**A3. Per-bucket SV time decomposition.**  For each bucket, report
mean `sv_depResolve_us`, `sv_loadKeySet_us`, `sv_hash_us`, total SV
time, and SV's share of cold wall.  Isolates which SV phase is
dominant.

**A4. Outlier table.**  Top-N slowest and fastest commits by
`cold_wall` with columns for `sv_candidates`, `sv_deps_resolved`,
SV subtime breakdowns, `verify_us`, `record_us`, `loadTrace_us`.

**A5. Degradation within a commit class.**  Partition commits by
recovery-outcome class (see C below), then A1-A3 within each class.
Critical insight: mean cold wall can look flat while worst-miss
commits clearly degrade — masked by the hit/miss mix varying over
buckets.  `compare-runs` should offer `--bucket-window=N` and
`--split-by=class`.

### B. Recovery/SV outcome classification per commit

**B1. Class partition.**  For every commit, bucket into:
- *Hit-only*: `recovery.failures == 0`
- *Partial miss*: `0 < recovery.failures < recovery.attempts`
- *Full miss*: `recovery.failures == recovery.attempts`
Report counts, mean wall per class, mean `ref_wall` per class,
and `cache_ratio = cold_wall / ref_wall` per class.  This is the
single most useful partition I found.

**B2. Per-failure-count breakdown.**  `fails == 0`, `fails == 2`,
`fails == 4`, etc.  For each distinct value report mean wall and
mean `nrThunks`.  Showed cleanly that each failed leaf attr adds
~3.8 s of wall and ~5.2 M thunks.

**B3. Hit path used.**  On hit-only commits, how many served via
primary cache vs via DirectHash vs via SV?  Uses `evalTrace.hits`,
`recovery.directHash.hits`, `recovery.structVariant.hits`,
`recovery.gitIdentityHits`.

**B4. Miss-cost decomposition.**  For each miss class, mean breakdown
of: `cpuTime`, `verify_us`, `verifyTrace_us`, `recovery_us`,
`sv_us`, `dh_us`, `loadTrace_us`, `record_us`, plus implied
"evaluator proper" time (`cpuTime - ref_cpuTime - accounted_trace_time`).

### C. Flat vs growing per-call costs

**C1. Per-dep SV resolve cost.**  Aggregate
`sum(sv_depResolve_us) / sum(sv_deps_resolved)` to ns/resolve.
Report by processing-order bucket.  Flat-across-buckets indicates
per-lookup cost is bounded; scan-count growth explains degradation.

**C2. Per-load cost for SV blob reads.**  Aggregate
`sum(loadTrace_us) / sum(loadTrace_count)` to µs/load.  Should be
flat across run; grows would indicate DB I/O regression.

**C3. L1 hit rate.**  `depHash.cacheHits / (cacheHits + cacheMisses)`
per commit and per bucket.

### D. `byDepKeySet` telemetry (per-DepKeySetId SV outcomes)

The `evalTrace.structVariant.byDepKeySet` array in stats.json has
one entry per DepKeySetId SV tried, with fields
`{depKeySetId, tried, succeeded, abortedEarly, hashMismatch, avgDeps, avgUs}`.
Several analyses work directly on this.

**D1. Bucket-success histogram.**  Across all commits, for each
bucket, compute `succeeded / tried`.  Bin by rate (0%, <10%, 10-25%,
...).  Answers "is success a learnable distribution?" for adaptive
scheduling.

**D2. Pareto of tries.**  Fraction of total tries contributed by
top-N buckets.  If 50% of tries come from 5% of buckets, scheduling
matters; if uniform, it doesn't.

**D3. Persistent-fail buckets.**  Count of buckets that `succeeded == 0`
across all commits that tried them.  Drives the OPP-1 ceiling.

**D4. OPP-1 simulation.**  Replay the bucket-try sequence in
processing order with a "skip after N consecutive fails" heuristic.
For thresholds T ∈ {3, 5, 10, 20}, count `(saved_tries, missed_wins)`.
Extended version weights by `avgUs × tried` to get time-weighted
savings.

**D5. Winner rank in DESC bucket-id order.**  For each commit where
SV succeeded, determine the winning bucket's rank within the set
of tried buckets sorted by DESC bucket_id (as a proxy for
DESC trace_id).  Report rank mean, median, and fraction in top-3 /
top-10.  Drives the "order-preserving vector" ceiling.

**D6. Abort-outcome breakdown.**  Aggregate `succeeded`, `abortedEarly`,
`hashMismatch`, `tried` across all buckets × all commits.
Answer: of all SV tries, what fraction hits, what fraction aborts
early, what fraction completes full-iteration but hash-mismatches?

### E. Debug-log grep-driven analyses

The `--debug` log lines eval-trace emits are structured.
Several analyses grep them directly rather than stats counters.

**E1. SV abort-reason distribution.**  `grep -oE "reason=[a-zA-Z]+"
debug.log | sort | uniq -c`.  Variants: `resolveFailed`, `volatile`,
`traceContextMiss`.  Total across all cold commits.  Showed that
100% of real-world aborts are `resolveFailed`, 0% volatile /
traceContextMiss.

**E2. SV abort-dep kind distribution.**  For `reason=resolveFailed`
aborts, `grep -oE "kind=[a-zA-Z]+"` — which CQK kinds are failing?
In my dataset, 100% were `structuredProjection`.

**E3. Top-failing-keys.**  For `reason=resolveFailed`, extract and
count the `key='<path>@<format><suffix>'` strings.  Normalise path
so only file-path + shape-suffix is counted.  Shows the handful
of files responsible for the bulk of aborts (Cargo.lock,
pyproject.toml, etc.).

**E4. Hash-mismatch dep-count distribution.**  For
`hash-mismatch: ... (N deps resolved)` lines, extract `N` and
report distribution (min, p25, p50, p75, p95, max, mean).  Tells
us how many deps SV iterated before giving up on a hash-mismatch
candidate.

**E5. DirectHash recompute size distribution.**  For
`recomputed N/M dep hashes for ...` lines, extract `(N, M)` pairs.
Classify as "full" (`N == M`) vs "partial" (`N < M`).  Report
count, mean M, savings potential from early-exit on partial events.

**E6. Recovery success counts.**  `grep -c "structural variant
recovery succeeded"` and similar for DirectHash / GitIdentity.
Cross-check against stats counters `recovery.*.hits`.

### F. SQLite-DB post-mortem queries

These inspect the cumulative trace-DB state at run end.

**F1. Table row counts.**  Traces, DepKeySets, History, Strings,
DataPaths, Results, Sessions, SessionRuntimeRoots, DirSets.
One-line `SELECT COUNT(*)` per table.

**F2. Blob-size distribution.**  `SELECT LENGTH(values_blob) ...`
histogrammed into size bins (< 1 KB, 1-10 KB, 10-100 KB,
100 KB-1 MB, > 1 MB).  Also total bytes in the two blob columns.
Correlate total to DB file size.  Identified the ~650 KB/trace
clustering that explains load cost.

**F3. DepKeySet sharing distribution.**  How many DepKeySets
have 1 / 2 / 3+ traces?  `SELECT traces_per_dks, COUNT(*) FROM
(SELECT dep_key_set_id, COUNT(*) AS traces_per_dks FROM Traces
GROUP BY dep_key_set_id) GROUP BY traces_per_dks`.  Observed
~99% singletons which retires the "multi-trace optimization"
ideas.

**F4. Per-attr_path history distribution.**  How many distinct
(recovery_key, attr_path_id) pairs are there?  How many traces
per pair?  Tells us which attr paths are "big" (history of
hundreds) vs "small" (singletons).

**F5. Attr path ID → human name.**  Lookup into
`attr-vocab.sqlite` to resolve attr_path_ids (e.g., 36) to
dotted names (`closures.gnome.x86_64-linux`).  Every analysis
that surfaces attr_path_ids should emit the human name
alongside.

**F6. Trace-id vs dep-key-set-id correlation.**  For a given
attr_path, plot `(trace_id, dep_key_set_id)` ordered by trace_id
DESC.  Tests whether bucket_id is a reasonable proxy for
trace_id (answer: near-perfect for the gnome workload).

**F7. Multi-attr-path trace sharing.**  Does any trace_id appear
in the History of more than one attr_path?  In my dataset: no —
history is per-attr-path disjoint.  Useful for ruling out
cross-attr optimization ideas.

**F8. Per-page-size and freelist counts.**  `PRAGMA page_count`,
`PRAGMA page_size`, `PRAGMA freelist_count`.  Sanity check that
DB isn't pathologically fragmented.

### G. Cross-run comparisons (A/B benchmarking)

When I had two cold/N runs from different code versions, the
analyses in A-D above were repeated pairwise.

**G1. Same-commit pairwise delta.**  For each commit present in
both runs, compute `(run_A_wall, run_B_wall, delta, ratio)`.
Summarise as total, mean, and distribution.  Essential for
validating any code change.

**G2. Per-component time delta.**  Same-commit deltas for
`sv_depResolve_us`, `sv_loadKeySet_us`, `recovery_us`,
`verifyTrace_us`, `record_us`, `loadTrace_us`.  Attributes
overall wall change to specific machinery.

**G3. Recovery-outcome delta.**  How did hit/miss counts per
commit change?  A change that preserved wall but swapped hits
between SV/DirectHash is not actually neutral — it may have
shifted the DB state in a way that ripples across subsequent
commits.  Surfaces the "path-dependent cache state" hazard the
order-preserving-vector experiment hit.

**G4. DB-state comparison.**  F1-F7 run against two DBs from
different runs.  Different DB shapes (trace counts, bucket
distributions) indicate the code change altered recording or
recovery behavior beyond pure speed.

### H. `compare-logs` per-commit event timeline

`compare-logs` already covers much of what's below, but I
consulted it repeatedly and wanted to call out:

**H1. Total `evalTrace` counter set, not just the subset it
currently prints.**  The existing `compare-logs` table truncates
the counter name to `evalTrace…` with ellipsis — forces you to
grep `stats.json` directly.  Emit all counters with full names,
sortable.

**H2. Per-kind dep-hash time breakdown.**
`nrDepHashContentUs`, `nrDepHashStructuredOuterUs`,
`nrDepHashStructuredJsonUs`, `nrDepHashStructuredTomlUs`,
`nrDepHashStructuredNixUs`, `nrDepHashGitIdentityUs`, etc.
Shows which CQK is expensive in verify.

**H3. Cross-linked event / counter view.**  For a given cold
commit, show the sequence of PASS_0_START / ROOT_VALUE /
PASS_0_END alongside the counter totals so timing and events
can be correlated.

---

## Cross-cutting consolidation observations

1. **The per-commit dataframe extraction is boilerplate.**  Every
   script I wrote began with "walk processing order, load
   `stats.json`/`timing.json`, flatten into records, dump to
   `/tmp/*.json`."  `compare-runs` should expose this once and
   let callers either query it in code (a library) or dump it
   (`--csv`, `--parquet`, `--json`).

2. **Log aggregation is done by shell one-liners today.**  Every
   analysis in §E was a `grep | sort | uniq -c | sort -nr | head`
   pipeline.  `compare-logs` should have first-class support
   for the structured debug-log format (each line is
   `eval-trace/<subsystem>: <event> <kv>=<value> ...`) so you
   can say `compare-logs --group-by=reason --from=eval-trace/recovery`
   and get a frequency table without grep.

3. **SQLite queries are typed but I wrote the SQL manually each
   time.**  A `compare-runs --db-inspect` mode that runs F1-F8
   against a given DB file (or the current one) would save that
   effort.  Bonus: resolve `attr_path_id` → human name
   automatically from `attr-vocab.sqlite`.

4. **The commit-class partition in B1 should become the default
   view.**  My investigations repeatedly rediscovered that
   overall means are misleading when hit/miss ratios drift.
   `compare-runs` should emit per-class summaries by default
   (or behind `--split-by=outcome`).

5. **`byDepKeySet` analyses are currently invisible.**  Nothing
   in compare-runs or compare-logs consumes this array.  It's
   the richest per-commit telemetry we have on SV behavior and
   deserves a dedicated section (or subcommand).

6. **Processing-order vs commit-date.**  generate-runs iterates
   newest-first, but stats files are named by commit SHA.
   Aligning "bucket 0-49" to "first 50 commits processed"
   required re-running `git log`.  Embed processing order as a
   field in a manifest emitted by generate-runs.

7. **Simulation mode.**  The OPP-1 simulation (D4) replays a
   recorded history with a different strategy to see what WOULD
   have happened.  This is powerful — more proposed heuristics
   (newest-first, bucket-id prioritisation, adaptive scheduling)
   could be simulation-tested against recorded `byDepKeySet`
   traces before writing code.  A `compare-runs
   --simulate=OPP-1 --threshold=20` or similar entry point
   formalises this.

8. **Pairwise run comparison is a first-class need.**  The
   current `compare-runs` does multi-run tables, but it doesn't
   do the "same commit, what's the delta in component X?"
   breakdown the code-change loop needs (G1-G2).

---

## Concrete work items for the consolidation session

Ordered by how much investigation pain they'd reduce.

1. **Per-commit dataframe export + Python API.**  One pass
   over `{run}/{commit}/` trees producing a tidy dataframe
   keyed by `(run_name, commit_sha, proc_idx)` with all flattened
   counters.  Enables all A/B/C/D/G analyses in one file.

2. **Commit-class partition as a built-in.**  B1-B4 tables and
   their per-bucket (`A5`) variants.

3. **`byDepKeySet` analyser.**  D1-D6 as a single
   `compare-runs --by-dep-key-set` subcommand.

4. **Debug-log structured grep.**  Parse `eval-trace/<sys>: ...`
   lines into key-value events; expose `compare-logs
   --count-events reason kind file` equivalents.

5. **DB inspector.**  F1-F8 as `compare-runs --db-inspect`
   with attr-vocab name resolution.

6. **OPP-1-style simulation harness.**  Framework to replay
   recorded `byDepKeySet` history under alternate strategies.
   Initial targets: persistent rejection thresholds (D4),
   newest-first ordering, success-weighted scheduling.

7. **Pairwise A/B comparison mode.**  `compare-runs --baseline
   cold/4 --target cold/5` producing G1-G4.

8. **Deduplicate between compare-runs and compare-logs.**  Both
   scripts independently parse stats.json and walk run dirs.
   Pull the walker + parser into a shared module.
