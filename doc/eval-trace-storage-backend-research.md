# Evaluation trace storage backend research

This note records the current performance findings and the proposed direction for
moving eval-trace storage away from SQLite when a custom format is the better
fit. The benchmark target is not to match the pre-schema SQLite cache. The target
is to beat it while preserving trace soundness and precision.

## Current benchmark facts

The comparable 100-commit nixpkgs closure runs currently show:

| run | cold wall | hot wall | cold hits/misses | hot hits/misses | writeback |
| --- | ---: | ---: | ---: | ---: | ---: |
| pre-schema SQLite, run 121 | 432.7s | 98.6s | 599/101 | 700/0 | 7.4s |
| normalized SQLite, run 132 | 575.9s | 94.4s | 599/101 | 700/0 | 152.2s |
| Git full manifest, run 133 | 617.3s | 166.0s | 599/101 | 700/0 | 170.0s |
| Git dirty segments, run 134 | 845.6s | not accepted | 599/101 | not accepted | 21.0s |
| Git checkpointed segments, run 135 | 590.0s | 257.7s | 599/101 | 700/0 | 31.3s |
| Git checkpointed segments, run 136 | 565.9s | 247.9s | 599/101 | 700/0 | 28.6s |
| external payload objects, run 137 | 458.1s | 103.5s | 599/101 | 700/0 | 7.7s |
| rejected payload-pack attempt, run 138 | 458.4s | 103.2s | 599/101 | 700/0 | 7.5s |

Run 134 made shutdown much faster, but startup became the bottleneck because
each startup had to replay a long segment chain and repeated full intern pools.
Run 135 bounded the worst replay behavior and kept output equivalence, but was
not viable. Run 136 reduced manifest size slightly with segment-local data-path
dictionaries. Run 137 moved trace-capsule payload bytes into content-addressed
external objects and left only payload hashes/metadata in the generation
manifest. That removed the large startup replay cliff and restored writeback to
roughly the pre-schema SQLite range, but still does not beat the pre-schema
target: cold remains 25.4s slower and hot remains 4.9s slower on the comparable
100-commit run set.

## Research conclusion

SQLite is a good embedded application-file database, but this workload is not
primarily a relational query workload. Runtime already wants a disconnected
in-memory snapshot. Shutdown wants to publish a batch of immutable observations.
Hot startup wants compact mmap-friendly indexes and lazy payload materialization.

The best final shape is:

```text
immutable generation manifest
append-only content-addressed capsule/result packs
compact mmap hot indexes
append-only candidate/runtime-root/session events
atomic head publication
asynchronous compaction
```

SQLite can remain useful as a migration validator, but it should not remain the
authoritative final data plane if it keeps forcing B-tree/index maintenance and
SQL crosswalk work into shutdown.

## Source and pattern anchors

- SQLite documents why it is attractive as an application file format, but also
  notes that transaction state includes rollback journal or WAL files. That is
  exactly the machinery we are trying to avoid on the shutdown hot path.
  https://www.sqlite.org/appfileformat.html
  https://www.sqlite.org/fileformat.html

- Git pack/index files are the closer precedent for this workload: immutable
  compressed objects, checksums, object-id lookup tables, fanout/chunk indexes,
  and multi-pack indexing.
  https://git-scm.com/docs/pack-format.html

- FlatBuffers and similar offset-based formats are relevant for hot projection
  files because they allow schema-guided in-place access without a parse step.
  They are not a complete storage backend by themselves.
  https://flatbuffers.dev/white_paper/

- LMDB/MDBX-style mmap KV stores are plausible fallback backends. They improve
  read-heavy access and sorted bulk insert, but they still impose general KV
  B-tree semantics on a workload that wants sequential immutable generation
  publication.
  https://lmdb.readthedocs.io/

- Lucene and LevelDB provide the clearest publication analogues: immutable
  segments/SSTables plus a small current-generation pointer or manifest. The
  storage format can be cache-specific, while publication and recovery follow
  the same append/build/verify/publish pattern.
  https://lucene.apache.org/core/2_9_4/fileformats.html
  https://github.com/google/leveldb/blob/main/doc/impl.md

- Publication must use explicit crash-consistency rules. `rename()` gives atomic
  visibility, and `fsync()` on the containing directory is needed to make new or
  renamed directory entries durable.
  https://man7.org/linux/man-pages/man3/rename.3p.html
  https://man7.org/linux/man-pages/man2/fsync.2.html

## Soundness constraints

- Persistent numeric IDs are not durable identity.
- Process-local interned IDs are runtime handles only.
- Durable identity must be content-key or natural-key based.
- Candidate heads, recovery indexes, structural-variant probes, Git identity
  indexes, and mmap hot indexes are projections only.
- Trace/result capsules and verified candidate events remain authoritative.
- Ambiguous or unverifiable data must become a miss, never a guessed hit.
- Corruption, hash collision, schema mismatch, missing payload, or stale
  projection must fail closed.
- Concurrent writers must not lose observations. Head publication must be
  compare-and-swap or protected by a narrow writer lock.
- Dirty observations are cleared only after all referenced objects are durable
  and the new head is committed.

## Backend task list

1. Treat the current Git segment implementation as a learning step, not the
   final architecture, unless it beats the target and passes the adversarial
   semantic review.
2. Add a custom generation-store prototype behind the existing storage seam.
3. Preserve the existing trace capsule ABI initially. Do not redesign result or
   dependency codecs in the first backend slice.
4. Write one immutable generation file or manifest plus pack files at shutdown.
5. Load by mmap or bounded sequential reads at startup, then close mutable
   handles.
6. Store payloads by content key and store candidate/runtime-root/session events
   as append-only records.
7. Build hot indexes as rebuildable projections from committed events.
8. Add CAS or narrow-lock head publication with fsync discipline.
9. Add tests for local-ID conflict, concurrent publish, crash recovery,
    corruption, stale projection, and dirty-provenance behavior.
10. Add benchmark gates requiring cold and hot runs to beat pre-schema SQLite
    without reducing hit rate or reference-output equivalence.

## First custom-store slice

The first implementation should be intentionally small:

```text
generation manifest
payload pack
candidate event table
runtime root event table
string/data-path dictionaries keyed by content
latest-head projection rebuilt on load
atomic CURRENT publication
```

The first slice can rewrite the whole generation. That gives a clean hot-startup
and soundness baseline. After that works, add append-only deltas and compaction
to reduce cold write amplification.

## Benchmark gate

A backend change is architecture-positive only if:

- Cold 100-commit wall time is materially below 432.7s.
- Hot 100-commit wall time is materially below 98.6s.
- Cold hit/miss remains at least 599/101 on the comparable run set.
- Hot hit/miss remains 700/0.
- Reference output equivalence is preserved.
- Instrumented bottlenecks explain the wall-clock movement.
- The change does not introduce hidden costs in startup replay, fsync, payload
  duplication, allocations, or page faults.

## Open adversarial questions

- Can payloads be made segment-local without embedding process-local IDs?
- Should candidate ordering preserve serial commit order or become
  content-order-invariant?
- What is the minimum projection needed for exact hot hits?
- Which recovery indexes are worth persisting if they are only fail-closed
  filters?
- Can structural-variant probing avoid hashing or decoding full payloads?
- How much of the current result JSON codec can remain before it dominates hot
  materialization?
- What compaction policy keeps startup bounded without reintroducing full
  shutdown rewrites?

## Benchmark update: run 136

Variant: checkpointed Git manifest with segment-local data-path dictionaries, but with the process string dictionary preserved in every manifest. The attempted empty string dictionary was rejected after it caused functional cache instability; preserving string interning side effects restored the build.

Results over the 100-commit eval-trace-bench set:

| run | wall | hits/misses | init | writeback | Git bytes | payload bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cold/121 | 432.676s | 599/101 | 5.752s | 7.352s | 0 | 0 |
| cold/135 | 589.950s | 599/101 | 118.156s | 31.257s | 1.830GB | 1.405GB |
| cold/136 | 565.905s | 599/101 | 99.928s | 28.583s | 1.599GB | 1.405GB |
| hot/121 | 98.631s | 700/0 | 7.224s | 0 | 0 | 0 |
| hot/135 | 257.675s | 700/0 | 158.539s | 0 | 0 | 0 |
| hot/136 | 247.855s | 700/0 | 148.329s | 0 | 0 | 0 |

Pairwise:

- cold/136 vs cold/135: -24.045s, 91 commits faster, mean ratio 0.932.
- hot/136 vs hot/135: -9.820s, 89 commits faster, mean ratio 0.962.
- cold/136 vs cold/121: +133.230s, all 100 commits slower, mean ratio 1.891.
- hot/136 vs hot/121: +149.224s, all 100 commits slower, mean ratio 2.514.

Conclusion: segment-local data-path dictionaries are safe and provide a real but small reduction in manifest bytes and startup time. They do not change the main architectural conclusion: startup replay of large serialized projection data dominates hot performance, and writeback still has periodic expensive publication spikes. The next backend should avoid replaying bulky manifests entirely, using compact mmap/read-mostly indexes plus immutable content objects or an equivalent custom pack layout.

## Benchmark update: run 137

Variant: checkpointed Git manifest with trace-capsule payloads split into
content-addressed external object files. The manifest stores trace/result
identity, payload hash, and payload path metadata. Startup validates that the
referenced payload objects exist, but does not read payload bytes until a trace
or result is actually decoded.

Results over the same 100-commit eval-trace-bench set:

| run | wall | hits/misses | init | writeback | Git bytes | referenced payload bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cold/121 | 432.676s | 599/101 | 5.752s | 7.352s | 0 | 0 |
| cold/136 | 565.905s | 599/101 | 99.928s | 28.583s | 1.599GB | 1.405GB |
| cold/137 | 458.063s | 599/101 | 9.163s | 7.718s | 0.193GB | 1.405GB |
| hot/121 | 98.631s | 700/0 | 7.224s | 0 | 0 | 0 |
| hot/136 | 247.855s | 700/0 | 148.329s | 0 | 0 | 0 |
| hot/137 | 103.487s | 700/0 | 6.100s | 0 | 0 | 0 |

Pairwise:

- cold/137 vs cold/136: -107.842s, all 100 commits faster, mean ratio 0.642.
- hot/137 vs hot/136: -144.368s, all 100 commits faster, mean ratio 0.418.
- cold/137 vs cold/121: +25.387s, all 100 commits slower, mean ratio 1.092.
- hot/137 vs hot/121: +4.856s, 94 commits slower, mean ratio 1.050.

Conclusion: external payload objects are a large and sound bridge improvement.
They directly validate the architectural diagnosis: manifest replay and embedded
payload bytes were dominating startup and writeback. They are still not the final
answer because run 137 does not beat pre-schema SQLite. The remaining gap should
be attacked by replacing Git manifest replay with a custom compact hot index:
fixed-width sorted keys, fanout tables, payload-pack offsets, event-log spans,
and atomic CURRENT publication. Git/libgit2 should be treated as prior art for
content-addressed packs and indexes, not as the final data plane, because Git
object lookup does not match the eval-trace hot query
`(session/recovery key, attr path) -> verified candidate`.

Additional research-agent conclusions:

- Direct Git objects are a poor fit: Git OIDs and delta-capable packs optimize
  `OID -> object`, while eval-trace needs projection indexes over sessions,
  recovery keys, attr paths, traces, and results.
- Further SQLite tuning is still useful for fallback and validation, but the
  current high-value path is not more SQL. Candidate SQLite work is limited to
  measured items such as dropping unused indexes, `PRAGMA optimize`, and
  candidate/head normalization.
- Boost.Serialization/cereal-style C++ object serialization is not appropriate
  for the durable format. The durable format needs canonical byte encoding,
  explicit schema epochs, exact float/string/path bytes, content hashes, and
  fail-closed verification.

## Rejected benchmark: payload-pack attempt, run 138

Variant: replace one-file-per-payload external objects with generation payload
packs and manifest `(pack id, offset, length)` references. This was intended to
reduce hot runtime filesystem overhead from opening many separate payload files.

Results:

| run | wall | hits/misses | init | writeback | Git bytes | referenced payload bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cold/137 | 458.063s | 599/101 | 9.163s | 7.718s | 0.193GB | 1.405GB |
| cold/138 | 458.447s | 599/101 | 9.370s | 7.519s | 0.193GB | 1.405GB |
| hot/137 | 103.487s | 700/0 | 6.100s | 0 | 0 | 0 |
| hot/138 | 103.194s | 700/0 | 6.080s | 0 | 0 | 0 |

Pairwise:

- cold/138 vs cold/137: +0.384s, 53 commits slower, mean ratio 1.004.
- hot/138 vs hot/137: -0.293s, 54 commits faster, mean ratio 0.997.
- cold/138 vs cold/121: +25.772s, 99 commits slower, mean ratio 1.095.
- hot/138 vs hot/121: +4.563s, 95 commits slower, mean ratio 1.047.

Conclusion: the naive payload-pack layer is not worth keeping. It adds format
complexity, loses the simplicity of content-addressed per-payload files, and
does not materially move either cold or hot wall time. The remaining gap is not
primarily directory-entry overhead for payload files. The next useful step is a
real hot index/custom generation format, not a different container for the same
Git manifest projection.

## 2026-05-23 cursor/projection benchmark pass

Baseline remains run 938 on the first 10 commits: cold 6.06s mean, hot 0.88s mean.

Experiments:

- run 1164: recursive JSON cursor with sibling batch verification. Outputs matched, but cold regressed to 6.84s and hot to 1.06s. Conclusion: per-node/cursor replay is the wrong JSON hot path.
- run 1165: v2 command-output certificate artifact, single atomic cert file with action-key, JSON payload, string-context payload, and content digests. Outputs matched, but cold was 6.65s and hot was 1.02s. Conclusion: artifact semantics are better, but exact-hit certification alone does not solve the performance target.
- run 1166: switched certificate payload/context digests to BLAKE3 and skipped context JSON parsing for empty contexts. Outputs matched, but cold was 6.71s and hot was 1.05s. Conclusion: parser/hash micro-optimization is below noise or worsens locality.
- run 1167: removed recursive cursor call sites so JSON misses no longer do duplicate demand construction. Outputs matched, but cold stayed 6.71s and hot 1.05s. Conclusion: the remaining gap is architectural, not cursor-call overhead.

Adversarial conclusion:

- A JSON output artifact is the correct semantic boundary, but exact-session action hits cannot beat the baseline by themselves.
- Cross-commit reuse needs an action certificate that validates the whole command-output observation proof in one batch, not recursive verification of every JSON node.
- If we keep exact artifact validation, its hot-path cost must be lower than the old v1 two-file action cache. The current v2 cert validates stronger invariants but is slower.
- Serving by stable recovery key alone would be unsound. The recovery key may route candidates, but proof validation must authorize bytes before serving.
- run 1168: split v2 certificate into small cert plus content-addressed JSON payload to avoid copying the payload out of a larger certificate buffer. Outputs matched; cold was 6.67s and hot was 1.03s. Conclusion: the single-file copy was not the main hot-path regression. Validating exact JSON artifacts remains slower than baseline v1-style exact action hits, and it still does not solve cold cross-commit misses.
