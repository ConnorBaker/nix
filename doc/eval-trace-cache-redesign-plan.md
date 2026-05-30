# Eval-trace cache redesign plan

> **This is the living plan and the authoritative forward doc for the current
> tree.** Start at `eval-trace-cache-README.md` for the consolidated index and
> benchmark-ledger reconciliation. Within this file, the two closing
> 2026-05-29 sections (*deep-research synthesis* and *CORRECTION after thorough
> work-log read*) are the current canonical direction; the CORRECTION
> supersedes earlier sections where they conflict. Benchmark anchors here are
> Ledger B (storage lineage): 100-commit `closures` run **totals** and means,
> not the findings-doc per-commit means. No perf code has changed since
> baseline `92a3df1ab`; every anchor predates HEAD.

This is the living plan for the eval-trace cache redesign. Update it when
benchmarks, implementation work, or adversarial review changes the direction.

## 2026-05-30 CURRENT-TREE baseline (HEAD `9f7311129`) — Ledger D

The first benchmark of the actual current tree (every earlier anchor predates
HEAD; see README gate item 6). This is its own ledger — **do not compare its
numbers against Ledger A/B/C**. It is the fixed reference point future lever
work measures against.

- **Harness:** `eval-trace-bench generate --nix . --nixpkgs
  ~/ext-sources/nixpkgs --num-commits 100 --run-number 1`, `NIX_CONFIG="builders
  ="`, no debug, no `NIX_SHOW_STATS`. Binary under test:
  `nix-2.35.0pre20260530_9f73111`.
- **Workload:** `nixos/release.nix closures.gnome`, 100-commit nixpkgs window
  ending `8f443eb47dcb`. (NOTE: `closures.gnome`, not full `closures` — narrower
  than some Ledger-B runs; do not compare totals.)
- **Soundness: PASS on all 100 commits** (cold and hot byte-identical to
  `--no-eval-trace`).

| Mode | Wall mean | Wall median | vs reference (mean) | hits/misses |
|---|---:|---:|---:|---|
| reference (no trace) | 6.47 s | ~6.45 s | 1.00× | n/a |
| cold/1 (record) | 3.72 s | 1.11 s | 0.58× | (incremental) |
| hot/1 (replay) | 0.96 s | ~0.95 s | 0.15× | 100/0 |

Distribution (the load-bearing observation): cold is **bimodal** — median 1.11 s,
mean 3.72 s. The gap is ~23 catastrophic re-record commits at 7–17.6 s
(worst `9b9f7241` 17.58 s, +1481% over median); the other ~77 commits sit at
~1.0 s. **Hot is flat ~0.95 s, zero variance, zero misses.**

Lever implications, now quantified against HEAD:
- **Lever 1 (observed-key pruning)** is a tail-rescue and the baseline proves
  it: the cold median is already ~1.1 s; the entire cold-mean prize is in
  collapsing the ~23 outliers toward the median. Confirms the work-log's
  "tail-rescue, not distribution shift" framing on the current tree.
- **Lever 3 (certificate-before-payload)** targets hot, but hot is already a
  flat ~0.95 s — confirming the residual hot cost is decode/startup, not the
  dep walk, and that this lever is low-priority (matches the 2026-05-29
  CORRECTION finding 1).

Caveats: single run (wall noise; use `pairwise` paired medians for A/B later);
stats columns intentionally blank (ranking run); the reference outlier on the
first commit (`8f443eb` 10.3 s vs 6.4 s median) is checkout/cold-disk warmup,
not signal. Run data on disk under `eval-trace-bench-results/{reference,cold/1,
hot/1}/` (gitignored, ~2.5 GB; manifests carry full provenance).

## 2026-05-30 Step-1 diagnostic — what the cold outliers actually are

Follow-up to the Ledger-D baseline: a `--with-stats` run (`cold-stats/2`,
written to a separate namespace so it can't contaminate clean wall runs) +
`classify`. This is the first phase-level decomposition of the current tree's
cold tail, and it **redirects the lever choice** away from the prior lineage's
assumption.

Findings:
- The 23 cold outliers are `partial-miss` (15 commits, 11.75 s, 47.6% hit,
  **55% recovery failure**) and `full-miss` (7 commits, 12.14 s, 36.7% hit,
  **100% recovery failure**). The 78 fast commits run ~1.0–1.8 s.
- **`nrThunks`: ~20.9M on outliers vs ~268K on fast commits (78×).** The cost is
  the **re-evaluation itself** (CPU 13–14 s), not trace machinery — recovery
  329–615 ms, structural-variant 170–380 ms, verify ~580–844 ms are all
  milliseconds against seconds of miss cost.
- Hit-path: **primary cache 0%**; DirectHash recovery 98.4%, StructVariant
  37.7%, history bootstrap 67.2%. Everything is served via recovery; outliers
  are recovery *failures* cascading to full re-eval.
- **The outliers are NOT by-name / enumerated-set churn.** Worst outlier
  `9b9f7241` (17.6 s) changes only `nixos/.../autossh-ng.nix` (one NixOS module,
  zero packages) yet re-evals ~20M thunks. The workload is `closures.gnome` = 2
  NixOS *system closures*; a change anywhere reachable by the closure defeats
  recovery for the whole closure.

Redirect: the outlier lever is **closure-localization of a deep change**, NOT
lever-1 enumerated-set pruning. Two hypotheses, now RESOLVED (below).

### Step-1 diagnostic — RESOLVED (2026-05-30, stats.json drill-down on `9b9f7241`)

H1 (over-coarse top-level dep) **rejected**; H2 (recovery can't localize)
**confirmed and sharpened into a new lever**.

The mechanism (outlier stats.json): the eval records **16,352 trace scopes** but
verifies only **7 at the top level** — the monolithic `closures.gnome` roots,
~1,278 thunks each. **`verify.failed=222` of `233481` deps (0.10%)**, spread
(not one coarse listing → H1 dead; `ownDepsMax=48738` = fine-grained per-trace
deps), fail **4 of 7 roots**. Recovery (DirectHash/gitIdentity/structVariant)
fails on all 4 → each re-evals its **entire** subtree. Machinery is cheap
(recovery 0.75 s + verify 0.99 s); **~13 s is raw re-eval** (cpuTime 14.6 s).
Fast commit `f37d` for contrast: 7/7 roots verify clean, **nrThunks=1**,
verify.failed=0. Binary per root.

**Root cause (CORRECTED after checking `record.count`): the cache records only
~7 COARSE traces for the whole eval — there is no sub-trace granularity to
reuse.** Full-cold first commit: `record.count=7` vs `depTracker.scopes=16357`.
The 16K scopes are transient `DepCaptureScope` frames that collapse into the 7
recorded traces, NOT addressable sub-traces. A 0.10%-of-deps change fails one of
only 7 monolithic root traces and there is nothing finer recorded to fall back
to. (Earlier wording "16K recorded sub-traces unreachable" was wrong — verified
via stats.json `record.count`.)

**New lever (Lever 5 — finer recording + incremental sub-trace reuse):** record
addressable traces at intermediate nodes (not just 7 roots) AND, on root verify
miss, re-enter per-child cache lookups so unchanged sub-traces warm-hit. It is a
RECORDING-granularity change, not merely a warm-path reuse change — both sides
are needed (reuse finds nothing without finer recording; finer recording is
bypassed without warm re-entry). Distinct from lever 1 (enumerated-set) and
lever 2 (derivation-boundary), though §1 of the lever doc ties the granularity
choice to lever 2's derivation-boundary idea (record at drv/module boundaries,
not every attr, to avoid the v53 "store too much" failure). It is the **measured
outlier lever for `closures.gnome`** — the 23 outliers are the commits where a
root verify fails.

**Code-grounded refinement (trace-session.cc).** The gap is now located:
- Warm hit → `materializeResult` (cheap). Verify miss →
  `evaluateFresh` (trace-session.cc:667-669).
- A per-leaf lazy-verification contract DOES exist (CLAUDE.md OR-3,
  `ParentChild_PerLeafLazyVerification`): forcing a *child through the cache*
  verifies that child's own trace. BUT it only helps when children are reached
  *via the cache*. When a **root** trace fails verify, `evaluateFresh` walks
  `navigateToReal`'s `realRoot` = **fresh thunks** (trace-session.cc:654-655),
  which **bypass the cache for the entire subtree** — the 16K recorded
  sub-traces are never consulted. The benchmark's `--json` deep-forces the whole
  structure, so a failed root re-evals its complete subtree fresh.
- So Lever 5 ≈ "**record finer (intermediate `TracedExpr` nodes, not just 7
  roots) AND on verify miss re-enter the cache per-child instead of fresh-walking
  the realRoot**". `installChildThunk`/`makeChild` is called only from
  `materialize.cc` today, so children are cache-routed ONLY on warm hits; both
  the cold record pass and the warm-miss fresh pass produce plain Nix thunks that
  bypass the cache. Lever 5 = apply the child-wrap in both passes.

**SUPERSEDED 2026-05-30 by a deeper code+DB+profiler study — the outlier lever
is LEVER 2 (derivation-boundary), not Lever 5.** The "wrap attrset children"
framing was mislocated. Decisive evidence:
- `closures.gnome.<system>` is a **string** (a store path), not an attrset.
  Producing it forces the whole NixOS system derivation (~20 M thunks) inside a
  single `TracedExpr` leaf. There are NO intermediate attrset sub-traces to
  reuse — the closure is data-shallow but computation-deep. `installChildThunk`
  already wraps the ~55 attrset children that exist; the cost isn't there.
- DB: `Traces=93` total across 100 commits, each `values_blob` 100 KiB–1 MiB
  (~233 K flat deps). The 16 K `depTracker.scopes` are dep-keys, not traces.
- The only reuse boundary INSIDE the computation is the **derivation**.
  `closures.gnome.x86_64-linux` = **6,419 derivations**; between an outlier and
  its predecessor **6,397 are identical, 22 changed (99.66 % reuse opportunity)**.
- Flamegraph (leaf-frame): **~58 % of eval cost is `make-derivation`/pkgs**
  (cacheable at the derivation boundary), ~25 % is the `lib/modules.nix`
  fixpoint (the floor that re-runs anyway). Realistic ceiling: ~halve the
  outlier re-eval, not eliminate it.

So Lever 5 (attrset sub-trace reuse) is real but targets a DIFFERENT workload
(deep-attrset enumeration, e.g. `nix-eval-jobs` over `pkgs`), NOT
`closures.gnome`. The `closures.gnome` outlier fix is derivation-boundary
caching. Full sketch + slices + open questions:
**`plans/lever2-derivation-boundary-caching.md`**. lever1 doc §"Design-question 1
RESOLVED — THE REFRAME" has the reasoning.

**O1 (keying) RESOLVED (2026-05-30, lever2 doc §2b):** not a schema change —
`Traces` are already content-addressed/attr-path-independent; only Sessions/
History routing is attr-path-keyed (Design A reuses it).

**ADVERSARIAL PASS (2026-05-30, lever2 doc §7-9) RECLASSIFIES Lever 2 from
"large but localized" to "high-blast-radius core-evaluator work."** Key findings:
- (GOOD) Dep attribution is ALREADY per-thunk: `forceThunkValue` (eval.cc:1683-94)
  snapshots `epochStart` then `recordThunkDeps(v,epochStart)` captures each
  thunk's own dep range. A derivation's input deps already exist as a clean epoch
  range — no new dep-tracking needed.
- (CORRECTION) The hook is NOT `derivationStrict` (its inputs are already forced
  by `forceAttrs` at primops.cc:1552 before it runs); it is the thunk-force
  boundary.
- (DECISIVE) The speedup needs verify-BEFORE-force, and the ONLY pre-force hook
  is `TracedExpr::eval` — which works only if the derivation's thunk IS a
  `TracedExpr`. Those are installed only by `materialize.cc` for attrset children
  of a cached result; a derivation deep in a string computation is not such a
  child. So Slice B' (the speedup) requires installing `TracedExpr` wrappers at
  derivation thunk CREATION in the core evaluator — high-blast-radius, touching
  the hot path used by ALL eval (cf. the rearchitecture vptr-in-hot-loop reversal).
- Recording alone (Slice A') is cheap but INERT (passive-metadata trap the
  work-log already hit). 

Revised recommendation: do NOT prototype Lever 2 directly. The cheap decisive
experiment is a throwaway spike installing `TracedExpr` at derivation thunks,
counting how many become cache-verifiable AND measuring fast-commit
non-regression — before any real wiring. If it perturbs the hot path (likely),
leave the cold tail as-is (23 outliers, soundness-correct, bounded). Facet mask
(§3) + storage budget (6,419 derivs/commit) remain on top of all this.

**SPIKE RAN (2026-05-30, branch `spike/lever2-derivation-feasibility`) — MATERIAL
BLOCKER (lever2 doc §11).** Step 1 measured: a traced `nix eval
closures.gnome.x86_64-linux` evaluates **10,455 derivations** but records only
**6 traces** (counter added at `prim_derivationStrict`, `evalTrace.spike`). Step 2
blocked: to verify-before-force a derivation it must be a `TracedExpr`, but
`TracedExpr` identity is attr-path-tree-shaped (`makeChild` needs a parent
`TracedExpr` + `Symbol name` + `extendPath`; `navigateToReal` walks the parent
chain through named selectors). A `derivationStrict` thunk created deep in
`make-derivation.nix` (via app/`let`/`map`/fixpoint) has NO attr-path from the
root and NO parent `TracedExpr` — `makeChild` cannot construct it. **Caching
derivations requires a SECOND content-addressed `TracedExpr` identity model
(derivation-input-hash keyed, parentless, `navigateToReal` → re-invoke
`derivationStrict`) threaded through evaluator thunk creation — a foundational
redesign on the hot eval path, RFC-scale, not a lever.** This concretely explains
the CLAUDE.md "derivationStrict outside scope" note: the cache's addressing model
structurally excludes non-attr-path values. **Verdict: Lever 2 not pursued; cold
tail left as a bounded, soundness-correct cost. Spike code is throwaway
(measurement-only counters).**

## 2026-05-30 deep-attrset workload finding — cache is net-negative on nix-eval-jobs shape

Ran the deep-attrset workload (python3Packages outPaths, ~11K packages, the
nix-eval-jobs shape) the GNOME diagnosis implied we should test.

NOTE: the first run used an -O0 debug meson build and commits whose delta didn't
touch python3Packages; both invalidated it (see retraction). The numbers below
are the CORRECTED RELEASE-binary re-run (result/bin/nix, the Ledger-D binary),
with a real python3Packages.streamz bump for the incremental test, hit/miss +
soundness captured. n=1.

| Scenario | Wall | hits/misses | sound |
|---|---:|---|---|
| reference (--no-eval-trace) | 0:18 | - | - |
| cold (trace, empty) | 2:34 | 0/11062 | - |
| hot (same commit) | 0:14 | 11062/0 | served==truth |
| incremental (parent cold -> streamz bump) | 0:17 | 11062/1 | served==truth |
| incremental reference | 0:18 | - | - |

Findings (sound): cold ~8.6x slower (not 12x); the cache is SOUND and PRECISE
(a 1-package bump invalidates exactly 1 trace, reuses 11062, served byte-identical
to ground truth); hot AND incremental are ~= break-even with no-trace (14-17s vs
18s). With ~100% hits the verification overhead roughly equals the eval cost it
avoids, so no net win -- but NOT net-negative and NOT a disaster. The retracted
"12x / net-negative / disable it" was an -O0 + no-op-delta artifact.

Opposite of closures.gnome only in cold magnitude: a package set is
attr-path-addressable so materialize wraps each child as a TracedExpr -> ~11K
traces recorded (vs 6 for GNOME). Cold wall >> cold CPU -> recording is I/O-bound
(SQLite + per-package dep-blob serialize).

Implications: (1) the deep-attrset case does not want Lever 1 (finer pruning) -
it already has maximal granularity, which is what makes cold expensive; it
wants cheaper/fewer per-trace recording AND amortization. (2) The decisive
experiment WAS the real incremental test above (done); the prior same-commit hot
break-even doesn't capture the real nix-eval-jobs value case. (3) The cache's value
is workload-shape-dependent and currently inverted for the most important consumer.
See eval-trace-cache-README.md "DEEP-ATTRSET WORKLOAD FINDING" for the full table +
option list (2', 2'').

## Current benchmark anchors

| Run | Backend shape | Cold wall | Hot wall | Notes |
|---:|---|---:|---:|---|
| 121 | pre-schema SQLite | 432.7s | 98.6s | Baseline to beat, not just match. |
| 132 | normalized SQLite | 575.9s | 94.4s | Cold writeback regressed badly. |
| 136 | checkpointed Git manifest | 565.9s | 247.9s | Startup replay dominated. |
| 137 | external payload objects | 458.1s | 103.5s | Startup/writeback mostly recovered; hot gap remains. |
| 156 | identity/session hardening snapshot | invalid | invalid | Overlapped a local build and result-symlink churn; useful only for counters/symptoms. |
| 157 | current green build before latest tests/rollback guard | pending | pending | Clean run in progress; do not compare against patched source until rebuild/run 158. |

Interpretation: storage lookup/startup/writeback is no longer the only likely
hot bottleneck. The next measurements must split lookup, trace payload decode,
dependency verification, recovery, and result materialization.

## Latest measured finding

### Identity/session hardening

Recent adversarial review found three soundness hazards around process-local
identity:

| Area | Status | Rule |
|---|---|---|
| Scalar string/path identity | fixed + direct tests added | `IdentityObject` equality is not enough; the owning weak scope token must still be live. |
| Attr replay ranges | fixed + regression test added | Stamped attr replay is used only for existing live stamped identities; otherwise weak replay fallback records the range. |
| Root materialization publication | fixed | `publishRootMaterializedValueIdentity` now rolls back partial publication internally if allocation or map insertion throws before returning a snapshot. |

Remaining hardening:

- Replace or formally guard the raw `TraceSession *` exact-proof resolver in
  `sameValueIdentity`.
- Make the single-threaded `ReadOptimizedMap` assumption explicit at runtime or
  redesign synchronization before supporting shared `EvalState` parallelism.
- Add fault-injection coverage for publication rollback if/when test-only
  allocation hooks are available.

Additional adversarial findings now addressed:

| Area | Status | Rule |
|---|---|---|
| Direct function equality | fixed + regressions added | Eval-trace identity may authorize aliased containers, but must never override Nix function incomparability for direct function leaves. |
| Backend release throwing | fixed | Identity scope tokens are invalidated and the backend is detached before flushing, so a throwing writeback cannot leave retained identities live. |

### Result-only capsule decode

Status: implemented, awaiting rebuild/benchmark.

`decodeCachedResult` now decodes only the capsule identity header and
`EncodedResultPayload`, then stops before dependency rows. Full dependency
decode remains the verifier/cold-path representation.

Soundness position:

- The caller must have already accepted the trace through verification or an
  exact proof summary.
- The raw capsule payload hash and header hashes are still checked before the
  result payload is used.
- This projection is not a proof authority and must not be used to accept a
  trace by itself.

Expected effect:

- Hot result materialization avoids dependency-vector allocation, dep-row
  decoding, and decode-time sort fallback.
- It still pays full zstd inflate until the envelope grows a result/proof
  sidecar; that is the next larger change.

### Replay no-active-access early skip, rejected

Run: `eval-trace-bench-results/manual-replay-no-access-skip`.

An early return in `replayMemoizedDeps` for calls with neither sibling replay
capture nor active trace access did not fire on the `closures` cold or hot
workloads. The added counter reported `noActiveAccess = 0`.

Comparison against `manual-attr-replay-counters`:

| Metric | Counter-only baseline | Early-skip trial |
|---|---:|---:|
| Cold traced wall | `1:31.79` | `1:32.23` |
| Hot traced wall | `0:01.67` | `0:01.68` |
| Cold replay deps added | `426205` | `426205` |

Decision: remove the optimization and counter. It preserved semantics but added
dead complexity and did not address the measured bottleneck.

### Inline replay gate, rejected

Run: `eval-trace-bench-results/manual-inline-replay-gate`.

The `forceValue` replay gate was moved into `eval-inline.hh` to avoid an
out-of-line call on the universal non-thunk path. The implementation preserved
the same logical gate: attrsets used stamped replay ranges, non-attrs used the
pointer Bloom filter only as a negative accelerator, and Bloom hits still
checked the exact weak range map.

Measured result:

| Metric | Counter-only baseline | Inline-gate trial |
|---|---:|---:|
| Cold traced wall | `1:31.79` | `1:32.16` |
| Hot traced wall | `0:01.67` | `0:01.65` |
| Cold replay deps added | `426205` | `425620` |
| Eval output bytes | same | same |

Decision: reject. It does not improve cold wall time and perturbs replay-add
accounting enough that the extra proof burden is not justified.

### Candidate: skip decode-time sort for canonical capsules

The hot path decodes `~53.5 MiB` of trace payloads and checks `842775` deps for
the `closures` hot run. Decode currently sorted deps after reading the capsule
even though recording already writes `sortAndDedupDeps` output. The implemented
trial treats the capsule as an immutable sorted run: check monotonicity while
decoding, and sort only if a legacy/unsorted payload is encountered.

Soundness position:

- Fast path does not accept new data; it only preserves canonical order already
  present in the payload.
- Fallback sort preserves old behavior for unsorted payloads.
- Existing payload hash/header checks still protect byte integrity.

Benchmark pending under `eval-trace-bench-results/manual-capsule-sorted-run`.

Measured result for `manual-capsule-sorted-run`:

| Metric | Counter-only baseline | Sorted-run trial |
|---|---:|---:|
| Cold traced wall | `1:31.79` | `1:30.89` |
| Hot traced wall | `0:01.67` | `0:01.48` |
| Hot full decode | `407209us` | `320895us` |
| Hot verify total | `889495us` | `805875us` |
| Eval output bytes | same | same |

Decision: keep. This is a pure decode-path optimization with fallback to the
old sort behavior for unsorted payloads.

### Candidate: full-trace verification memo

Hot verification still walks `842775` deps even though only `38346` current dep
hashes miss the session L1 cache. A conservative per-session memo now records
`FullTraceHash` values that verified without requiring a trace-local hash patch.
Later TraceIds with the same stored full trace can be accepted after marking
that TraceId verified, without walking the same dependency vector again.

Soundness position:

- `FullTraceHash` covers all stored dep keys and values.
- Reuse is session-local, so it is tied to the same current filesystem/source
  state and L1 dep-hash invariant.
- `ValidViaImplicitShapeOverride` is deliberately excluded because it patches
  the current trace hash for one TraceId; skipping another TraceId would erase
  that side effect and could make TraceContext consumers observe a stale hash.

Benchmark pending under `eval-trace-bench-results/manual-fullhash-verify-memo`.

## Architectural direction

Use two explicit layers:

| Layer | Responsibility |
|---|---|
| Semantic proof layer | Decide whether a cached result is sound and precise enough to reuse. |
| Binary storage layer | Make startup, lookup, and writeback cheap once the semantic key is valid. |

The desired end state is a custom immutable generation store with an AC/CAS
split:

| Component | Role |
|---|---|
| `TraceActionKey` | Evaluator identity, query key, requested precision, schema epoch. |
| `SessionProofDescriptor` | Typed immutable proof of evaluator semantics, source universe, store config, and ambient inputs. |
| `ObservationCoverage` | Exact typed positive and negative observations required by the result. |
| Action-cache entry | `(TraceActionKey, SessionProofDescriptor, Precision) -> payload digest + coverage digest`. |
| CAS object | Trace/result capsule, proof descriptor, coverage object, string/path tables. |
| Generation manifest | Immutable published set of indexes/packs. |

Do not use direct Git/libgit2 packs, RocksDB, LevelDB, or Boost.Serialization as
the primary backend. Borrow proven patterns from them: content-addressed packs,
fanout/sorted indexes, immutable generations, manifests, checksums, and atomic
publication.

Latest backend research strengthens this decision:

| Option | Decision |
|---|---|
| Custom sealed append-only pack + mmap index | Primary path. Removes SQL planning, B-tree row work, row materialization, and full trace decode from hits. |
| Hot fixed-layout proof/result summaries | Primary hot-path codec. Keep uncompressed and directly seekable; full trace stays cold. |
| SQLite | Baseline/control plane only unless benchmarks prove otherwise. Even recent SQLite cannot remove schema/record/B-tree overhead. |
| MDBX/LMDB | Prototype as an index/control-plane alternative, with large payloads still in packs. |
| libgit2 | Design inspiration only. Git ODB semantics, zlib/delta behavior, and object model do not match eval-trace hot hits. |
| RocksDB/LSM | Avoid unless workload becomes high-churn random writes; compaction/tail latency is the wrong tradeoff. |

Primary references:

- Bazel AC/CAS split: https://bazel.build/remote/caching
- Git pack/index fanout and checksums: https://git-scm.com/docs/gitformat-pack
- LevelDB manifest/log/SSTable pattern: https://github.com/google/leveldb/blob/main/doc/impl.md
- SQLite WAL one-writer/snapshot pattern: https://www.sqlite.org/wal.html
- POSIX atomic rename: https://man7.org/linux/man-pages/man2/rename.2.html
- POSIX file and directory fsync: https://man7.org/linux/man-pages/man2/fsync.2.html
- CBOR deterministic encoding rules: https://www.rfc-editor.org/rfc/rfc8949
- Cap'n Proto canonicalization/evolution reference: https://capnproto.org/encoding.html

## Semantic invariants

| Invariant | Rule |
|---|---|
| No erased session identity | Store typed descriptors, not only digests. |
| Hashes are accelerators | Digest lookup must be followed by exact canonical-key confirmation. |
| Unknown means miss | Unknown required feature, proof kind, or observation type cannot be accepted. |
| Negative observations count | Missing attrs, files, env vars, and lookup failures are semantic facts. |
| Coverage is exact | Bloom/prefix filters may reject work; they never prove a hit. |
| Recovery stays verified | Recovery/partial hits must replay verification. |
| Exact-proof bypass is gated | Verification can be skipped only for closed-world proof + exact coverage entailment. |
| Payloads are immutable | CAS digest identifies bytes; mutation is impossible by construction. |
| Manifest is authoritative | Startup must be bounded by live generations, not historical log replay. |
| GC is generation-safe | Delete only files unreachable from retained manifests after a grace period. |

## Attribute-set observation analogue

Attribute sets should be traced like filesystems:

| Filesystem observation | Attribute-set analogue |
|---|---|
| `pathExists` | `AttrPresence(set, name, present)` |
| `readDir` | `AttrSetShape(set, sortedNames)` |
| `readFile` | `AttrSelection(set, name, valueFingerprint)` |
| source position lookup | `AttrSourcePos(set, name, pos)` |

This keeps precision: unrelated attr additions should not stale a cached attr
selection, but adding a previously missing selected attr must stale it.

## Target storage layout

```text
eval-trace-store-v1/
  FORMAT
  LOCK
  CURRENT
  CURRENT.prev
  manifests/
    MANIFEST-000000000137.etm
  generations/
    g000000000137/
      GEN-MANIFEST.etm
      root.eti
      segments/
        s00000042/
          events.etl
          payload-0000.etp
          index.eti
  incoming/
  trash/
```

| File | Role |
|---|---|
| `.etm` | Manifest with generation, parent, segment list, file hashes, and feature bits. |
| `.eti` | mmap hot index: fanout, sorted digests, refs, exact-key metadata. |
| `.etp` | append-only payload pack. |
| `.etl` | append-only event log for rebuilding segment indexes. |
| `CURRENT` | Small verified pointer to latest manifest, atomically replaced. |

Publication protocol:

1. Write temp pack/log/index/manifest in `incoming`.
2. `fsync` each file.
3. `fsync` the staging directory.
4. Acquire the short writer lock.
5. Re-read `CURRENT`; merge/rebase if another writer won.
6. Rename immutable generation files into place.
7. Write and `fsync` `CURRENT.tmp`.
8. Rename `CURRENT.tmp` over `CURRENT`.
9. `fsync` the store directory.
10. Release the lock.

Readers take no locks. They read `CURRENT`, verify the manifest, mmap indexes,
and treat corruption or unsupported features as a miss.

## Modern C++ implementation rules

| Area | Rule |
|---|---|
| Errors | Use value-or-error results; distinguish normal misses from corruption/internal invariants. |
| IDs | Use strongly typed IDs for sessions, observations, attrsets, strings, and blob offsets. |
| Proof states | Use distinct recorded/verified proof types so unverified proofs cannot authorize hits. |
| Observations | Use a closed typed taxonomy; new semantic observation kinds require explicit handling. |
| Wire format | Never serialize native structs. Use explicit codecs, stable tags, fixed endian, definite lengths. |
| mmap views | An index view owns its mapping; spans/string_views never outlive the owner. |
| Publication | RAII cleanup, explicit `commit()` because destructors cannot report fsync/rename errors. |
| Locks | Lock-free readers, ranked writer locks, no evaluator calls while cache locks are held. |

## Work plan

### 1. Phase-split instrumentation

Status: in progress.

Add counters for:

- current-node lookup time
- history lookup time
- full-trace load time
- volatile-dep scan time
- `verifyTrace` call time
- recovery call time
- result decode/materialization time
- trace payload bytes/read/decode time
- result capsule cache hit/miss/read/decode/materialization time

Goal: make the next `NIX_SHOW_STATS=1` benchmark identify whether the hot gap is
verification/hash work, payload decode/allocation, mmap/file IO, or lookup.

### 2. Benchmarks after instrumentation

Status: pending.

Run the same `closures` and `closures.gnome` scenarios and compare against runs
121, 137, and the newest run. Track wall time and the new phase counters.

### 3. Semantic proof model

Status: pending.

Implement tests first for:

- exact proof hit remains valid under unrelated attr changes
- exact proof hit misses when selected attr/file/env/store observation changes
- open-world/volatile/uncovered observations cannot skip verification
- recovery/history hits still verify
- unknown observation/proof feature means miss

### 4. Exact-proof fast path

Status: pending.

Only after the typed proof/coverage model exists, add a fast path that skips
dependency verification for closed-world exact-proof hits. Gate with counters
and fail closed on conflicts.

### 5. Custom generation backend

Status: pending.

Start as an opt-in backend using the existing capsule bytes. Keep the first
slice small:

- write one immutable generation
- mmap one root index
- exact-key lookup with digest fanout + canonical-key confirmation
- payload pack refs
- atomic `CURRENT` publication

### 6. Adversarial review gates

Status: pending.

Before each larger change, review:

- soundness and precision
- crash consistency
- concurrent writers/readers
- GC safety
- mmap lifetime safety
- benchmark impact
- whether a simpler SQLite-side improvement is enough

## Immediate next data needed

The next benchmark must answer:

| Question | Counter family |
|---|---|
| Is hot eval dominated by dependency verification? | `evalTrace.verify.phases.verifyTraceCallUs`, `evalTrace.depHash.*` |
| Is result materialization material? | `evalTrace.resultDecode.*` |
| Are trace capsules being decoded repeatedly? | `evalTrace.loadTrace.*CacheHits`, `*CacheMisses`, `*DecodeUs` |
| Is payload IO still material? | payload read time and bytes counters |
| Is history bootstrap/recovery distorting hot runs? | history lookup/recovery phase counters |


## 2026-05-15 instrumented `closures` benchmark

Command shape:

```sh
NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH=... ./result/bin/nix eval \
  -f /home/connorbaker/nixpkgs/nixos/release.nix --json closures
```

Results from `eval-trace-bench-results/manual-instrumented` after a local `nix build -L . --builders ''`:

| mode | wall | user | sys | RSS | hit/miss |
| --- | ---: | ---: | ---: | ---: | ---: |
| no eval trace | 43.88s | 45.50s | 5.11s | 2.40 GiB | n/a |
| cold traced | 98.19s | 112.53s | 11.21s | 7.03 GiB | 0 / 42 |
| hot traced | 1.60s | 0.77s | 0.80s | 374 MiB | 42 / 0 |

Hot `closures` is not the immediate bottleneck: 42/42 hits produce a 1.60s wall run. The hot path still spends about 0.90s in verification, split roughly between full-trace load/decode (0.45s) and dependency verification (0.43s), so exact-proof fast paths still matter for smaller or latency-sensitive queries.

Cold traced is the current bottleneck. Writeback is only 1.14s, manifest encode is 1.09s, record hash is 0.80s, and dependency path resolution is 1.16s. That leaves most of the 54s cold-over-no-trace wall delta outside the current counters. Next evidence-gathering step: perf cold traced vs no-trace and add targeted counters around the dominant trace scaffolding/data-file/thunk paths.

## Parallel research synthesis: replacement store direction

Subagent research converged on the same core architecture: replace the SQLite/Git hybrid with an immutable generation-pack store, split into an action-cache index and a content-addressed payload store. This follows the same broad shape as Bazel's AC/CAS model, Git pack/index files, LevelDB/RocksDB immutable table + manifest publication, and SQLite WAL-style recovery rules, but keeps the semantics Nix-specific instead of inheriting a mismatched storage engine.

The important semantic refinement is that "session identity" must not be erased into a mutable Sessions row or loose hash. A cache hit should be keyed by a typed immutable proof descriptor: evaluator identity, language/trace ABI, request identity, evaluator mode, store/root/environment context, required observation domains, and precision descriptor. The payload can then be immutable CAS data addressed by digest. Exact proof-descriptor equality can justify a fast hit; coverage entailment can later allow safe partial reuse, but only if the observation coverage relation is exact and typed.

Storage proposal:

1. `TraceActionKey`: typed request/result identity, schema version, evaluator identity, requested precision.
2. `SessionProofDescriptor`: immutable typed descriptor for source roots, store context, evaluator/config flags, environment/impurity mode, and observation domains.
3. `ObservationCoverage`: exact typed positive and negative observations; probabilistic filters may only skip lookups, never accept hits.
4. `AC entry`: `TraceActionKey + proof descriptor digest + precision digest -> payload digest + coverage digest`.
5. `CAS object`: result payload, proof descriptor, coverage object, optional compressed chunks.
6. `Generation pack`: immutable batch of AC rows, CAS chunks, proof records, and mmapable indexes.
7. `Manifest`: small authoritative generation list, atomically replaced; startup reads the manifest and mmaps indexes, never replays unbounded history.

Publication proposal:

1. Write payload pack, event log, and index into an `incoming/writer-<pid>-<uuid>` directory on the same filesystem.
2. fsync files and containing directories.
3. Acquire a short cross-process writer lock only for manifest publication.
4. Rename immutable generation files into place.
5. Write and fsync a new manifest and atomically replace `CURRENT`.
6. Release the writer lock; readers remain lock-free because all generation files are immutable.

Key invariants:

1. Unknown descriptor type, unknown required feature, unsupported observation domain, corrupt bytes, digest mismatch, or incomplete proof means miss.
2. Bloom filters, fanout tables, and prefix indexes are negative accelerators only; acceptance requires exact canonical-key comparison and proof/coverage validation.
3. Payload mutation is impossible by construction; AC entries point to immutable CAS digests.
4. Startup cost must be proportional to live manifests/indexes, not total historical writes.
5. GC is generation-safe: readers pin/open the generation files they use; GC deletes only unreachable generations after a grace window.
6. No native C++ structs are serialized directly. Persistent bytes use explicit deterministic codecs with magic, version, required/optional feature bits, stable field tags, stable ordering, and fixed endian encoding.

Immediate performance implication:

The latest measured data before this note showed hot eval around 100s even after storage changes, while no-trace was roughly 44s and cold writeback was only about 1s. That means the next storage replacement must not merely make writeback faster. It must make hot-hit verification cheaper by moving enough semantic material into exact proof descriptors so we can soundly avoid revalidating large dependency sets on exact-proof hits. If exact proof construction is incomplete or too expensive, the correct behavior is to miss, not to accept a weaker cache key.

Adversarial checklist for the replacement work:

1. Prove every hit path checks typed evaluator identity, request identity, proof descriptor, precision, and exact key bytes.
2. Prove every observation domain has positive and negative observations, including attrset analogues of existence, shape/listing, selection/content, and source-position observations.
3. Prove unknown/unsupported domains are uncacheable or miss-only.
4. Prove manifest publication is atomic under crash at every write, fsync, rename, and directory-fsync boundary.
5. Prove generation GC cannot remove files needed by an active reader.
6. Benchmark null-storage, verification-only, lookup-only, decode-only, mmap fault behavior, and writeback-only before committing to a backend.
7. Compare against the pre-schema SQLite baseline and the current implementation; the target is to beat both cold and hot time, not merely recover pre-schema performance.

## Measurement note: attrset replay fast path

## Rejected experiment: full-trace-hash verification memo

Benchmark directory: `eval-trace-bench-results/manual-fullhash-verify-memo`.

Comparison against accepted `manual-capsule-sorted-run`:

1. Eval JSON output stayed byte-identical for cold, hot, and no-trace runs.
2. Hot traced wall regressed from `1.48s` to `1.51s`.
3. Cold traced wall regressed from `1:30.89` to `1:32.70`.
4. The added `fullHashMemoHits` counter was `0` on the hot run.

Conclusion: full-trace-hash memoization is dead complexity for this workload. The current workload has repeated trace loads but not repeated reusable full trace hashes in the shape this optimization needs. Keep the measured capsule sorted-run decode fast path; do not reintroduce full-trace-hash verification memoization unless a future benchmark first demonstrates repeated full hashes and a clear semantic model for trace-local override side effects.

## Rejected experiment: fused full-trace load and verification

Benchmark directory: `eval-trace-bench-results/manual-fused-verify-load`.

The change fused top-level `loadFullTrace`, volatile scanning, and `verifyTrace` into one blocking handoff, passing the already-loaded immutable dep vector into verification. The design was modeled after a covering-index/projection-cache shape: once a projection is loaded for one predicate, reuse it for the next predicate instead of re-querying the cache.

Measured result against accepted `manual-capsule-sorted-run`:

1. Eval JSON output stayed byte-identical for cold, hot, and no-trace runs.
2. Hot traced wall regressed from `1.48s` to `1.54s`.
3. Cold traced wall regressed from `1:30.89` to `1:32.07`.
4. It reduced `loadTrace.count` from `77` to `61` and `verifyTraceCallUs` from `459904us` to `427679us`, but full decode time and real wall time got worse.

Conclusion: the redundant `loadFullTrace` cache-hit path was not the bottleneck. This refactor made internal counters look cleaner but did not improve end-to-end performance, so it was removed. Future hot-path work should target the expensive work that remains: full capsule decode, dependency hash resolution, and the need to read/decode the full dep projection at all.

## Rejected experiment: raw capsule storage envelope

Benchmark directory: `eval-trace-bench-results/manual-raw-capsule-envelope`.

The trial changed the outer stored-capsule envelope from a probe frame followed
by a zstd-compressed full capsule to a probe frame followed by raw capsule
bytes. The motivation was the decode-counter run, where hot full-capsule decode
spent `209522us` in envelope decode, `62331us` in deps decode, `27783us` in
strings decode, and `0us` in sort fallback. The model was the standard
immutable-object/data-plane split used by Git pack objects and CAS stores:
storage codec should be an optimization, not object identity.

Measured result against the accepted sorted-capsule run:

1. Eval JSON output stayed byte-identical for cold, hot, and no-trace runs.
2. Hot traced wall regressed from `1.48s` accepted / `1.51s` instrumented to `1.69s`.
3. Cold traced wall regressed from `1:30.89` accepted / `1:31.34` instrumented to `1:31.67`.
4. Envelope decode improved from `209522us` to `178007us`, but full payload bytes grew from `53551068` to `132825142`, and full payload read time grew from `8868us` to `134017us`.
5. Manifest encoded payload bytes grew from `107124462` to `265681038`.

Conclusion: raw envelopes remove some zstd CPU but lose more to IO/cache/hash
pressure. Keep the bounded probe prefix plus compressed full payload. Future
codec work should use a faster compressed or block-indexed format, not
uncompressed full capsules.

## Replay-range finding: blocked and duplicate attempts

Benchmark directories:

1. `eval-trace-bench-results/manual-replay-result-counters`
2. `eval-trace-bench-results/manual-replay-blocked-fastpath`
3. `eval-trace-bench-results/manual-replay-duplicate-gate`

The replay-result counter pass showed that cold `closures` had `20881282`
range hits but only `423400` actual dep additions. The rest split into
`10598678` epoch-scope-blocked attempts and `9859204` duplicate-value attempts.
This means non-empty range elision is not automatically sound: most skipped
attempts are skipped because the current recording scope cannot accept the
range, or because the value has already been replayed into that exact scope.

Accepted change: remove the diagnostic NarIdentity scan from the blocked-range
path. It was walking the blocked range before returning false. That kept the
same semantics but reduced `rangeCopyTimeUs` from `541101us` to `144313us` in
the countered cold run. Eval JSON stayed byte-identical. Cold wall recovered
from `1:32.63` to `1:31.16`, close to the accepted sorted-capsule baseline
(`1:30.89`) despite the remaining counters.

Retained-state rerun: `eval-trace-bench-results/manual-retained-replay-fastpath`
kept output byte-identical. Cold traced wall was `1:31.24`, hot traced wall was
`1.56s`, and no-trace wall was `44.70s`. Replay remained
`10598678` blocked attempts and `9859204` duplicate attempts, with
`rangeCopyTimeUs = 223972us` and `memoizedTimeUs = 247965us`. The remaining
large cold gap is therefore not replay copy alone.

Rejected change: gate-level duplicate skip. The intended optimization was to
avoid the second range lookup for values already replayed into the current
scope, disabled during sibling capture to preserve `SiblingReplayCaptureScope`
semantics. In practice `gateDuplicateSkips` stayed `0`, so the gate does not
see the relevant scope state. Cold wall regressed to `1:31.86`. Remove it.

Post-change benchmark directory: `eval-trace-bench-results/manual-attr-replay`.

Comparison against the earlier manual instrumented runs:

1. Baseline instrumented cold traced: `98.19s` wall, `112.53s` user, `11.21s` sys, `7,371,424 KiB` RSS.
2. Weak replay cold traced: `92.35s` wall, `123.07s` user, `11.56s` sys, `6,922,512 KiB` RSS.
3. Attr replay cold traced: `91.31s` wall, `111.89s` user, `11.10s` sys, `7,047,460 KiB` RSS.
4. Attr replay hot traced: `1.61s` wall, `383,428 KiB` RSS, `42` hits, `0` misses.
5. Attr replay no-trace: `44.11s` wall, `50.98s` user, `5.03s` sys, `3,044,768 KiB` RSS.

Interpretation:

1. The weak replay map fixed a real GC-retention problem, but Boehm disappearing-link registration made cold CPU worse.
2. The attrset stamp fast path recovers most of that CPU cost and gives the best cold wall time so far.
3. RSS is still far above no-trace, so the remaining cold gap is still dominated by trace metadata lifetime and replay/recording overhead, not SQLite writeback.
4. `recordThunkTimeUs` is now lower than weak replay-only (`1.30s -> 0.72s`) but still far above the original map (`0.043s`).
5. Replay range hits grew from `7.29M` to `20.88M`, and exact misses grew sharply. That is a warning that attrset replay is recording/replaying too broadly. The next local optimization should tighten replay recording semantics so only values with a useful dependency range enter replay storage.

Next immediate implementation tasks:

1. Add counters to split replay records by attrset-stamped vs weak-pointer-stamped values.
2. Avoid storing empty or no-op dependency ranges.
3. Avoid storing attrset ranges when the range is too large to be profitably replayed or likely duplicates parent/sibling coverage.
4. Rebuild and rerun `closures` cold/hot/no-trace after each semantic optimization.
5. Keep the replacement-store plan separate: it is the larger path to beat pre-schema hot/cold behavior, but current instrumentation is still useful for understanding the in-memory overhead we must remove or bypass.

## Rejected experiment: external-LZ4 capsule storage envelope

Attempted to move the immutable trace capsule storage envelope from the existing zstd v2 payload to an explicit-codec v3 LZ4 payload. The target was hot-hit latency: full-capsule decode remained a measurable part of `loadTrace.capsuleDecode`, and prior raw-envelope testing showed that eliminating compression entirely lost to read bandwidth and cache pressure.

Result: rejected. `CompressionAlgo::lz4` currently initializes the external `lz4` program rather than an always-available in-process codec in the functional-test environment. The local build failed eval-trace tests because cold cache population emitted `couldn't initialize compression (Using external lz4 program)`, and the subsequent `NIX_ALLOW_EVAL=0` warm run then correctly failed with `not everything is cached, but evaluation is not allowed`.

Adversarial conclusion: codec agility is still a reasonable architectural direction, matching the content-object/data-plane split used by Git pack files and LSM/SSTable designs, but only if the codec is part of the Nix runtime closure or linked in-process. A storage-format migration that silently depends on an undeclared compressor executable is brittle and can reduce cache soundness operationally by preventing writeback. Keep the zstd v2 envelope until a linked codec or closure-guaranteed executable is introduced and tested.

## Architecture checkpoint: beating the old SQLite baseline requires proof-level hits

The backend research converges on a hard constraint: changing only the storage container is unlikely to beat the pre-schema SQLite baseline by enough. Recent retained-state benchmarks still show hot runs dominated by hit validation and dependency/proof work after startup/writeback costs were reduced. A custom or SQLite-backed store can improve writeback and payload decode, but it cannot make hot hits fundamentally fast unless the cache key carries enough typed proof/session identity to make acceptance cheap and sound.

### Direction

Adopt an action-cache/content-addressed-store split for evaluation tracing:

- `TraceActionKey`: evaluator identity, language/trace ABI, evaluation mode, request key, and requested precision.
- `SessionProofDescriptor`: immutable typed identity for the ambient observation domains that the result depends on.
- `ObservationCoverage`: exact positive and negative observations covered by the result, including attrset analogues of filesystem observations.
- `AC entry`: maps `(TraceActionKey, SessionProofDescriptor, PrecisionDescriptor)` to content-addressed payload/proof objects.
- `CAS object`: immutable trace/value/proof blobs addressed by digest and schema id.

This is the same high-level AC/CAS split used by Bazel remote caching: an action cache maps a precise action identity to metadata, while the CAS stores immutable content. The local implementation should borrow immutable generation/index ideas from Git pack/index files, LevelDB/RocksDB manifest-and-SSTable designs, and SQLite WAL-style crash recovery, but not inherit their mismatched runtime semantics wholesale.

### Required semantic model

The session identity cannot be erased into a mutable row or loose generation id. A hot hit is sound only when one of these holds:

- The current typed proof descriptor exactly matches the stored descriptor.
- The stored coverage/precision descriptor is proven to dominate the requested descriptor under an explicit entailment relation.
- Otherwise the entry is a miss, not a best-effort hit.

Observation coverage must include negative facts. Filesystem analogues already exist conceptually: path presence, file content, directory listing, symlink target, store path validity, env var value/absence. Attribute sets need the same granularity: attr presence/absence, attr selection, attrset shape, source-position observation, and evaluator-mode-sensitive operations. This preserves precision: unrelated attrs should not stale an attr-presence proof, while adding the exact missing attr must stale it.

### Storage shape to investigate next

The preferred target is a custom immutable generation-pack store:

- Readers open a small manifest, mmap immutable sorted indexes, and pread/decode only selected payloads.
- Writers append private segment files, build segment indexes, then publish a new generation by atomic rename plus directory fsync.
- The manifest is authoritative; startup must be proportional to live generation metadata, not historical write count.
- Index lookup uses fixed-width digest/fanout for speed, then exact canonical-key comparison for soundness.
- Corrupt, unsupported, or mismatched records are misses/quarantine candidates, never approximate hits.

SQLite remains a possible control-plane fallback only if the hot path is denormalized and query plans are proven with covering indexes. The normalized relational schema was useful for expressing conflict resolution, but the measured hot path argues that a persistent cache should avoid per-hit joins and mutable session rows entirely.

### Implementation tasks

1. Define typed `SessionProofDescriptor`, `ObservationCoverage`, and `PrecisionDescriptor` structures and canonical encodings.
2. Add instrumentation to split hot-hit time into key construction, proof descriptor construction, lookup, payload read/decompress/decode, and verification/entailment.
3. Prototype exact-descriptor hot hits in the current backend before writing the full custom store, so the semantic win is measured independently from the storage rewrite.
4. Prototype a custom generation-pack reader/writer under a new cache namespace, shadow-writing first and treating unsupported/corrupt data as misses.
5. Keep the current accepted optimizations: sorted-run capsule decode and blocked-replay fast path. Do not reintroduce raw envelopes, external-LZ4 envelopes, duplicate replay gates, or fused load/verify without new data.

### Adversarial constraints

- Bloom filters, prefixes, and probes may reject candidates early, but may not accept a hit.
- Hashes accelerate lookup; exact canonical bytes or schema-bound descriptors resolve equality.
- Unknown observation domains, unknown required feature bits, or descriptor-schema mismatches must miss.
- Cache publication must be copy-on-write and crash consistent: write temp files, fsync files, rename, fsync directories, then publish manifest/current pointer atomically.
- The evaluator must not run while holding cache publication locks.

## Exact-session proof adversarial checkpoint

An attempted exact-session proof short-circuit was reverted to telemetry-only after the expr test suite caught seven unsound accepts: copied-path mutation/deletion, derived-store-path source/missing/error cases, and Git identity recovery cases with failing implicit structure guards. The root cause was a category error: recoverability or session-fingerprint coverage is not itself a proof of dependency preservation.

The next proof design must persist and compare a real proof descriptor rather than infer acceptability from dependency kinds. In particular it must preserve negative observations, implicit structure guards, derived-store-path error/missing semantics, copied-path source identity, and trace-context dependencies. Until those semantics are represented in the descriptor and reproduced by tests, exact-session proof data remains diagnostic only and cannot bypass `verifyTrace`.

## Parallel research synthesis

The backend research converged on a custom immutable generation-pack store with a Bazel-style AC/CAS split: action/proof keys map to payload metadata, while payloads, proof descriptors, coverage descriptors, and precision descriptors live as content-addressed immutable records. The storage pattern borrows from Git pack/index fanout and MIDX chunks, LevelDB/RocksDB immutable sorted files plus manifest/CURRENT publication, and SQLite WAL recovery discipline, but avoids inheriting a general-purpose B-tree/LSM engine where hot lookups should be fixed-width index probes and exact key comparisons.

Key rules from the research:

- Readers should open a small authoritative manifest, mmap immutable sorted indexes, and hold no writer lock.
- Writers should publish private append-only segments by writing temp files, fsyncing files and directories, and atomically replacing the manifest pointer.
- Cache acceptance must require exact canonical-key equality and exact proof/coverage/precision entailment; Bloom filters or hash prefixes may only reject work, never prove hits.
- Unknown required features, corrupt indexes, malformed descriptors, or unsupported observation domains must cleanly miss or quarantine a generation.
- We should not serialize native C++ structs. Persistent bytes need explicit little-endian, tagged, versioned codecs with deterministic ordering, schema/domain separation, and exact collision resolution by full canonical bytes.
- The current benchmark evidence still points to verification and dependency hashing as the dominant hot cost: run139 hot was about 1.04s per sampled commit with roughly 304ms in `verifyTrace` across seven verified traces in the sampled stats, while payload decode was not the dominant phase.

Immediate implementation implication: build the persisted descriptor/proof layer in shadow mode first. Only once descriptor equality/entailment captures copied paths, derived store paths, Git implicit structure guards, negative observations, attrset/list precision, and trace-context dependencies should a hot hit bypass `verifyTrace`.

## Benchmark checkpoint: run 140 after shadow proof metadata

Validation before benchmark:

- `nix build -L . --builders ''` passed.
- Functional tests: 218 passed, 8 skipped.
- Expr tests: 1812 passed, 3 skipped.

Comparison against run 139 and the earlier pre-schema run 121, using `eval-trace-bench runs --runs cold/121,hot/121,cold/139,hot/139,cold/140,hot/140`:

| run | wall total | wall mean | wall median | CPU total | hits | misses | hit rate | verify time | writeback git time |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| cold/121 | 432.68s | 4.33s | 1.09s | 462.98s | 599 | 101 | 85.57% | 34.01s | 0.00s |
| hot/121 | 98.63s | 0.99s | 0.99s | 55.38s | 700 | 0 | 100.00% | 29.76s | 0.00s |
| cold/139 | 470.96s | 4.71s | 1.24s | 474.72s | 599 | 101 | 85.57% | 40.85s | 8.56s |
| hot/139 | 103.75s | 1.04s | 1.04s | 55.46s | 700 | 0 | 100.00% | 33.08s | 0.00s |
| cold/140 | 520.64s | 5.21s | 1.53s | 488.83s | 599 | 101 | 85.57% | 62.90s | 9.51s |
| hot/140 | 112.36s | 1.12s | 1.12s | 55.89s | 700 | 0 | 100.00% | 37.12s | 0.00s |

Soundness: all outputs matched reference.

Adversarial interpretation:

- The shadow proof metadata preserved cache effectiveness exactly: same hit/miss totals as run 139.
- It did not buy performance yet because acceptance still performs full trace verification.
- Passive proof metadata is therefore only justified if it is the next step toward replacing verification for a strictly proven subset.
- The current persisted metadata should not grow further until it is either used to skip work soundly or moved to a cheaper/lazier sidecar.
- The next implementation step must be a verifier fast path with explicit immutable-environment preconditions, not more telemetry.


## 2026-05-15 checkpoint: raw capsule reuse benchmark

Build: `nix build -L . --builders ''` passed after rerun. The first attempt hit a non-reproducible `binary-cache` SIGSEGV in `nix-env`; the rerun passed functional tests (`218` passed, `8` skipped), expression tests (`1812` passed, `3` skipped), and the package build.

Benchmark command: `nix run .#eval-trace-bench -- generate --num-commits 100 --run-number 141`.

Compared runs:

- `cold/139`: pre-proof-metadata baseline, before passive proof descriptor persistence.
- `cold/140`: passive proof descriptor metadata added, before raw capsule reuse.
- `cold/141`: raw capsule reuse in Git-generation writeback.
- Matching hot runs: `hot/139`, `hot/140`, `hot/141`.

Results:

- Soundness: all outputs matched the `cold/139` reference.
- Hit rate: unchanged at `599` hits / `101` misses for cold and `700` / `0` for hot.
- Cold wall time: `cold/140` regressed to `520.645s`; raw reuse restored `cold/141` to `472.107s`, effectively back to `cold/139` at `470.960s`.
- Hot wall time: `hot/141` improved to `100.173s`, better than `hot/140` at `112.357s` and close to `hot/139` at `103.755s`.
- Cold Git writeback: `9.513s` in `cold/140` to `4.205s` in `cold/141`.
- Manifest encode: `7.584s` in `cold/140` to `2.279s` in `cold/141`.
- Re-encoded capsules: `762` in `cold/140` to `112` in `cold/141`.
- Raw payload reuse count: `299` in `cold/140` to `949` in `cold/141`.
- Encoded payload bytes: `837.9MB` in `cold/140` to `201.3MB` in `cold/141`.
- Raw payload bytes: `568.4MB` in `cold/140` to `1.205GB` in `cold/141`.
- Verification remains the dominant avoidable cost: `49.226s` in `cold/141`, `31.733s` in `hot/141`.

Adversarial review:

- The raw-reuse change is sound only because reuse is keyed by the content-addressed `(full_trace_hash, result_hash)` pair. If either trace content or result content changes, the pair changes and the path falls back to fresh encoding.
- The change must not be generalized to reuse by local trace ID, result ID, session ID, commit, or cache-hit status. Those identities are either process-local, mutable, or too coarse.
- This optimization is writeback-only. It does not change cache acceptance and therefore cannot reduce precision or soundness.
- The main remaining problem is not writeback encoding; it is repeated verification over already-known dependency facts. Any next optimization must replace or narrow verification using immutable proof descriptors, not accept candidates based on session identity alone.

Next implementation direction:

- Use the persisted proof descriptor as an acceptance precondition only when it is complete, immutable, schema-current, and tied to the exact observer/evaluator/session semantics.
- Add telemetry that distinguishes full dependency verification from descriptor/proof acceptance, so a fast path cannot silently hide verification work.
- Keep the old verifier as the fallback and as the test oracle until the proof path has adversarial tests for stale descriptors, missing observations, changed evaluator semantics, changed volatile inputs, and payload/descriptor mismatches.

## Checkpoint: run 142 exact-session proof instrumentation

Run 142 was built after adding proof rejection counters. `eval-trace-bench -- runs --runs cold/141,hot/141,cold/142,hot/142` reports all outputs match reference and the same cache effectiveness as run 141: cold has 599 hits and 101 misses; hot has 700 hits and 0 misses.

Aggregate timing/counters:

- `cold/142`: wall total 465.772s, wall mean 4.6577s, wall median 1.2269s, CPU 480.799s, verifier time 46.894s.
- `hot/142`: wall total 97.212s, wall mean 0.9721s, wall median 0.9728s, CPU 54.674s, verifier time 29.355s.
- Hot proof attempts: 700.
- Hot proof hits: 0.
- Hot proof misses: 700.
- Hot proof rejection shape: 700 `noGitIdentity`, 700 first-uncovered `FileBytes`.

Interpretation:

The proof path is currently structurally unable to cover nixpkgs hot hits. The candidates are dominated by file-content observations, but the loaded trace candidate does not expose an immutable Git/source identity that can cover those observations. This is useful because it distinguishes a semantic coverage gap from a performance bug: accepting exact-session hits now would be unsound unless the session key itself commits to the exact bytes of every file observation, which it does not.

Next implementation target:

1. Audit where `GitRevisionIdentity` observations are recorded, persisted, loaded, and attached to file-content dep keys.
2. Tighten provenance stamping so file-content observations can be covered by an immutable source descriptor when the source tree is known.
3. Add counters for the remaining blocker: whether file deps are unstamped, stamped with a different repo, or the source identity observation itself is missing from the candidate.
4. Keep exact-proof acceptance disabled until proof descriptors can demonstrate exact coverage and old verification remains an oracle in shadow mode.

## Checkpoint: root-load Git identity provenance fix

Run 142 showed that every hot exact-session proof attempt failed with `noGitIdentity` and first-uncovered `FileBytes`. The root cause was not SQLite or payload format: root-load `GitRevisionIdentity` observations were created with the repo path, but `TraceSession::recordRootLoadDeps()` recorded them through the generic root-load path without preserving `governingRepoId`. That erased the typed repo identity required by `extractGoverningRepoId()`.

Implemented fix:

- `TraceSession::recordRootLoadDeps()` now computes the same provenance stamp used by ordinary dep recording.
- `GitRevisionIdentity` is governed by the repo it fingerprints.
- Path-addressed root-load deps are governed by the containing repo root.
- The comment in code ties this to the AC/CAS proof-key model and surrogate-key/crosswalk pattern: persisted strings are presentation; the interned repo id is the semantic join key.

Regression test:

- Added `TraceCacheFixture.RootLoadGitIdentity_PreservesGoverningRepoId`.
- The test constructs a temp repo with `.git`, records a root-load `GitRevisionIdentity`, forces the traced root, then asserts the recorded dep has a nonzero `governingRepoId` resolving to the repo root.

Validation:

- Rebuilt with `nix build -L . --builders ''`.
- Functional tests: 218 passed, 8 skipped.
- Expr tests: 1813 passed, 3 skipped.

Adversarial note:

`exactSessionProofCovers()` is still telemetry-only in both verifier paths; full verification still runs even if the proof coverage predicate becomes true. This keeps the provenance fix sound while we collect run 143 data. Actual verifier bypass must remain gated on persisted proof descriptors, exact current-session descriptor equality, strict coverage/precision entailment, and old verifier shadow agreement.

## Checkpoint: run 143, root-load Git provenance restored

Run 143 was generated after preserving governing repository identity for root-load `GitRevisionIdentity` observations. The build and expr test suite passed, including the new regression test that forces a traced root value and asserts the resulting Git dependency keeps a nonzero governing repo id resolving to the repository root.

Benchmark comparison against recent runs reported matching outputs. Cache effectiveness did not regress: cold remained 599 hits / 101 misses and hot remained 700 hits / 0 misses.

Exact-proof telemetry changed materially on the hot run: attempts=700, hits=100, misses=600, `noGitIdentity=0`, `gitRecoverableHits=100`, `gitNotRecoverable=600`, with the first uncovered category still `FileBytes=600`. This means the previous provenance-erasure bug for the root Git identity was fixed, but most hot candidates still contain file-content observations that are not proven recoverable under the same immutable Git identity.

Current interpretation: the remaining blocker is no longer session identity absence. It is observation coverage. The next instrumentation pass must distinguish file-content observations with no governing repo id from observations governed by a different repo id, plus structured directory/projection cases that need their own source identity rather than being silently covered by Git.

## Architecture checkpoint: custom backend adversarial pass

A custom immutable backend only wins if it removes hot verification work. Replacing SQLite or the Git manifest data plane without changing proof acceptance mostly improves writeback and lookup constants; it does not remove the repeated dependency verification that dominates hot runs.

Refined target backend remains an immutable generation-pack AC/CAS store:

- durable keys are canonical bytes plus domain-separated hashes, not process-local ids;
- readers are lock-free over a pinned generation set and mmap fixed-width indexes;
- writers build packs/indexes off to the side and hold a short publication lock only for manifest/CURRENT swap;
- payload/proof/coverage objects are CAS-addressed over uncompressed canonical bytes, with compression only as storage envelope;
- indexes are projections and must confirm exact canonical key bytes before accepting a hit;
- corruption, unknown feature bits, missing objects, and checksum mismatches degrade to miss/quarantine, never partial repair during reads;
- GC is conservative via CURRENT, CURRENT.prev, reader leases, grace windows, and retained recovery generations.

The immediate implementation sequence should not start by replacing the store. First, finish proof/coverage codecs and exact-proof telemetry in shadow mode, add adversarial tests for unsupported observation domains, and enable exact-proof bypass only for a small, typed, proven subset. Once hot wall time moves, build the custom backend as a new namespace with shadow writes and opt-in reads.

libgit2 is useful for source identity and pack/index design prior art, but not as the primary eval-trace data plane: Git object lookup is `OID -> object`, while eval-trace needs `(action/proof/precision, attr path/recovery key) -> candidates`. Using libgit2 as the AC index would add mismatch and hidden ref/repack/delta costs unless benchmarked otherwise.

## Research-agent synthesis: storage is not enough

The parallel architecture reviews converged on the same shape: a custom immutable generation-pack store with an AC/CAS split is the right long-term storage direction, but it is not the first bottleneck to solve. Current hot runs remain dominated by verification/dependency proof work. A storage replacement that keeps re-running the verifier will not beat the existing pre-schema baseline by much.

Key storage direction:

- Use immutable generation packs: write temp pack/index/manifest files, fsync, then atomically publish a new manifest pointer.
- Keep readers lock-free over immutable mmap/read-only files.
- Keep writers private until a short publication lock.
- Use content-addressed payload/proof/coverage objects and fixed-width action-cache indexes.
- Treat corrupt, unknown, or unsupported data as a miss, never as a best-effort hit.
- Do not use Bloom filters or digest-only comparisons for acceptance; filters may only reject quickly before exact comparison.

Relevant patterns and sources cited by the research agents:

- Bazel remote cache AC/CAS split: action-cache entry points to CAS content, which matches trace action/proof key to immutable payload.
- LevelDB/RocksDB immutable sorted files plus manifests/CURRENT files: useful as publication/index inspiration, less useful as engines because LSM compaction is not the workload we want.
- Git pack/index/MIDX fanout and immutable object packs: useful layout pattern, but Git/libgit2 object semantics are not the eval-trace data model.
- SQLite WAL and manifest-style recovery patterns: good reference for crash recovery and one-writer semantics, not enough to fix verifier cost.
- POSIX atomic rename plus file and directory fsync: required for durable publication.

Adversarial conclusion:

The next safe optimization target is exact-session proof semantics, not storage. A custom store should be designed around a proof descriptor that lets us decide whether a candidate is acceptable without replaying dependency verification. If the exact proof is incomplete, ambiguous, or contains an observation kind whose entailment semantics are not implemented, the correct answer is miss or full verification, not a widened hit.

## Run 145: exact-proof blocker classification

Run 145 is telemetry-only relative to run 144; it adds the `otherKind` split for exact-session proof rejections.

Aggregates:

- `cold/145`: CPU 480.682s, hits 599, misses 101, verify 47.722s, exact proof attempts 0, deps checked 13,762,352.
- `hot/145`: CPU 54.702s, hits 700, misses 0, verify 30.273s, exact proof attempts 700, exact hits 100, exact misses 600, deps checked 12,737,294.
- Hot exact-proof miss split: 200 `TraceContext`, 400 `EnvironmentLookup`, 0 file-content governance failures, 0 volatile failures, 0 remaining unknown `other` kinds.

Interpretation:

- The root-load/file governance fix was sufficient for this benchmark; continuing to chase file-content provenance will not improve hot exact-proof coverage here.
- The hot proof problem is now semantic rather than mechanical: `TraceContext` and `EnvironmentLookup` need explicit typed entailment rules, or they must keep falling back to full verification.
- It would be unsound to treat `EnvironmentLookup` as covered by Git identity or by generic session equality unless the exact variable names and observed present/absent values are committed to the proof descriptor.
- It would be unsound to treat `TraceContext` as covered without understanding whether it recursively depends on another trace/result proof and whether verifier bookkeeping side effects are required.

Next work item:

- Inspect `EnvironmentLookup` and `TraceContext` recording/verification semantics and decide whether an exact-session descriptor can cover them with equality, recursive proof closure, or neither.

## Implementation step: current EnvironmentLookup equality in exact-proof telemetry

Patch intent:

- Keep exact-session proof as telemetry only; no verifier bypass is enabled yet.
- Accept mixed proof coverage in the classifier when file/content deps are governed by the trace's Git identity and `EnvironmentLookup` deps match the current process environment exactly.
- Leave `TraceContext` rejected because it requires recursive current-node proof, cycle rejection, and preservation of verifier side effects (`verifiedTraceIds`, `traceContextMemo`, and no unsound L1 dep-hash writes).

Soundness rule used for env vars:

- Stored `EnvironmentLookup` deps carry `depHash(observed value)` under the environment variable name.
- Current coverage re-reads only that exact variable through `EvalEnvironment::readEnvVar(observeOnly, name)` and compares `depHash(current value)` to the stored digest.
- This does not infer env validity from Git identity or from session-key equality. It is exact current-input equality.

Why this is still not a bypass:

- The current Git/file branch is a coverage predicate, not yet a complete proof that the current Git identity was checked and accepted under the same guard semantics as `verifyTrace`.
- The code comments continue to require guard-preserving semantics for copied paths, derived store paths, Git implicit structure, and trace context before full verification can be skipped.

## Run 146: env equality was necessary but not sufficient

Run 146 changed only exact-proof telemetry/classification for `EnvironmentLookup`.

Aggregates:

- `cold/146`: CPU 480.812s, hits 599, misses 101, verify 53.853s.
- `hot/146`: CPU 54.829s, hits 700, misses 0, verify 31.971s.
- Hot exact-proof attempts remained 700, hits remained 100, misses remained 600.
- Current env checks: 1,600; env hits: 1,600; env misses: 0.
- `EnvironmentLookup` no longer appears as the fallback uncovered kind, but the same 400 traces now reject on `StorePathAvailability`.
- The other 200 misses remain `TraceContext` and should not be widened without recursive current-node proof.

Interpretation:

- The env proof implementation is semantically correct but revealed the next blocker rather than increasing exact-hit count.
- `StorePathAvailability` is the next tractable equality proof: it is a current store validity observation with a simple stored value. It can be checked by asking the current store whether the exact store path is valid and comparing to the stored value.
- This is still telemetry, not a verifier bypass. Store validity equality is safe as a classification predicate, but a production bypass would also need the full proof descriptor and side-effect preservation described above.

## StorePathAvailability exact-proof classifier

Run 146 showed that making `EnvironmentLookup` exact by current equality removed the env uncovered rejects, but exposed 400 `StorePathAvailability` uncovered rejects. This is the right next boundary: store-path validity is not session identity and not Git-governed content. It is an external current-world observation.

Implemented a telemetry-only classifier that mirrors the verifier/resolver semantics exactly: decode the canonical store-path key, call `isValidPath`, classify as `"valid"` or `"missing"`, and treat decode/store exceptions as `"missing"` because the existing resolver does the same. The rule is intentionally string-valued because the recorded wire format for this observation is already the textual enum `valid`/`missing`; changing that representation is separate from proving the bypass safe.

Expected benchmark result: hot exact-session proof hits should rise only if the 400 env+store traces have no additional blockers. If the remaining blocker is only `TraceContext`, hot exact hits should move from 100/700 to about 500/700, with the remaining 200 rejects attributed to recursive trace context. This still should not materially improve wall time because it is classification telemetry only; the actual verifier is still run.

## Research-agent synthesis after StorePathAvailability classifier

Verifier-track agents agree that exact proof is still telemetry: `Verifier::verifyAttrImpl` computes proof coverage and then still runs full `verifyTrace`. Production speedup requires a bypass path, but not from the current predicate as-is. The Git branch must be tightened from "file deps share a governing repo" into current Git identity equality against the recorded GitRevisionIdentity proof. Env and store-path observations need exact current equality, and TraceContext remains out of scope until recursive proof semantics are equivalent to `resolveTraceContextHash`.

Storage-track recommendation is to move toward immutable generation packs with mmap-able indexes rather than making SQLite or libgit2 ODB the hot backend. Libgit2 is useful for manifest/ref CAS, but not for per-key hot lookup. Durable identity should be content hashes and length-delimited canonical bytes; dense numeric IDs should stay local to a pack/capsule.

## Run 147 finding: StructuredProjection provenance gap

Run 147 confirmed `StorePathAvailability` current equality is exact but not the final blocker. Hot exact proof remained 100/700; the new counters showed 2,618,772 store availability equality hits and zero store misses, while the remaining non-TraceContext rejects were 400 `StructuredProjection` deps. That means env/store were correctly classified, but structured Nix-binding deps were not Git-covered.

Root cause: structured deps have `governingRepoId`, but the Nix-binding recording path passed compact structured components into `recordStructuredDep`, whose fallback derived the repo from `filePathId`. For Nix bindings, `filePathId` may be source-relative; the already-resolved `SourcePath` is the correct absolute path for repo discovery. The fix passes `pools.internGoverningRepo(sp.path.abs())` from `nix-binding.cc` into structured recording. This preserves the intended semantics: a current Git identity equality proof over the source file covers its derived structured projections without reparsing.

## 2026-05-15 exact-session bypass implementation boundary

Implemented a narrow production fast path after the StructuredProjection provenance fix:

- `exactSessionProofCovers` now requires the stored `GitRevisionIdentity` hash for the governing repo to equal the current repo identity, using `VerificationSession::gitIdentityCache` to avoid recomputing the Git fingerprint repeatedly.
- Git coverage remains scoped to deps whose `governingRepoId` matches the trace's stored Git identity dep; `TraceContext` and volatile deps still fail closed.
- Environment lookups and store-path availability observations are still re-read from the current process/store and compared exactly before accepting.
- Acceptance is restricted to current-node hits, not history bootstraps or recovery candidates.
- Acceptance is centralized in `SqliteTraceStorage::acceptExactSessionProof`, which clears trace-scoped subsumption state, marks the trace id verified for this verifier session, and decodes the cached result without publishing a recovery/current-node mutation.
- Added tests that seed the current Git identity cache explicitly, plus a changed-Git-identity negative test. This keeps fake repo tests honest: absent or changed current Git identity must fail closed.

Remaining adversarial concerns before trusting this broadly:

- TraceContext remains excluded. A recursive proof model would need parent current-node identity and cycle handling equivalent to `verifyTrace`.
- Store-path availability equality currently performs per-dep checks in exact proof rather than reusing the existing `StorePathBatch`; this is sound but may be expensive until bypass savings dominate or batching is wired into proof checking.
- Git identity equality is only as precise as `computeGitIdentityHash`. That function must continue to represent every file-content observation covered by `governingRepoId`; otherwise Git coverage would be unsound.
- The exact proof bypass does not patch trace hashes for implicit-shape overrides. That is intentional: implicit-shape and trace-context cases are not accepted by this fast path.

## 2026-05-15: exact-proof bypass and backend direction update

### Current implementation checkpoint

The exact-session proof path is intentionally narrow and fail-closed. It may bypass full dependency verification only when each stored observation is entailed by a current proof atom or by a trusted exact-session descriptor. `TraceContext`, volatile observations, copied/derived source-to-store observations, and unknown domains remain verification-only until their proof descriptors encode the same current-world predicates as the verifier.

Adversarial finding from the latest unit failures: a helper used by both Git-identity recovery and exact-session bypass must not conflate "candidate can be recovered by Git identity" with "candidate can be accepted without full verification". Git recovery may index/select a candidate containing implicit-structure observations because the recovered trace is still validated before acceptance. Exact bypass cannot accept those observations yet because no current equality proof is persisted for their guards.

### Research notes and sources

SQLite remains useful as a correctness/reference backend, but it is structurally mismatched with the fastest target shape. SQLite's mmap support avoids read copies for mapped pages, but writes still copy pages into private memory and write them back on commit, so mmap is mostly a query optimization, not a writeback solution: https://www.sqlite.org/mmap.html. SQLite's query planner can choose among many algorithms, and `ANALYZE`/`PRAGMA optimize` can improve choices, but we are still paying B-tree/index/FK/crosswalk work for data that is naturally append-only and projection-oriented: https://www.sqlite.org/optoverview.html.

The recent SQLite `carray()` support is useful for avoiding temp-table staging of application arrays, including BLOB vectors in newer versions, but it optimizes the existing SQL reconciliation shape rather than removing it: https://www.sqlite.org/carray.html.

Libgit2's ODB APIs provide object read/write, object streams, pack writing, multi-pack-index writing, and custom ODB backends: https://libgit2.org/docs/reference/main/odb/index.html. That is valuable for manifest/ref CAS or coarse generation blobs, but the ODB abstraction is commit/tree/blob-oriented and does not directly provide the mmap-able typed lookup indexes we need for exact heads, recovery heads, proof descriptors, and candidate ranking.

Git's pack format and multi-pack reuse show the right broad pattern for immutable packed objects plus sidecar indexes, but using Git object packs as the hot eval-trace index would impose object header, hash, compression, and pack lookup costs unrelated to our keys: https://git-scm.com/docs/gitformat-pack.

Boost.Serialization is not a good durable cache format for this problem. The official docs distinguish portable text/XML archives from native binary archives, and native binary archives carry machine-layout assumptions like native type sizes and endianness; our cache needs explicit schema epochs, little-endian encodings, stable tags, and fail-closed feature negotiation: https://www.boost.org/doc/libs/latest/libs/serialization/doc/archives.html.

### Refined architecture direction

Primary direction: an immutable generation-pack backend with mmap-able projection indexes. SQLite should become a reference/import/export backend during migration, not the target performance architecture.

Durable identity should be semantic hashes and canonical length-delimited bytes, not global integer IDs. Dense numeric IDs remain valid inside a process, a capsule, or one pack section, but they must not be the durable cross-process conflict-resolution mechanism.

Each flush writes one append-only segment containing candidate events, runtime-root events, proof descriptors, raw trace-capsule payload references, and sorted projection-index sections. Publish is atomic rename plus manifest CAS. On conflict, reload the manifest head, merge event streams deterministically by generation/ordinal, rebuild projections, and retry.

Startup should mmap the latest snapshot/projection index and lazily touch capsule payloads. Periodic snapshots are mandatory; replaying unbounded segments would recreate a startup-time regression.

### Implementation task list

1. Finish validating the current exact-proof bypass subset with 100-commit benchmarks and adversarial checks.
2. Keep exact proof fail-closed: no `TraceContext` bypass, no derived/copied source-to-store bypass, no volatile bypass, no unsupported-kind bypass.
3. Add a sidecar generation-pack writer that dual-writes alongside SQLite without changing reads.
4. Define the first binary section format: file header, feature flags, endian/version fields, section table, checksums, varints, and length-delimited byte strings.
5. Encode dirty candidate/runtime-root events and raw capsule references into one immutable segment.
6. Build segment-local sorted indexes for exact head and recovery lookup; do not persist process-local intern IDs.
7. Add a pack reader in shadow mode that compares proposed heads against SQLite-derived heads and records mismatches.
8. Add compaction/snapshot planning before enabling pack-first reads.
9. Only switch reads to pack-first after shadow-mode equivalence and benchmark evidence show it beats SQLite on cold and hot eval.


## 2026-05-15: benchmark update after exact-proof gating

Runs compared: `hot/148`, `hot/149`, `hot/150`, `hot/151`.

Findings:

- `hot/149` regressed to `80.719s` CPU because exact proof computed current Git identity on almost every attempted hit before falling back.
- Reordering exact proof to classify deps before current-world checks reduced redundant dep checks but still left one current Git computation per eval process; `hot/150` remained `80.452s` CPU.
- Making exact proof opportunistic, using only an already-populated `VerificationSession::gitIdentityCache` entry and failing closed otherwise, removed the duplicate Git identity work. `hot/151` measured `56.554s` CPU, better than the pre-production-bypass `hot/148` at `58.785s` CPU.
- `hot/151` had `700` exact-proof attempts, `0` hits, and `0` bypasses. The current proof path is therefore not the long-term performance strategy. Its value is fail-closed telemetry and avoiding the earlier expensive env/store proof checks.
- `cold/151` was `482.544s` CPU, effectively unchanged relative to runs 148-150 at this noise level. Cold remains dominated by misses/history verification/writeback shape, not exact-session proof.

Decision:

Do not broaden exact proof to get artificial bypasses. The soundness boundary is correct: TraceContext, implicit structure, derived/copied source-to-store observations, volatile observations, and unknown domains remain full-verification only. The next performance work should move toward the immutable generation-pack sidecar/shadow backend rather than more SQLite/exact-proof micro-optimization.

## 2026-05-15: generation manifest string-dictionary tightening

Implementation step:

- New Git-generation manifests now write an empty manifest-level string dictionary.
- The reader still consumes the section for older v3 manifests, but no longer interns those strings.
- The durable uses of strings are already encoded at their semantic use sites: data-path components are framed bytes, runtime-root fields are framed bytes, and trace capsules carry capsule-local string tables.

Reasoning:

- Process-local string ids are allocator artifacts. Persisting and replaying the whole process string pool couples a disconnected generation to incidental interning order, which is the same class of problem we rejected for SQLite explicit-id writeback.
- This mirrors the AC/CAS and pack/index split in the longer-term design: indexes should point at canonical bytes or content hashes, not at a previous process's local ids.
- Compatibility with existing cache data is preserved at the format-reader boundary only: old manifests can be consumed, but the old interning side effect is intentionally ignored.

Adversarial check:

- Candidate rows only reference segment-local data-path ids.
- Data paths store component bytes directly.
- Trace/result/dependency payloads are decoded from trace-capsule-local string dictionaries.
- Runtime roots store source, fetch identity, nar hash, and store path as framed/hash fields.
- Therefore the manifest-level string dictionary is dead metadata for the active generation path.

Follow-up fix:

- `nix eval-info` previously resolved CLI attr paths through the global attr-name/string pool before looking up the loaded data-path node. That made the historical manifest string dictionary observable.
- It now resolves directly against `dataPathPool.lookupChild(parent, component)`, preserving read-only lookup while depending only on the durable data-path bytes.

### 2026-05-15: cached FullAttrs materialization guard

Implementation note: cached `FullAttrs` payloads are now retained as recovery history but are not materialized as warm hits. A functional-test failure in `nix flake check` showed that a stale root attrset could verify through an incomplete dependency set, expose an obsolete child name, and only then fail when the child trace was checked. That is unsound because the caller has already committed to the stale traversal shape.

Adversarial check: this guard is intentionally broader than the observed root-flake failure. Any attrset payload can be stale if the proof does not explicitly cover member shape. Scalars and other fully observed payloads can still be reused; attrsets require a first-class shape proof before serving from cache is safe.

Design implication: the next architecture should encode attrset shape provenance explicitly, probably as a durable shape object keyed by source/proof inputs, rather than treating a parent evaluation event as enough to validate child membership. This follows the same rule as proof-carrying/provenance cache systems: reuse is valid for the property actually covered by the dependency proof.

Refinement after rebuild: the first guard was too broad and intentionally broke tests that require non-root attrset cache hits under `NIX_ALLOW_EVAL=0`. The implemented guard is now root-only (`AttrPathId(0)`). This matches the observed stale flake-root traversal bug while preserving existing nested attrset reuse. The deeper design issue remains: long term, root and nested attrsets should both carry explicit shape provenance rather than relying on path-specific exceptions.

Second refinement: the materialization guard was removed. The correct fix is not to ban root `FullAttrs` hits, because cached root attrsets are required to navigate `-f` and flake outputs when `NIX_ALLOW_EVAL=0`. The root cause was lower: `EvalState::fileTraceCache` can be populated before a trace recording scope exists, so a later traced evaluation may reuse the parsed/evaluated file without recording the coarse `FileBytes` source backstop. Cache hits now emit that backstop when a trace scope is active. This makes root attrset shape reuse fail closed on source changes while preserving root cache traversal for valid cache-only evaluations.

### 2026-05-15: immutable candidate head projection tightened

Startup now rebuilds exact-session heads by selecting the candidate with the highest persisted `nodeStamp` for each attr path, rather than relying on vector/load order. Recovery buckets are likewise sorted newest-first by `nodeStamp`. The flake-check stale-root failure survived the source-provenance fix because manifest candidate order is not a sound head relation after segmented/snapshot load; append-only event streams need an explicit deterministic projection key. This matches the immutable-log/head-index model in the custom generation-pack plan.

### 2026-05-15: flake exact-session source identity tightened

`FlakeSourceIdentity` now includes `LockedFlake::getFingerprint()` when it is available. The stable recovery key remains based on the original source identity, but exact-session rows are keyed by the locked source snapshot.

Reasoning:

- Exact-session identity is an action-cache key, so it must include exact input content identity.
- Stable recovery identity is a history-search key, so it should remain stable across source revisions.
- The old adapter accepted the fingerprint argument and ignored it. A path flake rewritten in place could therefore reuse a stale root `FullAttrs` row and then fail later while forcing a vanished child.

This is the same AC/CAS split used by remote caches and Git-like stores: stable refs/labels help find candidates, but immutable object/action digests are required before accepting cached output.

### 2026-05-15: missing trace payloads fail closed

Adversarial review found that `loadFullTrace()` returned an allocated empty dependency vector when trace metadata or raw capsule bytes were missing. That conflated a legitimate decoded zero-dependency trace with a corrupt/incomplete cache entry. Because empty dependency sets trivially verify, missing payload data could be accepted as a valid cache hit.

Implementation:

- `loadFullTrace()` now returns `nullptr` when the trace header or raw capsule is absent.
- Verification, recovery, replay, and eval-info callers treat `nullptr` as a miss/skip.
- Legitimate zero-dependency traces remain representable: they still decode a real capsule whose dep count is zero.

Soundness rule:

Cache metadata is not proof. A cached result may be accepted only after its persisted proof object has been loaded and decoded successfully. This matches the AC/CAS boundary: an action-cache entry pointing at missing CAS content is a miss or corruption, never a valid empty action.

### 2026-05-15: FullAttrs result identity uses durable attr-name bytes

`FullAttrs` result payloads now encode attribute names as stable string bytes, not
process-local intern ids. Result hashes are persistence-boundary identity: they
need natural keys, while intern ids are only in-process surrogate keys. The same
rule appears in the SQLite write-back design as a staging/crosswalk pipeline:
local ids may be bulk-loaded for speed, but canonical identity and conflict
resolution use durable keys.

This closes a concrete stale-hit class where separate processes could assign the
same local attr-name id to different one-key root attrsets (`overlays`,
`nixosModules`, `packages`) and therefore reuse the wrong `FullAttrs` result
payload. The eval-trace cache epoch is bumped to isolate the incompatible result
payload encoding instead of accepting or bridging old cache records.

Trace capsules now preserve the `FullAttrs` payload as durable JSON rather than
rewriting attr names through the capsule string table. That keeps the same
natural-key rule at both persistence layers.

The same epoch also removes persisted `ValueIdentityStamp` from container
metadata. Those stamps are runtime-local surrogate keys for pointer/equality
acceleration; importing them from another process could make unrelated cached
containers compare equal without forcing their children. Replayed containers
therefore mint or recover local identity only inside the current process.

### Open adversarial findings to track

- `nodeStamp` is still process-local. Exact heads now project by highest persisted stamp, but concurrent publishers that start from the same generation can create equal stamps. The durable model should sort by a segment/generation ordinal plus local event ordinal, or restamp local dirty rows after conflict import.
- `nix eval-info` now uses durable data-path bytes, but its attr-path tokenizer is still not the full Nix attr-path parser. Quoted/escaped and empty attr names need explicit tests and probably shared parser support.
- `fileTraceCache` cache hits now restore the coarse `FileBytes` backstop. They do not yet replay Nix-binding structured projection observations. This is sound but less precise and can cause avoidable invalidation.
- Exact-session Git proof is currently a cache-only shortcut over `VerificationSession::gitIdentityCache`; cold process restarts usually fall back to full verification first. That is fail-closed, but the intended cold-hit/hot-hit semantics should be made explicit in counters/tests.

## 2026-05-15 checkpoint: process-local identity cleanup and exact container proof

Validation:

- `nix build -L . --builders '' --keep-failed` passed.
- Expr tests: `1823` passed, `3` skipped.
- The prior blocker `TracedDataTest.PointerEquality_FindAlongAttrPath_SameCommandPath` now passes.

Implemented correctness fixes:

1. Removed persisted durable container identity from trace result payloads. Process-local value stamps are no longer encoded into durable JSON metadata, and the decoder rejects both legacy durable identity and process-local identity metadata in persisted container payloads.
2. Kept container identity as a runtime-only proof mechanism: copied attrsets/lists can recover their `ValueContext` through process-local stamps, but persisted bytes cannot claim pointer-like identity.
3. Tightened alias decoding: `aliasOf` now rejects self references, forward references, negative/non-integer references, and out-of-range references.
4. Fixed sibling alias ordering by sorting real-tree siblings lexicographically before alias detection, matching canonical result encoding order.
5. Fixed exact real-target proof for copied containers. Two resolved real `Value *` slots can differ while both point at the same underlying `Bindings` or list backing. Exact proof now accepts only when both paths resolve inside the same live `TraceSession` and the real targets either are the same `Value *` or share the same real container backing.

Soundness position:

- Durable storage still cannot persist process-local identity.
- Runtime exact proof remains fail-closed: no live same-session proof means no equality claim.
- The new attr/list backing comparison is not a cache-key shortcut. It is a current-evaluation proof over the real evaluated tree, analogous to the AC/CAS rule used throughout this plan: an index/key proposes a candidate, but exact current evidence is required before accepting it.
- Strong stamp mismatch still blocks equality for unforced traced producer thunks; copied forced containers may use exact same-session proof or same-parent alias proof.

Adversarial follow-up items from review:

1. Split `isTracedProducer` into a more precise identity-kind taxonomy. Today it is overloaded: it means both "came from a traced child" and sometimes "still an unforced producer thunk".
2. Scope `ValueIdentityStamp` more explicitly. Alias stamps should not be able to become cross-session proof by accident; acceptance should be tied to a live `TraceSession`, backend/proof descriptor, or exact real-tree evidence.
3. Harden canonical attr payload decoding further: reject duplicate attr names and non-canonical attr order if durable payloads rely on sorted canonical order.
4. Add negative tests for cross-session stamp collisions, stale/dead exact-proof sessions, forced attr/list copied containers, and unforced stamped-mismatch aliases.
5. Memoize `TraceSession::resolveRealTargetForIdentity()` by `(TraceSession, AttrPathId)` after semantics are settled; exact proof can otherwise repeatedly force/navigate the real tree.

## Parallel research synthesis: next performance targets after build green

The latest research agents independently reached two conclusions.

First, the long-term storage direction remains a custom immutable generation store with an AC/CAS split, not deeper dependence on SQLite or libgit2 as the primary data plane. The concrete target is:

- immutable generation manifests;
- content-addressed trace-capsule/proof/coverage objects;
- mmap fixed-width hot indexes;
- append-only candidate and runtime-root event logs;
- an atomic `CURRENT` pointer;
- short writer lock only at publication;
- offline/asynchronous compaction;
- SQLite as migration validator or debug fallback, not a hot-path authority.

This follows the same broad storage patterns documented earlier: Bazel action-cache/content-addressable-store separation, Git pack/index fanout over immutable objects, LevelDB-style immutable sorted files plus manifest/CURRENT publication, and POSIX atomic rename plus file/directory fsync for crash-safe publication. The important Nix-specific point is that these are patterns, not engines to inherit wholesale: eval tracing needs composite lookup by action/proof/precision/path, exact proof descriptors, and observation coverage, not just `OID -> object`.

Second, replacing the storage container alone will not beat the old SQLite baseline unless hot-hit verification work also shrinks. The highest-priority performance tasks are:

1. Persist and use exact proof/probe projections so exact-session hits can avoid full capsule decode and full dependency verification only when coverage is complete and typed.
2. Move structural probes out of full payload reads. `loadTraceProbe` should not read/hash the whole full capsule when the probe can reject cheaply.
3. Introduce a canonical dep-run object so recording computes sorted deps, partition views, trace hash, full trace hash, and hashable counts in one pass.
4. Add a binary result payload format or at least remove avoidable payload string copies from the JSON result decoder.
5. Stop startup/session load from scaling with all historical candidates; materialize indexes for active session/recovery keys only.
6. Memoize exact identity target resolution and add counters for resolve calls, path depth, force count, and tier-hit shape.
7. Collapse materialization origin lookup into a single pass and avoid the second attr traversal/dynamic-cast pass for prefetch hints.
8. Index parsed Nix attr bindings for structured-dep verification instead of linearly scanning bindings on hash misses.
9. Replace writeback string-concatenation keys with fixed struct keys and avoid raw payload owner copies.
10. Keep SQLite staging as debug/fallback-only or make it strictly dirty-only if it remains benchmark-visible.

Immediate next action:

- Run a new `eval-trace-bench` on the passing build.
- Compare against recent runs and the pre-schema SQLite anchor.
- Use the data to choose between proof/probe projection work and in-memory replay/recording overhead work for the next implementation slice.

## 2026-05-15 sidecar synthesis and current 100-commit benchmark anchors

Latest 100-commit anchors before the current rebuild:

| Run | Shape | Cold wall sum | Hot wall sum | Hits/misses | Notes |
|---:|---|---:|---:|---:|---|
| 121 | pre-schema SQLite | 432.7s | 98.6s | cold 599/101, hot 700/0 | Baseline remains unacceptable but must be beaten. |
| 154 | Git/capsule hybrid + current identity work | 551.7s | 97.0s | cold 579/121, hot 700/0 | Hot roughly matches baseline; cold regressed from lower hit rate and in-memory tracing/replay/verify cost. |

Interpretation:

1. Startup/writeback is no longer the largest hot-path issue.
2. Cold is worse partly because hit rate fell from `599/101` to `579/121`.
3. Hot remains expensive because exact-proof fast paths are not firing; full trace decode and dependency verification are still paid on hits.
4. A storage replacement is still desirable, but it must be paired with typed proof summaries that make exact hits cheap. A faster container alone will not beat the old baseline by enough.

## 2026-05-15 adversarial identity/session hardening

Recent review found that process-local container identity stamps can outlive the backend/session that made them meaningful. The invariant is now:

1. `ValueIdentityStamp` is a process-local crosswalk, not persistent identity.
2. A stamp authorizes pointer-equality only when its identity scope token is still live and the backend is still alive.
3. `PublishedRoot` identities are publication scaffolding for encoding/materialization, not live equality proofs.
4. Releasing a backend makes retained copied attr/list stamps fail closed; they must not prove equality in later sessions.
5. Stale stamped identities must not shadow a fresh pointer-keyed identity.

Implementation hardening in progress:

1. `shouldIsolateSiblingForce` requires a live identity scope before sibling capture.
2. root publication snapshots only live or unscoped previous identities, avoiding dead-session resurrection through rollback.
3. root publication no longer writes scope-less `PublishedRoot` entries into the stamped identity map for attrsets.
4. stamped attr/list lookup now uses live-stamp lookup before falling back to pointer-keyed identity.
5. `stampedValueIdentityMap` is being moved under the same `ReadOptimizedMap` lock discipline as the other identity maps because insertion can allocate and interact with GC/finalizer reentrancy.
6. New tests pin that copied attr/list stamps retained after `releaseBackend()` do not authorize equality.

## Next performance work items after run 155

Data-backed priority order:

1. Classify exact-proof misses by concrete `CanonicalQueryKind` instead of hiding them under `uncovered.other`.
2. Populate or lazily resolve current Git identity during exact proof; hot/154 had 100/700 exact-proof misses from Git-current misses.
3. Add typed TraceContext proof edges so exact proof can handle trace-context observations without full verification; hot/154 had 200/700 misses there.
4. Split trace capsules into proof/header, dependency block, and result payload so exact proof can run before full dep/result decode.
5. Persist/load a small exact-proof summary beside candidate heads and check it before `loadFullTrace`.
6. Fuse record hashing only after proof work; it helps cold misses, but the current 100-commit regression is dominated more by hit rate and validation than by writeback.
7. Tighten replay recording only where a counter proves semantic no-op or duplicate replay; prior broad replay gates were rejected.
8. Keep the custom immutable generation-pack backend as the medium-term target, using Git pack/AC-CAS/manifest patterns but not libgit2 as the semantic database.

Adversarial rule for all eight: if a proposed fast path cannot prove exact typed coverage, it must miss rather than accept a weaker cache key.

## 2026-05-15 progress note: run 158 and dirty candidate binding

Current benchmark anchor after the latest successful local build:

- `hot/158`: 93.938s, 700 hits / 0 misses. This beats pre-schema `hot/121` at 98.631s with the same hit rate.
- `cold/158`: 539.585s, 579 hits / 121 misses. This is still materially worse than pre-schema `cold/121` at 432.676s, 599 hits / 101 misses.

Interpretation:

- The Git exact-session proof gate fixed the hot regression from run 157; `exactUs` dropped from ~58.65s to ~0.016s.
- Cold still has a precision/hit-rate regression: 20 fewer hits than pre-schema. Restore hit rate before chasing storage micro-optimizations.
- Current aggregate writeback counters are zero for these runs, so the next bottleneck is not SQLite publication in this benchmark shape.

Soundness tightening applied next:

- A dirty candidate row is no longer enough to serve an in-memory result when the durable `(fullTraceHash, resultHash)` capsule pair is missing.
- Only same-process `FreshEvaluation` rows may rely on the in-memory pair before the shutdown publish boundary.
- `HistoryBootstrap` and `RecoveryPromotion` rows must be justified by durable content-addressed trace/result binding.

This preserves the disconnected/write-back-cache boundary: fresh runtime observations may live in memory until publication, but replayed/recovered observations must prove their binding through durable content-addressed objects.

## 2026-05-15 progress note: structural-variant precision regression

Run comparison isolated the cold hit-rate regression to five commits:

- Each changed from 7 hits / 0 misses in `cold/121` to 3 hits / 4 misses in `cold/158`.
- Four commits lost all four structural-variant recovery hits.
- One commit lost four history bootstraps after the same recovery/indexing changes.

Root cause found in structural-variant recovery:

- The probe/full candidate loop treated a mismatch between `current(key)` and the representative candidate's historical `hash(key)` as an early reject.
- That is not precision-preserving. Structural-variant recovery exists to recompute a representative key set against current values, then look up the resulting trace hash in history.
- A representative can therefore differ from its own historical values while still producing a trace hash for a valid historical trace/result pair.

Fix direction:

- Keep aborting on uncomputable deps. That is fail-closed and equivalent to full verification.
- Treat representative historical-value mismatches as telemetry only, not as candidate rejection.
- Acceptance remains gated by recomputed full trace hash lookup plus normal recovered-trace acceptance; no cached result is served from the representative mismatch itself.

## 2026-05-15 progress note: adversarial GitIdentity and trace-context fixes

Adversarial review found two GitIdentity soundness gaps:

- Git identity hashing covered HEAD plus tracked dirty/deleted files, but not untracked files. A trace that read an untracked generated file could be recovered after that file changed because GitIdentity did not include it.
- GitIdentity recovery reused exact-session fingerprint coverage in a recovery-key domain. Session-only observations such as `SessionSystemValue` and `DerivedStorePath` are not covered by Git state and must be recomputed by DirectHash/SV or rejected.

Fixes:

- `computeGitIdentityHash` now includes sorted untracked path/content entries in the GitIdentity digest.
- `allDepsGitRecoverable` no longer treats session-fingerprint-only deps as Git-recoverable; `DerivedStorePath` is explicitly rejected for GitIdentity recovery.
- Added regression tests for untracked-file GitIdentity hashing and session-only dep rejection.

Precision fix:

- Trace-context dependency resolution now verifies or recovers the parent attr before computing the parent trace/result context hash.
- This restores child hits when the parent can be soundly recovered, while retaining the existing in-progress trace guard for cycles.

## 2026-05-15 implementation note: deps-only verification projection

Added a dependency-only trace capsule projection for reject-heavy recovery paths. Primary verification continues to backfill result payloads so hot accepted hits do not regress by forcing an extra result decode. Recovery candidate scans opt out of result backfill because most candidates may be rejected after dependency/proof inspection.

Semantics: unchanged acceptance authority. The storage backend still validates the immutable capsule payload hash and identity header before exposing deps. The projection is an allocation/performance optimization only; it is not a new proof source.

Pattern tie-in: this follows the existing CAS/action-cache split and the Git pack-index/SSTable sorted-run pattern already cited in the capsule code: projections are accelerators, while the content-addressed object remains authoritative.

## 2026-05-15 continuation notes

Current correctness state:

- Local build is green after fixing result-payload semantic hash validation, stale structural-variant history lookup, raw-capsule conflict detection, malformed git-generation path counts, and test harness reentrancy.
- Probe-value mismatches in structural-variant recovery remain diagnostic rather than authoritative, because rejecting on mismatch would lose precision: the whole purpose of structural recovery is to recompute candidate deps under the current structure.

Current performance direction:

- Treat result sidecars as the first optimization slice. They are replay-only and never authorize a cache hit; they are usable only after exact verification, full verification, or recovery has already accepted the candidate.
- Add preflight summaries second in shadow mode, then enable narrow exact-session bypasses only when the summary proves no volatile deps, trace-context deps, implicit-structure guards, or derived-store-path checks requiring full verifier logic.
- Preserve full-capsule decode as the fail-closed fallback for all missing, corrupt, unknown-version, or hash-mismatching sidecars.

Implementation invariants:

- Durable sidecar identity must bind to canonical hashes, not process-local ids.
- Result sidecar replay requires recomputing computeResultHash(payload) and comparing it with the candidate result hash.
- Key/preflight sidecars may filter or avoid work only when complete enough to reproduce the existing verifier decision exactly.
- SQLite remains boundary-only until replaced by an immutable generation-pack backend; no runtime DB connection should be introduced.

Benchmarking:

- New 100-commit eval-trace-bench run started as run 900 after the latest correctness fixes.
- Compare cold/hot 900 against previous cold/hot runs and the pre-schema baseline before choosing the next optimization slice.

## 2026-05-15 sidecar refinement notes

Result sidecar implementation tightened after adversarial review:

- Result sidecars remain replay-only; they do not authorize hits.
- The result sidecar is now prefix-readable from object-backed capsules. Hot replay can read the storage envelope prefix containing probe + result frames instead of reading and hashing the whole compressed capsule first.
- Full capsule read, payload-hash validation, and `decodeTraceCapsuleResultPayload` remain the fallback for missing or malformed sidecars.
- Persistent full-trace loads now recompute trace hash, dep-key-set hash, and full trace hash from decoded deps using the store-owned dep-key feeder, so capsule headers are no longer trusted as self-authenticating.
- Git generation CAS conflict handling now fails closed if the concurrent generation cannot be reloaded.
- `applyOutcome` cleanup/return control flow was corrected so non-invalid outcomes return true and clear trace-scoped verified-file state as intended.

Benchmark run 900, collected before the prefix-readable sidecar refinement:

- Soundness: all outputs matched reference.
- Cold: 581 hits / 119 misses, 83.0% hit rate, mean wall 6.83s.
- Hot: 700 hits / 0 misses, 100.0% hit rate, mean wall 0.96s.
- Cold outliers are miss-heavy commits; hot replay is now close to ~1s/commit before the prefix sidecar patch.

Next measured step:

- Rebuild with prefix-readable sidecars and rerun eval-trace-bench as a new run number.
- Compare hot mean and `decodeCachedResultPayloadReadUs`/capsule decode counters against run 900 to see if prefix sidecars moved replay cost.
- If hot does not move materially, shift to exact-session preflight summaries because cold miss-heavy commits dominate current wall time.

## 2026-05-15 continuation: sidecar authority and backend direction

Adversarial review tightened the result-sidecar design: a prefix result sidecar is a fast projection, not independent authority. `decodeCachedResult` must only accept a sidecar after the corresponding full capsule has already passed payload-hash validation plus recomputed `traceHash`, `depSetHash`, and `fullHash` checks. If that marker is absent, result materialization falls back to full capsule decode and marks the raw object verified only after those checks pass. This follows the content-addressed storage/Merkle validation pattern: indexes and sidecars are hints; object content validates itself before it authorizes replay.

The same review exposed a capsule codec hole: full dependency rows were not self-contained for structured `sourceContentHash` material. The shared guard table intentionally omitted conflicting guards, which meant decoded deps could be insufficient to recompute the original hashes. The wire format is now bumped so each full dependency key row carries its optional source-content guard hash directly. The side table can remain as legacy/auxiliary compression, but validation no longer depends on it.

Research agents converged on a stronger long-term backend direction: a custom append-only pack/index with an mmap-readable index and atomic manifest should beat both SQLite and Git-object generation for this workload. SQLite remains useful as a catalog/debug/import path, but the hot path wants a storage model closer to Git pack indexes and LSM/SSTable sorted-run indexes: immutable records, content-addressed validation, and small prefix metadata before full payload materialization.

Current performance interpretation from run 900:

- Hot hit rate is `700/700`, but exact-session bypass is `0`; hot still decodes full capsules before exact-proof checks.
- Hot run 900 read roughly `762MB` of full capsule payload and spent about `5.1s` in full capsule decode over 100 commits.
- Cold run 900 regressed versus older runs mostly through full capsule/object reads and structural-variant dependency resolution; full decode time was about `55s` over 100 commits.

Next implementation priorities:

1. Finish correctness tightening for sidecars and self-contained capsules, then rebuild.
2. Re-run `eval-trace-bench` against run 900 to quantify the sidecar-authority/self-contained-capsule changes.
3. Add exact-session preflight summaries before `loadFullTrace`; summaries must be immutable and bound to `fullHash`, `traceHash`, `depSetHash`, `resultHash`, and the payload identity.
4. Add benchmark metrics for result decode, full/probe loads, exact-session preflight outcomes, and Git writeback/load phases.
5. Prototype a custom append-only pack/index once hot-path measurements show sidecar/preflight limits.

## 2026-05-15 hardening checkpoint: canonical persisted encodings

Adversarial review of the Git-generation and sidecar bridge found several places where persisted projections could be accepted too late, accepted non-canonical bytes, or allocate too much before failing. The implementation now treats malformed Git generations as a fallback boundary rather than a constructor failure: startup catches generation-load errors, clears partial in-memory state, and falls back to the SQLite bulk load path.

Tightened validity boundaries:

- Git-generation manifests now have total-byte, segment-count, and per-section row caps before large allocation.
- External payload objects are validated with final-symlink rejection, `fstat` regular-file checks, and size caps in lifecycle/writeback paths as well as replay paths.
- Runtime-fetch identity blobs now reject duplicate and out-of-order attrs, matching the canonical map-ordered encoder.
- Capsule/probe data-path rows now reject unused name fields on array-index children.
- Dirset capsule serialization now sorts digest ids and members before interning strings, so equivalent records produce stable capsule bytes.
- Result sidecar frame limits now cover the admitted result payload plus aux envelope instead of making valid capsules ineligible for sidecars.

Semantics: these are fail-closed/canonicalization fixes, not new acceptance authority. They reduce malleability and late surprises in the persisted projections while preserving the rule that indexes/sidecars can accelerate or reject, but full canonical content and proof checks authorize hits.

Targeted validation passed:

- `TraceStoreTest.ExternalCapsuleProbe_RejectsFinalSymlink`
- `TraceStoreTest.RuntimeFetchIdentityDecoder_RejectsDuplicateAttrs`
- `TraceStoreTest.RuntimeFetchIdentityDecoder_RejectsOutOfOrderAttrs`
- `TraceStoreTest.CapsuleProbeFrame_RejectsOversizedStoredFrameLength`
- `TraceStoreTest.CapsuleResultSidecar_AllowsResultFrameLargerThanProbeCap`

Architecture review refinement:

- Exact-session preflight should not be widened from current capsule probes alone; probes are projections and may omit full proof coverage.
- A safe preflight needs an immutable, typed proof summary bound to the canonical trace/result identity and payload identity, then shadow-checked against the current full verifier before enabling bypass.
- The custom backend should remain append-only and copy-on-write: write private objects/indexes, fsync, compare current head, atomically publish, and fail closed or merge on conflict.

## 2026-05-15 checkpoint: complete-probe shadow and rebuild/benchmark state

- Added complete-probe preflight as shadow-only instrumentation. It may compute whether a complete probe would have accepted or rejected, but it never authorizes a cache hit and always falls through to the existing authoritative full-trace verification path.
- Added focused tests for complete-probe shadow hit/miss accounting, runtime-fetch canonical attribute decoding, and fd-safe external capsule probe handling. The focused `nix-expr-tests` run passed all 5 selected tests.
- Removed the dead non-try git-generation payload opener left by the fd-scoped validation split.
- The interrupted `eval-trace-bench` run 901 is not authoritative for the latest build. It failed during hot evaluation because the local `result` symlink pointed at `.#nix-expr-tests`, so `result/bin/nix` was absent. The next valid comparison must use a fresh run after `nix build -L . --builders ''` restores a full Nix `result`.

Correctness stance: probe bytes and result sidecars remain projections. They can reject candidates or measure possible fast paths, but only hash-bound stored-envelope/proof authority may eventually authorize returning a cached result without loading the full trace.

## 2026-05-15 checkpoint: adversarial soundness fixes

Adversarial review found that two projection fast paths could become authority accidentally:

- Zero-dep exact-session preflight was able to decode a result sidecar from a reopened cache before the raw trace capsule payload had been verified in the current process. That is now rejected. The first reopened hit must verify the stored capsule envelope; only later in-process hits may use the sidecar/cache as a fast path.
- History bootstrap could select a row by stable recovery key and then accept it through primary verification even when the trace carried `GitRevisionIdentity` source identity. Source identity is now treated as recovery proof material: history rows with Git identity route through recovery, and recovery acceptance rechecks stored GitRevisionIdentity deps before serving.
- Dependency/result projections now reject array data-path rows with nonzero name ids, matching full-capsule validation. Projection decoders remain accelerators; malformed projections must fail closed.

This is intentionally conservative. It may reduce recovery precision for traces that have matching content deps but stale Git identity, but it preserves the stronger semantic stance: source identity recorded in the capsule is part of the proof required to serve a history/recovery hit.

## 2026-05-15 sidecar architecture review: SQLite replacement direction

Recommendation from parallel review: move the hot eval-trace cache backend toward immutable generation files with mmap fixed-width indexes and framed capsule payloads. This borrows the shape of Git pack/index and multi-pack-index designs, but does not make libgit2 or Git object storage the durable authority: eval-trace has its own schema epochs, hash domains, proof semantics, canonical dependency material, and conflict rules.

Reference patterns:
- Git pack/index: immutable packed objects with indexes from object identity to offsets.
  https://git-scm.com/docs/pack-format.html
- Git multi-pack-index: separate index layer over multiple immutable packs/generations.
  https://git-scm.com/docs/multi-pack-index
- SQLite WAL/mmap remain useful references, but they do not eliminate runtime connection/locking concerns.
  https://www.sqlite.org/wal.html
  https://www.sqlite.org/mmap.html
- Cap'n Proto and FlatBuffers are useful serialization references, but eval-trace canonicalization must remain explicitly owned by this code because proof bytes are the cache authority.
  https://capnproto.org/encoding.html
  https://flatbuffers.dev/evolution/

Proposed generation layout:
- `manifest`: generation id, schema epoch, hash algorithm, file sizes, file hashes, and format versions.
- `objects.pack`: framed trace capsules, proof descriptors, probes, and result sidecars.
- `objects.idx`: mmap fixed records keyed by authoritative object identity, at least `(full_hash, result_hash)` once result-bearing traces are considered.
- `exact.idx`: mmap fixed records for exact-session-style lookup keys; this is an accelerator, not replay authority.
- `recovery.idx`: mmap fixed records for recovery candidate enumeration by stable attr/policy/source/probe keys.

Soundness constraints:
- Indexes and projections are accelerators only. They must not authorize replay without object-hash/proof equivalence checks.
- Object headers must bind schema epoch, hash algorithm, full trace hash, dependency/proof hash material, result hash, source-content guard material, payload kind, and payload hash.
- Conflicting objects for the same authoritative key are corruption and must fail closed or quarantine a generation.
- mmap is only for immutable files opened by fd and size-validated before mapping; generation publish must be temp-write/fsync/atomic-manifest-rename.

Next implementation experiments:
- Prototype `objects.idx` and `exact.idx` over existing capsule/result bytes before replacing the write path wholesale.
- Measure startup, exact-hit latency, recovery candidate enumeration, shutdown publish, page faults, syscalls, and bytes decoded.
- Add crash/corruption tests: truncated generation, swapped payload object, duplicate key with conflicting payload, stale manifest, unknown epoch, and hash-algorithm mismatch.

## 2026-05-15 sidecar correctness review: follow-up fixes

Parallel adversarial review found one active authority-boundary bug and several design constraints:

- Fixed: deps-only capsule projection verification no longer authorizes result-sidecar replay. The raw capsule snapshot now distinguishes deps projection verification from full/result-bearing payload verification.
- Fixed: recovery attempted-candidate deduplication now uses `(trace_id, result_id)`, not `trace_id` alone, so stale or inconsistent history rows cannot suppress a later valid trace/result pair for the same trace.
- Fixed: result-projection capsule decoding now rejects duplicate source-content guard rows, matching the full/deps decoders.
- Fixed: source-identity bootstrap guarding now uses a central taxonomy predicate for implicit structural identity deps instead of directly matching `GitRevisionIdentity` at the call site.

Constraints preserved:
- Result sidecars and projection payloads remain accelerators, not authority.
- History lookup may select candidates, but source-identity-bearing rows must pass recovery/guard validation before serving cached results.
- Descriptor-backed exact hits remain disabled until descriptor digest, coverage, precision, schema epoch, and hash algorithm are durably persisted and bound to the authoritative object.

## 2026-05-15 sidecar performance review: SQLite-side candidates

Highest-priority incremental improvements if SQLite remains the publication backend while a custom generation backend is prototyped:

- Add a read-only/no-dirty shutdown fast path. If no capsule/candidate/runtime-root/intern/manifest state is dirty, skip temp table setup, canonical crosswalk construction, writeback SQL, and cleanup.
- Stop loading full SQLite capsule payloads at startup. Load metadata, hashes, sizes, and row identity first; fetch full payload bytes lazily only when a candidate survives lookup/proof checks.
- Avoid full payload reads for probe/result sidecars. External payload objects already have bounded sidecar reads; SQLite BLOB-backed capsules should match that behavior through incremental BLOB I/O or bounded `substr` reads.
- Add attribution counters before deeper SQL tuning: schema/pragma time, capsule metadata rows, payload bytes, candidate event rows, head-cache rows, runtime-root rows, SQL VM steps, full scans, sorts, autoindexes, full-capsule load reasons, and shutdown writeback stages.
- Prototype chunked `VALUES` or `carray` writeback only after the read/startup/dirty-fast-path work, because benchmarks currently suggest startup/read materialization dominates many hot paths.
- Audit `TraceCapsules` secondary indexes and covering indexes with real query plans before changing schema; extra indexes can easily improve reads while hurting cold writeback.

Adversarial constraint: none of these changes may treat sidecars, head caches, or projections as authority. They can only reduce candidate enumeration and byte materialization before the existing object/proof checks.

## 2026-05-15 benchmark checkpoint: run 902

Command used:

```sh
nix run .#eval-trace-bench -- generate --run-number 902 --num-commits 100
nix run .#eval-trace-bench -- runs --runs cold/900,cold/902,hot/900,hot/902
```

Run 902 was produced from the binary built before the latest authority-boundary hardening edits in this checkpoint.

Summary across 100 commits:

| run | mean wall | median wall | p90 wall | p95 wall | min | max | hits | misses | hit rate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| cold/900 | 6.834s | 1.239s | 18.765s | 19.840s | 1.066s | 47.434s | 581 | 119 | 83.00% |
| cold/902 | 8.270s | 1.270s | 22.537s | 24.511s | 1.216s | 25.309s | 551 | 149 | 78.71% |
| hot/900 | 0.957s | 0.947s | 0.983s | 1.011s | 0.900s | 1.306s | 700 | 0 | 100.00% |
| hot/902 | 1.074s | 1.074s | 1.098s | 1.103s | 1.037s | 1.114s | 700 | 0 | 100.00% |

Earlier completed cold means for context:

| run | cold mean | cold hits/misses | hot mean | hot hits/misses |
| --- | ---: | ---: | ---: | ---: |
| 153 | 5.591s | 579/121 | 0.979s | 700/0 |
| 154 | 5.517s | 579/121 | 0.970s | 700/0 |
| 155 | 5.471s | 579/121 | 0.928s | 700/0 |
| 156 | 6.440s | 579/121 | 3.167s | 700/0 |
| 157 | 5.303s | 579/121 | 1.514s | 700/0 |
| 158 | 5.396s | 579/121 | 0.939s | 700/0 |
| 159 | 5.947s | 581/119 | 1.396s | 700/0 |
| 900 | 6.834s | 581/119 | 0.957s | 700/0 |
| 902 | 8.270s | 551/149 | 1.074s | 700/0 |

Interpretation:

- Run 902 is a regression against 900 and against the earlier 153-159 cluster for cold wall time.
- The cold hit rate also dropped from 83.00% to 78.71%, so this is not just SQLite writeback overhead; precision/recovery changed for some commits.
- Hot remains sound and 100% hit-rate, but median/mean hot wall regressed by roughly 12% versus 900.
- The cold distribution remains bimodal. The major problem is not the 1.2s median path alone, but the number and cost of high-tail misses/outliers.

Next actions refined from this data:

- First rebuild and re-run targeted tests after the authority-boundary hardening edits.
- Then identify the 30 lost cold hits in 902 versus 900 using `eval-trace-bench logs`/stats before doing performance-only work.
- Treat no-dirty shutdown and lazy SQLite payload reads as worthwhile, but secondary to restoring cold precision.
- Continue the custom generation backend direction because even the better completed runs remain cold-mean dominated by miss/outlier behavior and hot mean remains close to a second.

## 2026-05-29 deep-research synthesis: fix authorization before storage

A deep-research pass (107 agents, 24/25 claims confirmed against primary sources: rustc
incremental dev guide, Salsa, Adapton, BSalC paper, RocksDB BlockBasedTable, Lucene codecs,
Cap'n Proto, Bazel BzlLoadFunction/Skyframe) was cross-referenced against the live tree. The
throughline reorders the plan: **the hot path is slow because it re-derives authority it could
compare in one fixed-size step, and the prior storage experiments failed because they optimized
payload access without first making authorization cheap. Fix authorization first; storage second.**

### Finding 1 — the unimplemented lever: compact certificate before `loadFullTrace`

Every mature incremental engine reuses a result by comparing a small fixed-size fingerprint and
stopping there: rustc compares a 128-bit `Fingerprint`; Salsa serves the memoized return value
when inputs are unchanged; the BSalC verifying trace stores hashes only. Our hot path does not do
this. The live sequence is:

```
verify()        store/verifier.cc:1909   lookupCurrentNode (cheap) -> loadTraceKeysAndHeader
                                          -> containsVolatileDep -> verifyTrace()
verifyTrace()   store/verifier.cc:1778   loadFullTrace(ea, traceId) at verifier.cc:1806  <-- EAGER
                                          then runPass1/runPass2 walk ALL deps
loadFullTrace() store/sqlite-trace-storage.cc:236   deserializeKeys + deserializeValues -> vector<Dep>
```

Every hot hit pays full key+value deserialization and a full dependency walk (~842k deps /
~53.5 MiB decoded; redesign-plan.md "skip decode-time sort" section) even though only
~38346/842775 current dep hashes miss the session L1 cache. The "full-trace verification memo"
candidate above is the embryonic form, still benchmark-pending.

**Action.** Make `verify()` authorize a hot hit by a fixed-size `FullTraceHash` compare against
session-L1-resident dep hashes, skipping `loadFullTrace` + the dep walk on certificate hit; decode
the result payload lazily only AFTER authorization. This is pure libexpr, needs no storage-format
change, and is transparent to all consumers — which also solves the long-standing "the
action-cache win lived above libexpr" problem (it serves `nix-eval-jobs` and every other
`libexpr` consumer, not just `nix eval --json`). Lazy payload WITHOUT lazy authorization is what
run 137 already tried and it did not beat SQLite, so both must be lazy.

### Finding 2 — precision is a projection problem, not a payload-size problem

Firewalling/early cutoff in mature engines comes from PROJECTION queries layered over a coarse
node, not from finer whole-trace payloads: rustc `no_hash` nodes are shielded by output-hashed
projection "firewall" queries; Salsa backdates by comparing tracked-struct fields one by one;
Adapton `force_map` projects a sub-field so changes to UNOBSERVED fields never enter the dirtying
phase at all. We already have the primitive (`StructuredProjection`, per-binding NixBinding AST
hashes, `fromJSON`/`fromTOML` leaf projections). Therefore the §8.2 over-invalidation issues
(whole-file `.nix` content dep, session-level lock-file invalidation, `callFunction` `#keys`)
should be attacked with MORE and FINER projections — not bigger traces. This is the precise reason
the v53 capsule rewrite failed (it preserved too much full source-specific payload). Note the
Adapton distinction: `force_map` prunes the dirtying phase, whereas our `markFileVerified`
subsumption is dirty-then-clean; a projection-only "prune dirtying" path is strictly cheaper.

### Finding 3 — the sound derivation-boundary proof, and an early-cutoff warning

The repeatedly-falsified by-name / current-package shortcut should be replaced by a Bazel-style
recursively-composed content-addressed transitive digest under a Skyframe-style strict
dependency-capture invariant: inputs may be read ONLY via registered dependencies (direct
filesystem reads silently break incremental correctness — exactly what the unsafe oracles did).
Warning from Bazel itself: a content digest gives soundness but NOT early cutoff — Bazel's
compiled-`.bzl` layer has no early cutoff because `BzlCompileValue` lacks a meaningful equality
relation, so any file change propagates fully. Early cutoff requires an explicit
output-equality/fingerprint relation at EACH layer; our `FullTraceHash` is that relation and must
be kept.

### Finding 4 — storage format is downstream of authorization, not a parallel track

SSTable/Lucene-`segments_N`/Git-pack startup (tiny generation pointer read with no log replay;
footer -> binary-search index -> bloom filter to jump to one block) and a zero-copy mmap payload
layout (Cap'n Proto/FlatBuffers — but only unpacked single-segment is truly parse-free; PACKED
mode and ~4 KiB page granularity are caveats) are confirmed design properties of other systems —
but NONE is benchmarked against this cache. Run 137 already showed lazy-payload-over-immutable-
objects does not beat SQLite without the authorization fix. Per `eval-trace-cache-findings.md`,
the custom generation backend should NOT return until the proof model first proves a win. The
lock-free reader / atomic-`CURRENT`-publication angle returned ZERO surviving verified claims and
is deferred until a custom store exists.

### Live-code correction (not visible to web research)

The `rearchitecture-proposal.md` §2.1 plan to make `TraceStorage` an abstract base with ~23
virtuals was implemented and then REVERSED: `store/trace-storage.hh:1-46` documents that the vptr
"measurably perturbed register allocation in the hot `verifyTrace` loop." It is now a
non-polymorphic `TraceStorageBase` + a `TraceStorageLike` C++20 concept. Consequence: in this hot
loop struct layout and field offsets are measurable, so the certificate fast path must preserve
`SqliteTraceStorage`'s tight layout. Do not re-propose abstracting the backend.

### Recommended sequence (each gated by the 10-commit correctness + benchmark gate)

1. Certificate-before-payload fast path (pure libexpr; finding 1).
2. Projection-coverage precision pass, including a `force_map`-style prune-dirtying path (finding 2).
3. Sound derivation-boundary transitive digest (Bazel/Skyframe), only after the gate harness is trusted (finding 3).
4. Custom immutable-segment store + zero-copy mmap + lock-free readers — only if 1-3 prove the proof model wins (finding 4).

## 2026-05-29 CORRECTION after thorough work-log read

The synthesis above was written from the distilled docs before reading the body of
`eval-trace-cache-work-log.md` (16k lines). A subsequent thorough read of the work log corrects
two of the four findings. The principles from the research are sound, but the prior work already
explored them and the measured results invert the priority order. **This block supersedes the
sequence above where they conflict.**

### Finding 1 (certificate before `loadFullTrace`) — already explored, every SOUND form refuted

Not a fresh idea. The mechanism is proven but does not pay for itself soundly on this workload:

- Unsound oracle (run 1015, `NIX_EVAL_TRACE_TRUST_CURRENT_NODE_FOR_BENCHMARK`) skips the walk and
  decodes only the result: **hot 0.678s vs 0.880s baseline**. So the dep walk genuinely is the hot
  cost for true exact hits — but the oracle "is unsound by construction because it trusts
  current-node metadata and does not prove dependency coverage."
- Cheap fingerprint (the `verifiedTraceIds` session memo) — runs 909, 980, 987, 1032, 1125 — did
  NOT move hot wall, because once the memo is in place `loadTrace.count` is already minimal (7) and
  the residual hot cost is result decode/materialization + scheduler/startup fixed cost, not the
  walk. Several of these regressed cold and were reverted.
- Authoritative fingerprint (`ExactReplayDescriptor`) — run 1042 persisted it: **cold 9.67s** vs
  6.06s baseline; run 1133 built it at record time: **first cold commit 38.1s**, reverted.
- Direct-serving / fusing the authority ahead of the decode — runs 1107/1108 (hot regressed to
  ~0.92s), 967/976/983/1104/1116 (hot improved ~20-50ms, cold wrecked every time). Explicit reason:
  "descriptor validation plus candidate lookup is too expensive to add ahead of the existing capsule
  decode unless direct payload eligibility is both common and cheaply known."
- The entire exact-replay-descriptor layer was **removed as inactive dead code in the 2026-05-27
  cleanup**.
- Soundness bar (run 993): a trace-row exact-replay class stored in manifest metadata "is an
  index/projection, not hash-bound proof material" — L1 residency is not proof of current dependency
  coverage. A `FullTraceHash`-vs-resident-state compare faces the same bar.

The ONLY un-refuted shape is the one the log names as future work: precompute hit eligibility into a
**cheaper per-current-node bit/index** so the fixed-size compare needs no descriptor reconstruction
on hot and no descriptor construction/persistence on cold, while still clearing the run-993 coverage
bar. Low priority regardless, since the residual hot cost is decode/startup, not the walk.

### Finding 2 (precision) — INVERTED. Prune coarse deps; do not add finer ones

The synthesis above said "add more/finer projections." The work log shows the **opposite**, decisively:

- Adding finer/fuller projection or sidecar data NEVER improved the benchmark: probe prefilter (918),
  bounded full-dependency probe projection (921), probe cap 65536 (925), naive StructuredProjection
  spot-checks (1034 — hot regressed to ~0.95s), v10 maintainer observed-keys (no change), Lead G
  widened `sourceContentHash` reuse (zero hits). Recurring measured reason: "the extra sidecar work
  costs more than the avoided full trace materialization."
- The single biggest lever in the entire log is the **observed-key / observed-directory proof**
  (v6->v11), which is exactly the Adapton `force_map` "prune the dirtying phase" idea: authorize
  serving when only UNOBSERVED sub-fields (attrset bindings, by-name directory children) changed.
  It drove cold 3.32s -> 1.88s and hot 0.88s -> 0.38s. It works by **slicing/removing coarse deps**
  (suppress construction-only `#keys` at `splice.nix`/`customisation.nix`; slice `DirectoryEntries`
  to the observed child names; ignore unobserved-binding churn), NOT by layering finer deps over a
  coarse node. The one finer *gate* added for soundness (v7 `#keys` suppression) cost 0.84s cold.

Two refinements (verified 2026-05-29 by reading the run passages directly, not just the agent summary):

- **The cold win is a tail-rescue, not a distribution shift.** `cold/46` vs `cold/47` improves the
  MEAN (1.92s vs 2.97s) but the MEDIAN (0.45s vs 0.47s) and p90 (~12.3s for both) are nearly
  identical. The gain comes entirely from converting ~9 catastrophic by-name-churn miss commits into
  proof hits (12 slow cold stores vs 21). It kills the worst cold outliers; it does not make every
  hit cheaper. Pitch it as outlier-elimination, not hot-path speedup.
- **The soundness boundary is narrower than "just wire it up."** The work log (lines 969-974) states
  the proof is sound ONLY for the observed key universe — NOT for `attrNames`, missing-attr
  suggestions, or complete negative membership — and it depends on successfully parsing simple
  non-recursive attrset files (falls back if parsing fails). So `attrNames` / negative-membership /
  recursive-attrset coverage is genuinely UNSOLVED, not merely un-wired.

Three caveats therefore keep this from being done: (a) it is currently **command-JSON-only** — above
libexpr, i.e. the disqualified layer per `eval-trace-cache-findings.md`; (b) it is gated behind
Nixpkgs path heuristics and env vars, so it is not yet **sound by construction**; (c) its coverage
stops at the observed-key universe. The real work is a producer-side "construction-only vs
result-visible shape observation" provenance fact that makes the unobserved-change proof sound by
construction, transparent to `libexpr`, and extensible past the observed-key boundary.

### Finding 3 (semantic derivation-boundary digest) — survives; the genuinely-open frontier

The Bazel/Skyframe recursively-composed transitive digest under strict dependency capture is the
SEMANTIC producer-side proof that the work log independently concluded it needs: the remaining cold
tail is whole-file `FileBytes` rejection on by-name `package.nix` files whose source changed but
whose demanded output is unchanged (runs 87/88), and the changed-package oracle (~run 10114) was
rejected because "a parent can depend on a package's non-output attrs or passthru... accepting
because only the own package drv is unchanged would serve stale parent output." Both point at the
same fix: a semantic drvPath/output-facet proof with a facet mask, not a file whitelist. This is the
research's strongest non-redundant contribution.

### Finding 4 — unchanged (storage downstream of the proof model).

### Corrected sequence

1. Make the observed-key / unobserved-change PRUNING proof (the log's #1 win) sound-by-construction
   and transparent to `libexpr` — lift it out of the command-JSON layer and off the path heuristics.
2. Semantic derivation-boundary transitive digest (Bazel/Skyframe) for the whole-file `FileBytes`
   tail — what runs 87/88 and the changed-package oracle both concluded they need.
3. Certificate-before-payload ONLY as the un-refuted per-current-node eligibility bit/index; low
   priority (residual hot cost is decode/startup, not the dep walk).
4. Custom immutable-segment store / mmap / lock-free readers — only if 1-3 prove the win.

## 2026-05-29 scoped research + code audit on the pruning frontier (GAP 1 / GAP 2)

Two scoped deep-research rounds (GHC/OCaml/Unison/Cargo interface hashing; Shake/Adapton/Bazel/Buck2
negative + enumeration deps) plus a primary-source-verified adversarial review and a direct code
audit. All claims below were cross-checked against primary sources and our own code; the prose
summaries of the research were NOT trusted (one round's synthesis output was malformed). Net: the
corrected sequence above does not change — these findings sharpen the soundness constraints on
finding 2 (the prune-the-dirtying-phase / observed-key proof).

### The observed-key / by-name slicing proof is MORE aggressive than any production build system

The decisive research result: NO surveyed production system (Bazel, Buck2, Shake) does
"only unobserved members of an enumerated set changed -> reuse" at the glob/directory-listing layer.
They all conservatively invalidate on ANY directory-entry-set change — Bazel `GlobValue` depends on
the entire `DirectoryListingValue` (verified: any add/remove, even of a non-matching file,
invalidates, because `DirectoryListingStateValue.equals` compares the whole sorted dirent set);
Buck2 DICE dirties both `ReadDirKey` variants of the parent on any entry change; Shake's
`getDirectoryFiles` rebuilds on any matched-set change. Every documented stale-result bug in those
systems traces to a missing dependency edge, untracked existence, or an under-reporting watcher —
never to intentional fine-grained reuse despite a known membership change.

Consequence for us: the work log's v6->v11 observed-key / by-name directory-slicing proof (which
reuses across changes to unobserved directory children / unobserved attrset bindings) has NO
battle-tested precedent. The orthodox-sound move is the conservative one (invalidate on any set
change) — and that is exactly the direction the work log measured as expensive (v7 `#keys` hardening
cost 0.84s cold). So our winning optimization is genuinely novel, and the entire soundness argument
reduces to proving a provenance distinction with zero external validation: that a
"construction-only" enumeration (a listing/keyset used only to build an index, never to drive a
result-visible decision) is distinguishable from a "result-visible" enumeration. That provenance
fact is the real work behind making the pruning proof sound by construction.

Failure-mode catalog our negative/keyset/enumeration deps must be tested against (each a real
documented bug): missing dependency edge (Bazel #6351); untracked existence of empty dirs (Bazel
PR #15774); watcher under-report (Buck2 watchman workaround); symlink-following set expansion
(Bazel #11875); child-state node independent of parent (Bazel #26863).

### GAP 1: GHC gives the soundness FLOOR, not a speedup — and a verified-firewalled local gap

GHC's per-declaration interface hash deliberately folds in everything a consumer could observe
(exported unfoldings, RULES, instances, fixity) and serializes the HASH of each referenced name so a
declaration's fingerprint changes if anything in its transitive closure changes (verified verbatim:
the user's guide states a change to A's inlining "may conceivably not change B.hi one jot" yet C
must still recompile). Orphan instances can't be located per-name and force a transitive,
module-level `mi_orphan_hash` (verified verbatim) — a real soundness bug (#12733) came from getting
that wrong. Lesson: this tells us the MINIMUM we must observe to stay sound (it CAPS how aggressive
pruning can be); it does not hand us a faster hot path. GHC folds MORE in to stay safe; our problem
is the opposite tail (we already have soundness via the FileBytes backstop and want LESS
invalidation).

Code audit of our analogue (`computeNixScopeHash` / `computeNixBindingHash` / `showForHash`): the
"`ExprVar::showForHash` hashes the variable name, not its referent" gap is REAL but NOT independently
exploitable — it is firewalled. `maybeRecordNixBindingDep` unconditionally co-records a `FileBytes`
dep on the same file alongside the binding hash; the structural override is per-file
(`FileIdentity = (sourceId, pathId)`) and re-verifies the covering dep, forgiving a FileBytes failure
only when a binding hash for that same file passes; and `findNonRecExprAttrs` is fail-safe (any
expression shape it doesn't understand yields no binding hash -> FileBytes-only). The genuine
residual gap is a referent reachable neither through the file's own bytes nor the enumerated scope
chain — i.e. an attribute injected by an overlay / `//` merge evaluated elsewhere. That is exactly
GHC's orphan-instance case, and it is the SAME gap already documented as OR-4 / parent-mediated value
change (§8.1, low severity). So GHC's orphan-hash / transitive-discriminator prior art applies to
OR-4, not to in-file `ExprVar`. Any future overlay-precision work needs a transitive discriminator,
per that prior art — but it is a precision/soundness item, not a hot-path speedup.

### Unison caveat

Unison's "content-address the definition, the interface/impl question vanishes" is over-applicable:
it works because Unison resolves names->dependency-hashes at codegen time (a closed, resolved graph).
Nix evaluation is lazy/dynamic — dependency hashes are not known until evaluation — so adopt only the
local idea (key on the content-hash of the observed sub-expression, which our NixBinding hash already
approximates), not the full transitive-baking model.
