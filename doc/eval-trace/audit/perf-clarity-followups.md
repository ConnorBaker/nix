# Eval-trace performance / clarity followups

Items observed while reviewing the rearchitecture + Tagged devirt +
TraceStorage devirt work on branch
`vibe-coding/file-based-eval-cache`.  Originally 13 items; after a
validation-and-fix pass (commits `46e054b11` through the current
HEAD), most have been addressed or reframed.  This document is the
running tracker; the status column is the authoritative state.

Severity scale:
- **hot** — touches a path fired per-dep or per-attr during verify or
  record (21.58M per bench commit worst case).
- **warm** — per-trace / per-session overhead.
- **cold** — once per process.
- **clarity** — no measured perf impact; reading / auditing cost only.

---

## Status

| # | Item | Severity | Status |
|---|------|----------|--------|
| 1 | TracedDataNode devirt | hot | **Done** (`46e054b11`). 4 concrete types moved to `traced-data-nodes.hh`; `TracedDataNode::{objectKeys,objectGet,arraySize,arrayGet,materializeScalar,canonicalValue}` are switches on `tag_` in `traced-data-dispatch.cc`.  Zero vtables for `TracedDataNode`/`JsonDataNode`/`TomlDataNode`/`DirDataNode`/`DirScalarNode`.  `.text` −2.9 KB. |
| 2 | TracedExpr devirt | hot | **Done** (`ddb41bd78`). `RootTraceExpr`/`ChildTraceExpr` collapsed into a single `TracedExpr` with `Kind { Root, Child }` discriminator; 3 `dynamic_cast`s replaced with kind checks.  `RootTraceExpr::make` / `ChildTraceExpr::make` survive as empty-struct factory shells for call-site compat.  Only `Expr::eval` vtable remains (evaluator-level, unavoidable).  `.text` −5.0 KB. Byte-identical `nix eval` output confirmed on real nixpkgs. |
| 3 | SqliteTraceStorage friend fanout | clarity | **Reframed, retired**. The audit found only 3 real friends (`VerifyImpl`, `Verifier`, `Recorder`) — not 6.  `TraceStorageTestAccess` isn't a friend; it reaches the store via public API.  `OriginScope`/`OriginScopeFactory` friends are on tag types, not on the storage.  Not a cleanup opportunity. |
| 4 | debug() arg-eval audit | warm | **Reframed, retired**. The audit traced all 23 debug sites in verifier.cc — every expensive-arg site already has an outer `if (verbosity >= lvlDebug)` guard.  No missing guards; the CLAUDE.md guidance is being followed. |
| 5 | Trace cache map fusion | warm | **Done** (`7eedd2a36`). Fused `traceHeaderCache` + `traceFullCache` into one `traceCache` of `TraceCacheEntry{header, optional full}`.  `deferredTraceBlobs` left separate (phase-exclusive, never co-resident).  Hot `ensureTraceHeader` and `loadFullTrace` paths each do one map probe instead of two. |
| 6 | VerifyContext mutability docs | clarity | **Done** (current HEAD). Added a docblock on `struct VerifyContext` in `verifier.cc` naming which of the seven fields are mutable via the verify path and which are read-only.  Split into two types rejected: `store` is mutated via `patchTraceHashInMemory`, `state.store` mutates NarInfo disk cache — the RO/RW boundary isn't as clean as the original proposal suggested. |
| 7 | L1 depHash allocation on miss | warm | **Reframed, retired**. The audit traced the miss path: `readFile()` + BLAKE3 dominates (10–100 µs per miss); the string-copy the original proposal targeted is ~100 ns (<1%).  Not a motivated fix. |
| 8 | Tagged::Hash `is_avalanching` lie | correctness | **Done** (`7eedd2a36`). Removed the `is_avalanching = void` marker from `Tagged::Hash`.  libstdc++'s `std::hash<uint32_t>` is literally the identity function (verified empirically); claiming avalanching on identity told `boost::unordered_flat_map` to skip its own `hash_mix`, which clusters monotonic IDs catastrophically for bucket count `2^k` (IDs `[1..N]` all map to bucket 0).  `TransparentStringHash` (the other site the original doc pointed at) is genuinely avalanching — `std::hash<string_view>` on libstdc++ is a MurmurHash variant — so no change there. |
| 9 | `GCMap<K, V>` alias | correctness guard | **Retired**. The audit grep'd for every `unordered_flat_map` with a GC-allocated value type in eval-trace and found ZERO current violations.  The real GC-critical sites already use `traceable_allocator` correctly.  The alias would be purely prophylactic; its absence is not hurting anything. |
| 10 | Re-entrancy detector `thread_local` → pointer | warm (debug only) | **Done** (`7eedd2a36`). Collapsed the `thread_local boost::unordered_flat_map<TraceStorage *, uint32_t>` (~1000 probe+increments per bench run in debug builds) to `thread_local TraceStorageBase * heldStorage`.  One pointer store instead of a hash probe.  Debug-only path; added an explicit assert if the one-storage-per-thread invariant is ever violated. |
| 11 | `TraceStorageLike` concept policy | clarity | **Done** (current HEAD).  Added a docblock on the concept declaration spelling out: template over `TraceStorageLike` ONLY when a call site genuinely needs backend genericity (currently only a few test paths); production callsites hold `SqliteTraceStorage &` directly. |
| 12 | `ResultPayload` string ownership | warm | **Retired**. The audit found the hot path already uses `loadResultPayloadCached` which returns `const ResultPayload &` — zero copy.  The by-value `loadResultPayload(ea, id)` entry point exists only to satisfy the `TraceStorageLike` concept and has zero production consumers. |
| 13 | Tidy baseline sweep | clarity | **Done** (`b5cf7b900` + `901e280f4`).  All 61 unique `cppcoreguidelines-pro-type-member-init` sites across libstore/libutil and the stragglers in libexpr public headers addressed.  Latent hazards (`Pool::max`, `BuildResult::status`, `ServeProto::remoteVersion`, `UserLock::uid/gid`, `PathInfo::downloadSize/narSize`, `Worker::goal2`, `RetryDelayParams::attempt/baseMs/ceilMs`, `SQLiteSettings::useWAL`, `ClientSettings` 9 fields) fixed with explicit default values.  Buffer-target cases (`struct stat`, `struct rlimit`, etc.) get `{}`.  Large read buffers (8KB–64KB) use NOLINTNEXTLINE where zero-init would be wasted work.  Final tidy run across src/libstore/ + src/libutil/ shows zero remaining warnings. |

## Post-audit summary

All 13 items closed.  Summary:

- **Real wins landed**: 1, 2, 5, 8, 10 (TracedDataNode/TracedExpr
  devirt, trace-cache fusion, Tagged hash correctness, re-entrancy
  detector collapse).  Aggregate binary savings: ~12 KB `.text` plus
  bss reduction.  Byte-identical `nix eval` output on real nixpkgs
  confirmed post-devirt.

- **Clarity notes landed**: 6 (VerifyContext mutability docblock),
  11 (TraceStorageLike policy), 13 (mechanical baseline sweep —
  61 sites + stragglers; tidy now returns zero warnings for this
  check across libstore + libutil + libexpr).

- **Reframed / already-addressed**: 4 (debug guards OK), 7 (readFile
  dominates miss), 9 (no violations), 12 (already uses zero-copy path).
  Not worth PR.

- **Retired entirely**: 3 (friend count was wrong — only 3 real
  friends, already tight).

## Context-window triage (not in scope for this list)

Items observed but explicitly out of scope for perf/clarity:

- **OR-1 / OR-3 / OR-4 / OR-7 / OR-11** — open research items
  tracked in `src/libexpr/eval-trace/CLAUDE.md`.  These are
  correctness gaps or latent hazards, not perf/clarity.
- **Structural-variant recovery early-exit** — already has its own
  telemetry (`--enable-structural-variant-mismatch-telemetry-for`)
  and CLAUDE.md documentation.  A bench-driven optimization, not a
  reviewable code smell.
- **Fiber scheduler / `syncAwait` barrier** — analyzed via
  decompilation earlier in the branch's history; the barrier is
  structural and not amenable to a point fix.
