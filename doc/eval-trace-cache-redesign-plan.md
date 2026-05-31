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

## 2026-05-30 — the arc converges: content-addressed trace identity RFC + consume-side proof

The architectural root cause (`plans/architecture-trace-model-vs-CA.md`: eval-trace flattens the
transitive dep closure because identity is tied to attr-path POSITION, while the build layer
references inputs BY HASH/EDGE) was reached independently THREE times this session — by the Lever 2
spike, the Lever 5 analysis, and the Tier-1 edge-recorder prototype — each blocked on the same wall:
non-attr-path values (derivations, scalar `outPath`s, function results) have no trace identity to
edge-reference. The session's closing work turns that diagnosis into a concrete, test-backed proposal
and proves the half of it that needs no hot-path change.

**The RFC: `plans/content-addressed-trace-identity-rfc.md`.** Give a small, principled set of
non-attr-path values a content-addressed trace identity (derivations, keyed by the EXISTING `drvPath
= hashDerivationModulo(inputs)`), so a value reached from N attr-paths records ONCE and is referenced
by N edges instead of flattening its whole closure into each consumer. This is the direct answer to
the "you can't cache hundreds of millions of intermediate values" barrier: the proposal caches FEWER
nodes (≈6,419 derivations / closure ≈ the same order as today's ≈10,397 attr-path nodes, a ~3,500:1
reduction vs the 22.9 M thunks), and the flattening is BOUNDED rather than eliminated — it stops at
the first content-addressed node below instead of spanning the transitive closure. The recursion that
would explode is cut at the identity boundary, exactly as `hashDerivationModulo` recurses on input
drvs and not on every value that produced them.

**Two real-evaluator corrections landed as tests (commit 694fbd827):**

- **`store/derivation-input-flattening.cc` (2 tests).** A derivation whose `args` embed
  `readFile(shared)`, consumed by two siblings via `.outPath`, flattens the shared `FileBytes` into
  BOTH consumer traces (the 607× mechanism is REAL for derivations) — *alongside* an inert
  `StorePathAvailability(.drv)` dep. The SPA dep verifies by `isValidPath(oldDrvString)` — a pure
  EXISTENCE check — so its answer is identical before and after the input changes; it CANNOT
  distinguish a v1-input from a v2-input. The load-bearing soundness carrier is the flattened
  `FileBytes`, NOT the SPA dep. (Diagnostic confirmed both consumers carry `[fileBytes]` AND
  `[storePathAvailability …-p.drv]`.) **Consequence:** the producer edge in the RFC must be a
  producer-TRACE-HASH edge (a `TraceValueContext`-style dep that folds in the derivation's input
  deps), NOT the existing SPA dep. This corrects an "already recorded" over-claim in an earlier RFC
  draft — only the trace-hash edge tracks inputs, and it does not exist yet (additive recorder work).

- **`store/ca-trace-key-routing.cc` (3 tests — the consume-side floor, commit c77064387).** Design A
  (a reserved `AttrVocabStore` namespace `"__ca:<drvHash>"` interned via `internName`, which accepts
  arbitrary `string_view`) is proven against the REAL store/vocab/verify pipeline with zero production
  change: **(R1)** a producer trace recorded under a synthetic CA key round-trips and verifies — the
  CA key is a first-class trace identity; **(R2)** TWO consumers at genuinely DIFFERENT attr-path
  positions both edge to the SAME CA producer via the existing `TraceValueContext` trace-hash edge and
  BOTH hit (the cross-scope sharing the earlier C2b test could not show with a literal vpath);
  **(R3)** mutating the shared producer's input changes its trace hash and invalidates BOTH consumers'
  edges. The edge is load-bearing, not vacuous — a probe-then-revert pass confirmed that corrupting
  the stored producer hash flips R2 to a miss. The existing recursive/memoized/cycle-broken
  `resolveTraceContextHash` (verifier.cc:243-278) carries the consume side; it needs NO new machinery.

**Net standing of this direction.** The consume side is DONE and proven (routing key + edge-verify
machinery work unchanged). The ONLY net-new piece is the RFC's §3b: a producer-trace boundary at
`derivationStrict` (open a `DepCaptureScope` around the input force, record a `CATraceKey(drvPath)`
producer trace, content-addressed-deduped). That is the single hot-path change, and it carries the
v53/vptr precedent warning (a hot-loop structure change that regressed and was reversed). It is
**NOT BUILT**: the go/no-go is the RFC §7 smallest-slice measurement — prototype ONLY the producer
boundary (no consumer edges yet) and measure (i) does it record ~6,419 deduped producer traces, (ii)
the per-derivation scope overhead on the Ledger-D benchmark, (iii) does cold storage drop. If the
scope overhead is acceptable, wire the consumer edge + the facet gate and re-run the soundness suite.

**Soundness floor is the test suite built BEFORE the design** (all non-vacuous, probe-verified):
`store/derivation-edge-soundness.cc` (C1/C2/C2b/C3 — edge invalidation, single-edge storage,
shared-producer amortization, the output-only facet hazard), `store/derivation-observation-facets.cc`
(the output-only gate is real AND detectable from the recorded dep set), `store/dep-flattening-
baseline.cc` (the 607× duplication to beat), `store/derivation-outpath-soundness.cc` (input change
invalidates via input deps, not bypassed by the SPA existence check), `store/ca-trace-key-routing.cc`
(this section), and `store/keyset-escape.cc` (cross-trace escape — a shape-carrying edge must
fail-closed). This relocates the README's abstract Option 3 / Lever 2 RFC into a concrete,
half-proven proposal with a defined hot-path measurement as its only remaining gate.

### 2026-05-30 follow-up: §3b recording-side composition proven (`store/ca-producer-boundary-recording.cc`)

Drove the REAL `DepRecordingContext` via `TestScopeAccess` — same calls a §3b recorder would make
(`pushScope`/`record`/`takeDeps`/`popScope`) — to prove the recording-side composition the consume-
side floor leaves open: capture a sub-computation's input-reads in an isolated nested scope, finalize
those isolated deps as a CA-keyed producer trace, and emit a SINGLE edge into the parent's stored dep
set (not the flattened inner deps). Six tests, all probe-then-revert verified for non-vacuity:

- **P1 ScopeIsolatedProducer_ParentEdgesNotFlattens** — nested scope captures inputs in isolation;
  parent records exactly own-dep + edge; verify routes through. Probe (inline inner deps): caught
  (size 4, innerLeak 2).
- **P2 ConsumerDepCount_IndependentOfClosureSize** — same composition with M=2 and M=50 input deps
  yields the SAME consumer dep count (1 own + 1 edge). The 607×→1 collapse made concrete at unit
  scale. Probe: caught (large=52 ≠ small=2).
- **P3 ProducerInputChange_InvalidatesConsumerViaEdge** — real-eval mutation propagates: producer
  trace hash changes → consumer's edge-stored old hash mismatches → invalidates. Probe (skip
  mutation): caught.
- **P4 NestedProducers_ChainInvalidatesFromDeepest** — depth-2 edge chain (C→A→B); mutation at B's
  deepest input invalidates C through both edges. The build-layer `hashDerivationModulo` recursion
  lifted to eval, exercised through the real verifier's `resolveTraceContextHash`. Probe: caught.
- **P5 RepeatedEdgeRecord_DedupesToOne** — 3 `ctx.record(edge)` calls dedupe to 1 stored dep. The
  recorder's `Dep::Key` dedup contract holds for edge deps identical to content deps. Probe (3
  different keys): caught (3 deps).
- **P6 ProducerSideTable_RoutesEdgeVsFlatten** — sketches the `Value*→{caKey, traceHash}` side-table
  interface a production §3b recorder needs to route between edge-emission (producer) and flatten-
  replay (non-producer) at force time. Drives both routes through the real context + verify; both
  halves resolve. Probe (disable gate): caught.

**Net: §3b's RECORDING composition WORKS in synthetic.** The shape that the production recorder
must produce (`pushScope` → capture inputs → finalize as CA producer trace → record edge in parent)
is fully reachable through the existing scope + store machinery with zero production change. The
composition is sound, idempotent, recursive-correct, and dedup-safe.

**Material blocker reached for the test-only phase.** What remains for production §3b is the
hot-path glue: a `Value*→(caKey, traceHash)` side table populated when a producer scope finalizes,
consulted in `replayMemoizedRange` to route between flatten and edge-emission at force time. This
requires modifications to `MemoReplayStore`, `replayMemoizedRange`, and the producer-scope opener
in `forceThunkValue` — all hot-path production code that carries the v53/vptr precedent warning
(reversed a hot-loop structure change in the past). Per the established discipline:
1. Encode the routing decision in the type system (not "the recorder remembers to consult the side
   table") — make illegal states unrepresentable.
2. Measure against the Ledger-D baseline (current-tree benchmark anchor at HEAD `9f7311129`).
3. Pass the 10-commit correctness gate (byte-identical `nix eval` output vs `--no-eval-trace`).

P6's interface sketch + P1-P5's correctness floor define exactly what the production prototype must
produce. The next slice is hot-path territory and exits the test-only zone.

### 2026-05-30 follow-up #2: §3b hot-path infrastructure landed (dormant, byte-identical)

The `Value*→(caKey, traceHash)` side table and the `replayMemoizedDeps` gate are now in
production code. Currently DORMANT — no caller registers into `producerMap` yet — but the
infrastructure is sound, probe-verified, and adds no observable change to real evaluation.

**Production additions:**
- `MemoReplayStore::producerMap` — `boost::unordered_flat_map<const Value *, ProducerEntry,
  ..., traceable_allocator<...>>` mirroring the lifetime/GC-hazard discipline of `epochMap`.
- `MemoReplayStore::registerProducer(value, caKey, traceHash)` — `insert_or_assign` for the
  BUG-7 GC-address-reuse hazard.
- `MemoReplayStore::lookupProducer(value)` — nullopt for unregistered Values; the gate's
  fall-through.
- `MemoReplayStore::clearProducerMap()` — wired into `clear()`.
- `TraceRuntime::replayMemoizedDeps` — gate fires BEFORE the existing `getReplayRange` lookup.
  When a producer is registered AND a recording scope is active, emits ONE
  `TraceValueContext` edge dep targeting the CA key and returns. Independent of `epochMap`
  (the producer can have no epoch entry). Bounded by `nrReplayBloomHits`-style fast paths.
- `nrReplayProducerEdges` counter — surfaced under `evalTrace.replay.producerEdges` in
  `NIX_SHOW_STATS` JSON.

**Hot-path cost when producerMap is empty (steady state today):** one
`boost::unordered_flat_map::find()` per `replayMemoizedDeps` call. Bounded by the same set
of forces that today pay the existing bloom+epochMap lookup. Effectively zero in benchmark
terms; will be remeasured once a §3b producer-trace boundary registers Values.

**Test floor (all probe-verified, all fail under deliberate-bug probes):**
- `dep/producer-side-table.cc` — 5 tests: register/lookup round-trip, distinct keys, BUG-7
  hazard, lookup-unregistered nullopt, lifecycle clear.
- `dep/replay-producer-gate.cc` — 4 tests: G1 registered Value emits edge not flatten; G2
  unregistered still flattens (control); G3 no recording scope → no-op; G4 no epoch range
  still emits edge.
- `dep/ca-producer-scope.cc` — 3 tests: end-to-end through `state.traceCtx` — boundary
  shape, replay-gate-emits-edge through production replayMemoizedDeps, producer-input
  mutation invalidates consumer via gate-emitted edge through `resolveTraceContextHash`.

**Soundness gates passed:**
- Sandboxed `.#checks.x86_64-linux.nix-expr-tests-run`: 1840 tests, 3 documented skips,
  0 failures.
- Canonical correctness check (per the test-discipline section above): `nix eval -f
  ~/nixpkgs/default.nix --system x86_64-linux asciidoc.nativeBuildInputs --json` produces
  BYTE-IDENTICAL output between `--no-eval-trace` and trace-on. The hot-path change
  introduces zero observable difference in real evaluation.

**Material blocker for the next slice — production-side §3b boundary:** wiring a CALLER
into `producerMap` requires an integration point that knows (a) when a Value is a
derivation result, (b) the drvHash for the CA key, and (c) the deps recorded during that
derivation's evaluation. The natural shape is wrapping `forceAttrs(args[0])` +
`derivationStrictInternal` in `prim_derivationStrict` with a sub-scope that captures the
input deps, persists them as a CA producer trace via the active `TraceBackend`, and
registers the result Value. That requires threading `EvalContext<Suspendable>` (or an
equivalent backend-access capability) into the primop, which is the same hot-path
session-integration question the prior spike's "second TracedExpr identity model" framed
differently. Approaches under consideration:
1. New lightweight "record-producer-trace" path that doesn't require `EvalContext<Suspendable>`
   threading (uses the active session via a TLS or capability lookup).
2. Lift the producer-scope OUT of `prim_derivationStrict` to `evaluateResolvedTarget` (which
   already has `EvalContext<Suspendable>`) and detect derivation results post-eval.
3. Defer until the §7 measurement decides whether the dormant gate's overhead is worth
   shipping the producer-side wiring at all.

### 2026-05-31 follow-up #3: production caller wired into prim_derivationStrict

The four-way investigation (commit `baa35bbf9` and prior) eliminated approaches (a) and
(c): (a) is killed by the C-extension API (PrimOp signature is frozen by external
plugins); (c) is structurally wrong (misses non-attr-path derivations, runs after deps
flatten, can't partition producer-vs-consumer reads). Approach (b) is the right shape and
has a direct precedent in `TraceBackend::recordRuntimeRoot`: `Certifier<BlockingTag>::
withProof` + `withExclusiveAccess`, no fiber color required, no `coroBlock` indirection.

**Sync API on `TraceBackend` (commit `baa35bbf9`):**
- `TraceBackend::recordSync(pathId, value, allDeps) → optional<{RecordResult, TraceHash}>`
  mirrors `recordRuntimeRoot`. One mutex acquisition fetches the trace + its content
  hash; the trace hash is what `replayMemoizedDeps`'s gate emits as the value of the
  TraceValueContext edge.
- `TraceBackend::verifySync(pathId)` — sibling sync entry point used by tests asserting
  cross-session persistence.
- `TraceSession::recordCAProducer(value, drvHash, innerDeps)` — primop-friendly
  facade. Computes `__ca:<drvHash>` CA routing key, persists producer trace, registers
  Value*→{caKey, traceHash} in `producerMap`. Returns false when no backend is bound.

Test surface — `dep/trace-session-record-ca-producer.cc`, 4 tests, all probe-verified:
R1 round-trip, R2 distinct drvHashes → distinct keys, R3 cross-session persistence, R4
input-mutation invalidates the persisted producer.

**The `prim_derivationStrict` hook (commit `13cf303d2`):**
After `derivationStrictInternal` returns, snapshot the epoch-log range
`[epochStart, epochEnd)` that grew during the call (= the producer's deps) and call
`session->recordCAProducer(v, drvPath, innerDeps)`. Active only when a TraceSession is
bound; `--no-eval-trace` short-circuits to no-op.

**Conservative shape — soundness floor unconditional.** The hook does NOT isolate the
producer's input-reads from the consumer scope. The deps still flow into the consumer's
existing scope as before. An aggressive shape (sub-scope isolation in the producer +
edge-instead-of-flatten in the consumer) was tried and reverted because it under-records:
any ambient-eval dep that legitimately flowed into the consumer scope but isn't tied to
the producer's content is lost. The conservative shape preserves the consumer's existing
scope shape exactly (zero precision loss); the amortization win comes from siblings
sharing the producer trace via the gate when re-forced.

**Production additions (since the dormant infra commits):**
- `TraceRuntime::snapshotEpochRange(start, end)` — public read accessor for an epoch-log
  slice. Used by `prim_derivationStrict` to capture the deps recorded during a single
  derivation's evaluation.
- `Verifier::verifyAttrSync(ea, pathId)` — sibling of `verifyAttr` taking an explicit
  exclusive-access capability instead of awaiting on the colored ctx. Used by
  `TraceBackend::verifySync` (test-only).

**Soundness gates passed:**
- Sandboxed `.#checks.x86_64-linux.nix-expr-tests-run`: 1847 tests, 3 documented skips,
  0 failures.
- Full `nix build -L .#default`: PASSES — including all 226 functional tests
  (eval-trace-core, eval-trace-deps, etc.).
- Canonical correctness: `nix eval -f ~/nixpkgs/default.nix --system x86_64-linux
  asciidoc.nativeBuildInputs --json` BYTE-IDENTICAL between `--no-eval-trace` and
  trace-on.
- `producerEdges = 0` in real eval: confirms the gate is correctly conservative. The
  gate fires only from `SiblingForceScope::commit` (sibling-shared values) — narrow
  scenario. The ~thousands of `derivationStrict` calls per closure all register
  producers; the SQLite store now holds a CA-keyed producer trace per derivation; the
  consumer-side scope shape is unchanged.

**Observation: the gate doesn't fire much in current workloads.** `replayMemoizedDeps`
is called only from `SiblingForceScope::commit`, which fires for explicitly
sibling-isolated thunks under a `SiblingReplayCaptureScope`. To benefit measurably from
the producerMap registration, either (a) more producer Values must end up in
sibling-share contexts, or (b) the gate must fire from a broader site (e.g., a
post-`derivationStrictInternal` pass that emits the edge for sibling consumers
specifically detected via a new mechanism). The amortization win is theoretically
present but quantitatively small in the current eval pipeline. Benchmarking on the
Ledger-D anchor is the next concrete step to decide whether the wiring is worth
keeping or whether the gate site needs widening.

**Async/off-thread recording — investigated, NOT pursued (reframed correction).** The
parallel investigation surfaced that `TraceBackend::record` is ALREADY off-thread via
`coroBlock(blockingPool, ...)` — what's synchronous is the eval thread's `syncAwait`
*wait*. Removing the wait (true fire-and-forget recording) is reachable in principle but
has a `traceId` back-write reconciliation hazard: the recorder writes
`expr.ensureLazy().traceId` after publish, and a later sibling's prefetch/replay reads
it. Per perf-lever L-E (cold cost is hash + serialize CPU, not flush I/O — flush is
already non-fsyncing under WAL+`synchronous=off`), making recording asynchronous would
not reduce CPU cost; the win would be eval/record overlap, bounded by the single
`storeMutex_` serialization. Not pursued in this slice.

### 2026-05-31 follow-up #4: bench measurement + adversarial fixes + DEFAULT-OFF

**Bench results (Ledger-D anchor, 100 commits closures.gnome, no `--with-stats`):**

| Phase | run-1 (pre-§3b) | run-2 (post-§3b) | Δ |
|---|---|---|---|
| reference (no-trace) | 6.47s mean / 6.43s med | 6.23s mean / 6.18s med | (noise) |
| cold | 3.72s mean / 1.11s med | 13.44s mean / 13.32s med | **3.6× slower** |
| hot | 0.96s mean | 13.47s mean | **14× slower** |

Soundness PASS (byte-identical) on all 100 commits. The §3b hook is correctness-safe
but a major perf regression. Hot is especially bad because: (a) cache-hit at the root
trace materializes a `closures.gnome.x86_64-linux` STRING, but (b) evaluating that
string still re-runs derivation thunks deep in `make-derivation.nix`, and (c) my hook
fires for each `prim_derivationStrict` call regardless of whether the consumer trace is
serving from cache. Result: hot loses ~12.5s/commit recording producer traces.

In-session dedup helped within one process (~6,419 derivations recorded once per fresh
process), but each `nix eval` is fresh — dedup doesn't span invocations.

**Adversarial-review fixes that landed during this slice (commits `e9d01d892`,
`8d393a1e9`, `b8ae91b12`):**

- `producerBloom` template params were misread: `<Bits, PointerAlignment>` not
  `<NumSlots, NumHashes>`. Fixed alignment from 4 to 16 (correct for GC-allocated
  `Bindings*` keys); FPR ~3% at n=6,419 in m=65,536 with k=2. Net win on the ~22M-call
  hot path even before dedup.
- `clearReplayIndex` deliberately does NOT touch `producerMap` — clearing producer
  bindings on a rollback-empty path would erase legitimate registrations. Documented
  the intentional asymmetry inline.
- `rollbackEpoch` doesn't scrub `producerMap` either; safe today because
  `prim_derivationStrict` registers only on the success path. Documented as a future-
  proof note: if registration moves before the success/throw decision, rollback must
  scrub `producerMap` analogously.
- **CRITICAL fix:** `producerMap` keyed by `Bindings *`, not `Value *`. With `Value *`
  keying the gate NEVER fired in real eval (`producerEdges = 0` despite 1129
  producers persisted). Root cause: `prim_derivationStrict(state, pos, args, v)`
  receives `&v` that is `&vCur` — `callFunction`'s STACK-LOCAL Value (eval.cc:2306).
  After `callFunction` returns, `vRes = vCur` (eval.cc:2530) COPIES the result to
  `&vRes`; `&vCur` becomes stale. `Bindings *` is stable across the copy because
  `Value::mkAttrs(b)` stores the pointer. With this fix, `producerEdges = 69` per
  asciidoc eval (gate fires).

**DEFAULT-OFF decision (commit `82713fd91`).** Hook gated on
`NIX_ENABLE_CA_PRODUCER=1`. Default behavior is identical to pre-§3b. The
infrastructure (producerMap + bloom + gate + recordSync + recordCAProducer + ~22 unit
tests) stays in tree as proven scaffolding. Re-enabling is one env-var away once the
cost profile changes — e.g., async/batched producer recording, or a hook that skips
re-recording when the caller's consumer trace is being warm-served, or a workload
where sibling-share contexts dominate so the gate fires frequently enough to amortize.

The §3b conservative shape is **structurally sound but cost-prohibitive on this
workload**. The §3b RFC's amortization claim (sharing producer verification across
sibling consumers) requires sibling re-forces of the producer Value within one
recording session — rare in current `nix eval` invocations against `closures.gnome`.
A workload like `nix-eval-jobs` with deep sibling attrsets might exercise it more.
Until measured, default-off is the right shipping state.

### 2026-05-31 follow-up #5: sibling-share workload MEASURED — gate does not invert (MATERIAL BLOCKER)

Follow-up #4 closed on a hypothesis: a `nix-eval-jobs`-shaped deep-sibling-attrset
workload "might exercise [the gate] more … until measured, default-off is the right
state." This is that measurement. It uses the existing Ledger-D binary
(`result/bin/nix`, §3b present, default-OFF) with no rebuild: the env var
`NIX_ENABLE_CA_PRODUCER=1` flips the hook, and two readily-available counters
decompose the result.

**Method (no rebuild — measured against the existing binary).** Evaluate
`map (n: python3Packages.${n}.outPath) [pkgs…]` over a growing package list, twice per
size (§3b OFF vs ON), each from a fresh isolated cache (`XDG_CACHE_HOME=$(mktemp -d)`),
`--impure` (the workload imports nixpkgs via `getEnv`). Two derived quantities:

- **P (producer records)** = `evalTrace.record.count(ON) − record.count(OFF)`. The
  consumer-trace count is identical across modes (the gate changes dep *content*, not
  the number of consumer traces), so the delta is exactly the producer traces §3b adds.
- **E (gate fires)** = `evalTrace.replay.producerEdges`.

The go/no-go is whether **E:P** climbs toward (and past) the `closures.gnome` baseline
of `614:6419 ≈ 0.096` as sibling count grows. The amortization thesis predicts P
plateaus (shared closure recorded once via `Bindings*` dedup) while E grows (each added
sibling re-forces the shared closure and fires the gate).

**Result — the thesis is falsified on this workload.**

| pkgs | rec OFF | rec ON | P (prod) | E (fires) | E:P |
|---:|---:|---:|---:|---:|---:|
| 1 | 2 | 2020 | 2018 | 65 | 0.032 |
| 2 | 3 | 2104 | 2101 | 65 | 0.031 |
| 3 | 4 | 2117 | 2113 | 65 | 0.031 |
| 5 | 6 | 3201 | 3195 | 105 | 0.033 |
| 8 | 9 | 4259 | 4250 | 118 | 0.028 |

| marginal step | ΔP | ΔE | ΔE:ΔP |
|---|---:|---:|---:|
| 1→2 | 83 | 0 | 0.000 |
| 2→3 | 12 | 0 | 0.000 |
| 3→5 | 1082 | 40 | 0.037 |
| 5→8 | 1055 | 13 | 0.012 |

E:P stays ~0.03 — **worse** than the 0.096 baseline — and the *marginal* ΔE:ΔP trends
toward zero. Adding scipy after numpy re-forced ~1935 shared derivations (ΔP=83 proves
`Bindings*` dedup works: numpy/scipy share most of their closure at record time) yet
fired the gate **0 additional times** (ΔE=0). Sibling sharing is large and real; the
gate is blind to it.

**Counter decomposition — WHERE it fails (not just that it fails).** The replay-path
counters (all ON-mode) localize the miss inside `replayMemoizedDeps`:

| pkgs | totalCalls | bloomHits | added | producerEdges |
|---:|---:|---:|---:|---:|
| 3 | 75,132 | 75,067 | 259 | 65 |
| 5 | 127,828 | 127,718 | 848 | 105 |
| 8 | 180,554 | 180,344 | 1,797 | 118 |

`totalCalls` (gate *reached*) grows ~50k per sibling batch — so re-forces DO reach the
gate. But `bloomHits ≈ totalCalls` at every row: ~99.9% of gate-entries fall straight
through the producer branch into `getReplayRange` (the **flatten/epoch** path), which
succeeds. The producer branch (`lookupProducer`) is reached and returns empty almost
every time. The re-forces take the existing Value*-keyed epoch-replay path, not the
producer path.

**Root cause — the keying split is structural, traced to `derivation.nix`.** Two
independent keyspaces that never meet on the re-force path:

- The **entry gate** keys on `Value *`: `mayHaveMemoizedDeps(v) = replayBloom.test(&v)`
  (eval.cc:1883), and `replayBloom` is set **only** by `recordThunkDeps(&v)`
  (memo-replay-store.hh:169). `registerProducer` sets `producerBloom`, never
  `replayBloom` (memo-replay-store.hh:228-232).
- The **producer lookup** keys on `Bindings *`: `producerKeyFor(v)=v.attrs()` →
  `producerMap[Bindings*]`. The producer is registered on the **`strict`** attrset —
  the raw `derivationStrict` result (`outputs`+`drvPath`+`type`, ~3 attrs).

But `src/libexpr/primops/derivation.nix:36-50` shows what consumers actually touch:
```nix
strict = derivationStrict drvAttrs;                 # ← producer keyed on THIS Bindings*
commonAttrs = drvAttrs // { … } // (builtins.listToAttrs outputsList) // { … };
in commonAttrs // {
    outPath = builtins.getAttr outputName strict;   # consumer reads a STRING off the //-wrapper
    drvPath = strict.drvPath;
}
```
A consumer (`numpy.outPath`) forces the **`commonAttrs // {…}`** wrapper — a *different*
`Bindings*` built by `//` (measured: the numpy package attrset has **76 attrs**, not 3)
— and ultimately reads `.outPath`, a **string** with no `Bindings*` at all. The `strict`
attrset is forced once when `derivation` is first applied, and **never re-forced by
siblings**. So `producerKeyFor(v)` on what siblings re-force never matches the registered
producer key. The gate cannot fire on sibling sharing **by construction of the wrapper**,
independent of workload depth. This is the same "derivation result is a `//`-wrapper, the
addressable identity is one layer below what consumers see" shape that the
`compositional-trace-dag-design.md` Tier-1 analysis circles — re-confirmed here as the
reason the *replay* gate (not just the *record* cost) is the binding constraint.

**Consequence for the re-enable conditions (corrects follow-up #4 condition c).** A
sibling-share-heavy workload does NOT rescue §3b: it is not that the gate fires too rarely
to amortize, it is that the gate is keyed to a value siblings never re-force. Async/batched
recording (condition a) and suppress-on-warm-served (condition b) would reduce the *cost*
of producer records but cannot create *benefit* — they make a near-zero-fire gate cheaper,
not more effective. **The CA-producer direction as currently keyed has no benefit channel
proportional to its cost on either measured workload.**

**What this does NOT rule out.** The architecture's *thesis* (edge-not-flatten) is
untouched; what is blocked is the specific keying (`strict`-result `Bindings*`). A viable
variant would have to register identity on the value consumers actually re-force — the
`.outPath`/`.drvPath` **string** (Tier-1's "scalar producer has no identity" wall from
`compositional-trace-dag-design.md`'s spike, now re-confirmed from the replay side) or the
`commonAttrs //` wrapper attrset. Both are larger than a hook re-key: the string case needs
the scalar-identity work the Tier-1 spike already found dead on `replayMemoizedDeps`
widening; the wrapper case needs the producer keyed on a `Bindings*` that doesn't exist until
*after* `derivation.nix` runs in the language layer, outside the `derivationStrict` primop.

**Verdict.** §3b stays default-OFF. The blocker is upgraded from "cost ≫ benefit on this
workload" (#4) to "**the replay gate is structurally keyed to a value that sibling consumers
never re-force; no measured workload inverts E:P, and the two candidate re-keyings are each
their own RFC-scale identity problem**". Infrastructure stays in tree as scaffolding; the
next real step is a re-keying design, not a cost optimization.

Reproduction: `benchmarks/eval-trace-bench/experiments/ca-producer-sibling-firerate.sh`
(isolated `XDG_CACHE_HOME` per run, `NIX_SHOW_STATS_PATH` capture, OFF/ON pairs,
`record.count` delta = P, `replay.producerEdges` = E). n=1 wall noise; counts are
deterministic across re-runs (n=3 reproduces P=2113 E=65 exactly).

### 2026-05-31 follow-up #6: re-keying feasibility — the consumed string is NOT identity-free (softens #5's wall)

Follow-up #5 closed by calling both re-keying candidates "RFC-scale," and asserted the
string case "needs the scalar-identity work the Tier-1 spike found dead." A code read of
the derivation-result construction (`primops.cc:2104-2115` + `mkOutputString` at
`primops.cc:218-231`) shows that assertion was **too strong** and must be corrected.

**The strings consumers read already carry the derivation's content-addressed identity —
in their string context, not as a trace identity:**
- `result.drvPath` is `mkString(drvPathS, { NixStringContextElem::DrvDeep{.drvPath = drvPath} })`
  (primops.cc:2105-2111). The drvPath store path IS the content address
  (`hashDerivationModulo`, memoized in `drvHashes` at primops.cc:2100-2101).
- `result.<output>` (e.g. `outPath`) is built by `mkOutputString` →
  `state.mkOutputString(…, SingleDerivedPath::Built{ .drvPath = makeConstantStorePathRef(drvPath),
  .output = o.first }, …)` (primops.cc:224-230). The output string's context carries the
  producing drvPath + output name.

So the value a sibling consumer actually re-forces/reads (`pkg.outPath`, a string) is **not**
identity-free in the way the Tier-1 `replayMemoizedDeps`-widening spike concluded. That spike
keyed on the *Value's trace identity* (the `TracedExpr`/materialize stamp), which scalars lack
— a true statement about *that* channel. It did not consider the **string-context channel**,
which carries `drvPath`/`SingleDerivedPath::Built` — exactly the content address §3a's CA key
(`__ca:<drvHash>`) is derived from. The identity the producer is keyed by and the identity the
consumed string carries are the SAME drvPath; they are simply attached to different things
(the `strict` `Bindings*` vs the output string's context).

**Revised feasibility of the re-keying.** The blocker is narrower than "RFC-scale scalar
identity":
- **Producer side is unchanged** — keep recording the CA producer trace keyed by `__ca:<drvHash>`
  (already built, already proven by `ca-trace-key-routing.cc` R1/R2/R3).
- **Consume side becomes a context-read, not a new identity scheme.** The edge could be emitted
  when a consumer's recording scope observes a string whose context contains a
  `DrvDeep`/`SingleDerivedPath::Built` referring to a drvPath that has a registered producer
  trace — emit `TraceValueContext(__ca:<drvHash>)` instead of flattening that observation.
- This moves the gate from "Value re-forced under `replayMemoizedDeps`" (which sees the wrong
  value) to "string-context observed at the coercion/selection boundary" (which sees the
  drvPath). The hook site is the open question, not the identity.

**The genuinely hard part that remains (this IS still a real design problem, not a quick fix):**
1. **Where is the context observed at record time?** A consumer reads `pkg.outPath` and then
   *does something* with the string (interpolates into a builder, passes as a buildInput). The
   drvPath context flows through string coercion (`coerceToContextObject`,
   eval-trace/CLAUDE.md "Provenance Publication Semantics"). The edge-emission site must be where
   that context is first consumed into the recording scope — likely the string-coercion /
   `ExprConcatStrings` boundary, not `replayMemoizedDeps`. Unverified which site, and whether it
   has an active recording scope + `TraceAccess::current()`.
2. **Facet soundness still binds.** The §3b facet gate (`derivation-observation-facets.cc`)
   showed `outPath` is output-only (records only `StorePathAvailability(.drv)` + system) so an
   edge drops nothing — GOOD. But a consumer that reads `pkg.drvPath` *and also* `pkg.meta`
   (a non-output facet) must keep the facet dep. The context-read edge must be additive to facet
   deps, never a replacement, unless the observation is provably output-only. This is the same
   gate, now enforced at a different site.
3. **The win is bounded by what flattening the context-edge removes.** #5 showed the gate as-built
   fires ~0 on the value channel. The string-context channel *would* fire on every `.outPath`
   read — but whether that nets out positive depends on (a) how many flattened deps an edge
   replaces (the producer's input closure, which IS the 607× — potentially large win) vs (b) the
   per-read context-inspection cost on the hot coercion path (the vptr-in-hot-loop hazard again).
   UNMEASURED. This is the real go/no-go and it requires a prototype, not just analysis.

**Net correction to #5's verdict.** The re-keying is NOT "its own RFC-scale identity problem" —
the identity already exists on the consumed string (drvPath in context). It IS "a hot-path
recorder change at the string-coercion boundary, gated by the existing facet rule, whose
cost/benefit is unmeasured." That is a prototype-sized question (the same shape as the original
§7 slice), not a foundational identity redesign. The #5 wall was real for the *value* channel and
is the reason the as-built gate fires ~0; it does not apply to the *string-context* channel, which
is the correct next thing to prototype. Soundness floor unchanged
(`derivation-edge-soundness.cc` + `derivation-observation-facets.cc` already pin the contract).

### 2026-05-31 follow-up #7: flatten-site discriminator MEASURED — confirms the coercion hook is flatten-replacing, not additive

Before trusting #6, an adversarial pass raised the make-or-break objection: #6 wants to hook
the output-string read (`coerceToContextObject`/`copyContext`) to emit an edge *instead of*
flattening. But if the shared input `FileBytes` flattens into the consumer at FORCE of the
derivation attrset (the RFC §1 "primop has no sub-scope, input-reads land in the consumer's
scope" mechanism), then the coercion hook fires DOWNSTREAM of where the FileBytes was already
copied — additive again, and #6 would be wrong in the same way #5's value-channel gate is.

Settled by measurement, not argument — a new discriminator test
(`store/derivation-input-flattening.cc::DrvInput_FlattenSite_ForceVsOutputRead`, real evaluator,
committed). Three sibling consumers over the SAME shared-input derivation `d`:

| consumer | expression | forces `d`? | reads output string? | recorded `FileBytes`? | recorded `.drv`? |
|---|---|:---:|:---:|:---:|:---:|
| cx | `d.outPath` | yes | yes (output) | **1** | 1 |
| cy | `d.drvPath` | yes | yes (drvPath) | **1** | 1 |
| cz | `builtins.attrNames d` | yes | **no** | **0** | **0** |

`cz` forces the derivation (adversarially confirmed: `attrnames` and `outpath` both evaluate
14 thunks — `derivationStrict` ran in both; the cz trace exists, it just carries no leaf obs) but
reads NO output string, and carries **neither** the input `FileBytes` **nor** the `.drv` dep. The
shared `FileBytes` lands in a consumer's trace **only when it reads an output string**.

**Verdict: the flatten is output-read-driven, NOT force-driven.** This is the opposite of the
adversarial worry and it CONFIRMS #6's hook placement: `coerceToContextObject`/`copyContext` is
exactly where the input observation enters the consumer's recording scope, so an edge emitted
there is **flatten-replacing, not additive**. (It also corrects a latent assumption from the RFC
§1 framing — forcing the derivation attrset does not itself flatten the input closure into the
consumer; the flattening is attached to consuming the output string's context, not to the force.)

**What's now settled vs still open for the prototype:**
- SETTLED: identity exists on the consumed string (#6); the flatten site is the output-string
  coercion, reachable and flatten-replacing (#7); the facet gate is mechanically checkable
  (`derivation-observation-facets.cc`); soundness floor pinned (`derivation-edge-soundness.cc`,
  `ca-trace-key-routing.cc`).
- STILL OPEN (the genuine go/no-go, needs the prototype): (1) is `TraceAccess::current()` live at
  the coercion site during cold recording? (2) the per-read context-inspection cost on the hot
  coercion path vs the flattening it removes — the same vptr-in-hot-loop hazard, UNMEASURED;
  (3) the edge must remain additive to any *non-output* facet dep the same consumer records.

The blocker for §3b is now precisely located and is prototype-sized, not RFC-scale: a
recording-side edge at the string-coercion boundary, gated by the output-only facet rule, whose
hot-path cost is the one unmeasured number that decides it.

### 2026-05-31 follow-up #8: RETRACTION — #7 was confounded; the flatten is force-of-args-driven, so #6's coercion hook is ADDITIVE not flatten-replacing

An adversarial pass on the "finalized" #6/#7 proposal found a confound in #7's own test and
**refutes the hook-site claim**. This entry retracts the #6/#7 conclusion. The honest verdict
reverts toward #5: the re-keying is genuinely hard, not prototype-sized at a coercion boundary.

**The confound.** #7 concluded "flatten is output-read-driven" from a single discriminator
`cz = builtins.attrNames d` measuring `FileBytes=0`. But cz differs from cx/cy (`d.outPath`/
`d.drvPath`) on **two** axes at once: (a) it reads no output string, AND (b) `attrNames` does not
force the derivation's `args` at all (it needs only the attrset keys). cz=0 is consistent with
*either* cause. #7 picked (a) and called the hook site confirmed. That was reasoning from a
confounded test — exactly the failure mode these adversarial passes exist to catch.

**The confound-split (committed, real evaluator —
`DrvInput_FlattenSite_ForceVsOutputRead`, extended with cw/cv):**

| consumer | expression | forces args? | reads output string? | `FileBytes`? |
|---|---|:---:|:---:|:---:|
| cx | `d.outPath` | yes | yes | 1 |
| cy | `d.drvPath` | yes | yes | 1 |
| **cw** | `deepSeq (removeAttrs d [outputs]) null` | **yes** | **no** | **1** |
| cz | `builtins.attrNames d` | no | no | 0 |
| cv | `seq d null` | no (WHNF) | no | 0 |

**cw is decisive and was missing from #7.** It forces the derivation's args deeply but reads NO
output string — and STILL carries `FileBytes`. So the input flatten is **force-of-args-driven**,
not output-read-driven. cz=0 was because `attrNames` doesn't force the args (identical to cv), not
because it skipped an output read. #7's verdict is **wrong**.

**Consequence — #6's hook site is refuted, and the idea collapses into the already-reverted
aggressive shape.** The FileBytes is recorded into the consumer's scope during
`forceAttrs(*args[0])` inside `prim_derivationStrict` (primops.cc:1602). A `coerceToContextObject`
edge at a later output-string read fires *after* that recording already happened — it is
**additive, not flatten-replacing** (the exact trap #6 talked itself out of). The ONLY place an
edge could *replace* the flatten is a dep-capture sub-scope around `forceAttrs(*args[0])` +
`derivationStrictInternal` — which is precisely the **"aggressive shape" already tried and reverted
as unsound** (primops.cc:1554-1568: it under-records ambient-eval deps that legitimately flow into
the consumer scope but aren't tied to the producer's content). There is no new flatten-replacing
site downstream of the args-force.

**Also corrects #6's narrower sub-claim.** #6 noted the consumed string carries the drvPath in its
context (`DrvDeep`/`Built`) — that fact is TRUE and unretracted. What is retracted is the inference
that an edge keyed off that context, emitted at the read, could replace the flatten. It can't: the
flatten is upstream of the read, at args-force.

**Net verdict (reverts to #5, now fully grounded).** The CA-producer direction's blocker is NOT a
prototype-sized coercion hook. To replace (not duplicate) the flatten requires isolating the
derivation's input-reads at the args-force boundary, which is the aggressive shape — and that is
unsound as-built because it cannot distinguish the producer's own input-reads from ambient-eval
deps that legitimately flow into the consumer. Making it sound requires partitioning those two
classes of dep at record time — which is the genuine, unsolved, RFC-scale problem. #5's "RFC-scale"
framing was right; #6/#7 were an over-optimistic detour produced by a confounded test.

**Process note.** #6 and #7 were committed as findings and are now partially retracted by #8. The
test (`DrvInput_FlattenSite_ForceVsOutputRead`) is KEPT — its confound-split matrix is the durable
artifact and a regression guard for the force-of-args attribution. The lesson: a one-variable
discriminator that actually varies two hidden axes will manufacture a clean-looking but false
verdict; split every axis before concluding.

### 2026-05-31 follow-up #9: consumer-sharing measurement — RETRACTED (confounded by thunk memoization)

> **RETRACTED 2026-05-31 — see follow-up #10 for the corrected measurement.** The probe below
> hooked `prim_derivationStrict`, which runs ONCE per derivation (thunk memoization). The 2nd+
> consumer of a shared derivation gets the already-evaluated Value and never re-runs the primop,
> so the probe recorded only the FIRST forcer of each derivation and was BLIND to the
> memoized-thunk sharing the RFC targets — exactly the 607× shared-closure case. Controlled test:
> one derivation consumed by three sibling scopes (`{a=d.outPath; b=d.outPath; c=d.drvPath;}`) was
> reported as N=1. So "85.9% singly-consumed → STOP" is a MEASUREMENT ARTIFACT, not a workload
> property; the verdict is INVALID. The faithful site is the per-consumer re-force
> (`replayMemoizedDeps` epoch-range replay of a memoized derivation-result Value). The §7.7b
> go/no-go is OPEN again. Original (wrong) write-up retained below for the audit trail — DO NOT
> CITE its numbers. Same confound class as #7 (a clean-looking metric that measures the wrong
> event); the lesson recurs: validate the probe against a controlled case BEFORE trusting it.

`plans/derivation-producer-partition-rfc.md` (the forward design through 18 adversarial
passes) reduced its blocker to one quantitative go/no-go (RFC §7.7b): how many DISTINCT
attr-path consumer traces force each producer drvPath? If ~1, the producer-partition design
adds per-producer overhead (record + edge + routing row) without amortizing, reducing to §3b's
net loss. RFC §8 step 0 specified measuring this BEFORE any prototype. This is that measurement.

**Instrumentation (measurement-only, env-gated, no behaviour change):**
`src/libexpr/eval-trace/drv-sharing-probe.{hh,cc}`, gated on `NIX_MEASURE_DRV_SHARING=1`
(no-op otherwise). A `ConsumerScope` RAII guard in `TracedExpr::evaluateResolvedTarget`
pushes the consumer's `pathId`; `prim_derivationStrict` calls `recordProducer(drvPath)` which
accumulates `drvPath → set<consumer pathId>`; dumps a histogram at process exit. Faithfulness
validated pre-build (pass #18): on the mapAttrs-over-python3Packages workload each package is a
distinct attr-path consumer trace, so a shared infra derivation forced under many package
scopes registers many distinct consumers — the infra-sharing signal §7.7b needs. (closures.gnome
is the WRONG workload — one coarse string-leaf scope — so this is measured on python3Packages.)

**Result (clean run, full `mapAttrs (n: v: v.outPath) python3Packages`, exit 0):**

| metric | value |
|---|---:|
| distinct producer drvPaths | 35,383 |
| mean distinct consumers / producer | **1.33** |
| singly-consumed (N=1) | **30,409 (85.9%)** |
| multiply-consumed (N>1) | 4,974 (14.1%) |
| max consumers for one producer | 135 |

Weighted: of 46,980 (producer,consumer) flatten-pairs, only ~35% are on shared (N>1)
producers. So under the RFC, ~86% of producers are PURE ADDED COST (producer-trace record +
consumer edge + routing row vs just the consumer's flattened closure today), while the design
could collapse at most ~a third of the flattening even granting the heavy tail full weight.

**VERDICT: STOP — the record-time producer-partition direction does not pay off**, on its OWN
most-favorable workload (python3Packages, the 607×-flattening case; worse on closures.gnome).
The build-layer "hashDerivationModulo amortizes across hundreds of consumers" analogy does NOT
lift to eval: derivations are overwhelmingly singly-consumed PER EVAL. The mean of 1.33 is far
from the N≫1 the design needs. This is the §8-step-0 kill-switch firing as designed —
empirical, not a soundness failure (soundness was CLOSED).

**Caveat (honest):** the proxy counts distinct consumer pathIds, not closure-byte volume per
consumer. A few huge-closure shared producers (stdenv at N=135) save disproportionately, so the
flatten-pair fraction slightly under-credits the tail. But 85.9% singly-consumed dead weight is
decisive regardless of tail weighting.

**What survives:** ALT-4 (verify-time fragment sharing, RFC §7.8) is NOT killed by this — it
keeps conservative recording and dedups at verify, so it adds NO per-singly-consumed-producer
overhead. If the eval-trace perf effort continues, ALT-4 is the better-motivated branch; the
record-time producer edge is measured not-worth-building. Probe + soundness scaffolding stay in
tree. Reproduce: `NIX_MEASURE_DRV_SHARING=1 NIX_MEASURE_DRV_SHARING_PATH=/tmp/r.txt nix eval
--impure --json --expr 'builtins.mapAttrs (n: v: (builtins.tryEval (v.outPath or "")).value)
(import <nixpkgs> {}).python3Packages'`.

### 2026-05-31 follow-up #10: consumer-sharing RE-MEASURED correctly — the RFC's sharing premise HOLDS (unit scale, decisive)

Follow-up #9's probe was retracted (confounded by thunk memoization — it hooked the
one-time `prim_derivationStrict`). Re-hooking it at `replayMemoizedDeps` ALSO failed a
controlled-case validation (still reported a 3-consumer-shared derivation as N=1),
because the per-consumer event re-forces the OUTPUT STRING, not the derivation
attrset — three confounds, all from trying to count via the wrong Value/site. The
probe was REMOVED (confounded instrumentation is worse than none).

The faithful measurement is a UNIT test through the real `makeCache`/`TracedExpr`
consumer-trace machinery (where each consumer is a genuine attr-path trace and the
materialize path replays a memoized derivation's dep range into each consumer's
scope): `dep-flattening-baseline.cc::SharedDrv_SameOutPath_BothConsumersFlattenInputClosure`.

Shape: one SHARED derivation `d` (args embed `readFile(shared)`), consumed by two
siblings BOTH reading the SAME `d.outPath` (d forced once, memoized). Question: does
the second consumer's trace ALSO carry d's input closure, or does memoization mean
only the first flattens?

**MEASURED: cxHasFile=1, cyHasFile=1** — BOTH consumers independently carry the shared
derivation's input `FileBytes` (and the `.drv` dep), even reading the identical output
off a once-forced derivation. So **the 607× consumer-sharing the RFC targets is REAL**:
a shared derivation's input closure flattens into EVERY consumer trace via
`replayMemoizedRange`, not just the first forcer. The §7.7b go/no-go premise (producers
are consumed by N>1 distinct consumers, and each re-flattens) HOLDS at unit scale,
decisively.

**Net correction to #9:** the STOP was a measurement artifact, fully retracted. The
sharing exists; the direction is NOT killed. What remains genuinely open (and is the
real §7.7b/§6 work) is the WORKLOAD-SCALE distribution + the hot-path cost — i.e. how
many consumers share each derivation across a full python3Packages eval (the unit test
proves the MECHANISM, not the population), and whether the args-force sub-scope's
overhead is acceptable. Those still need measurement, but they are no longer gated
behind a false STOP.

**Lesson (the third confound in this arc, now costly):** a measurement probe must be
validated against a CONTROLLED case with a known answer BEFORE its verdict is trusted —
the #9 probe, the replayMemoizedDeps re-hook, and the original prim_derivationStrict
hook all looked plausible and all measured the wrong event. The unit test through the
real consumer-trace path is faithful because it inspects STORED per-consumer traces, not
a probe at a guessed site. Prefer "inspect the artifact the system actually produced"
over "instrument the event I think corresponds to it."

**What stays in tree:** the decisive unit test (regression guard for the sharing
mechanism). The confounded probe was removed.

### 2026-05-31 follow-up #11: workload-scale derivation-sharing FIRMED from already-decoded artifacts — N≫1 robustly (cheap path)

Follow-up #10 proved the sharing MECHANISM (unit test: a shared derivation flattens into
every consumer). This firms the POPULATION at workload scale WITHOUT a new probe or run —
by reasoning over the architecture doc's already-decoded python3Packages numbers (decoded
from stored `keys_blob`s, the faithful artifact-based method, not a guessed-site probe).

Inputs (architecture-trace-model-vs-CA.md, decoded python3Packages cold DB):
- 36.5M flattened deps, ~60K distinct atoms (the 608× duplication), ~10,397 traces.
- StorePathAvailability (.drv-existence) = 34.9% of deps; avg 3,376 deps/trace.

Derivation: SPA deps ARE the per-derivation `.drv` references. So:
- per trace: ~3,376 × 0.349 ≈ **1,178 distinct derivations referenced** (distinct because
  within-scope `seenDeps` dedup makes each `.drv` key appear once per trace — verified).
- total derivation-flatten-instances across all traces: 36.5M × 0.349 ≈ **12.7M**.
- mean consumers per derivation = 12.7M / (distinct derivations).

The one unpinned input is the distinct-derivation count for python3Packages. The estimate is
INSENSITIVE to it across the whole plausible range (adversarially bounded):

| distinct derivations | mean consumers/derivation |
|---|---:|
| 6,419 (closures.gnome analogue) | ~1,984 |
| 20,000 | ~637 |
| 60,000 (impossible upper bound — ALL distinct atoms = derivations) | ~212 |

Even the impossible worst case gives **mean ~212 consumers/derivation**; realistically 200–2,000.
**N≫1 robustly** — three orders of magnitude above the confounded probe's false "1.33." The
RFC §7.7b sharing premise is confirmed at BOTH unit scale (#10, mechanism) and workload scale
(#11, population). The SPA (`.drv`-existence) deps that drive this are genuinely per-derivation
and shared N≫1 — confirmed by the unit test (the SPA dep flattens into every consumer).

> **ADVERSARIAL-PASS CORRECTION (2026-05-31, on #11 itself — Attack B).** The original #11
> draft said a derivation edge "would collapse the 608× — the duplication is overwhelmingly
> derivation-closure sharing." That OVERSTATED the benefit by conflating two distinct claims:
> (i) "derivations are shared N≫1" (ESTABLISHED — the SPA deps prove it) and (ii) "a derivation
> edge removes the 608×" (NOT established). A derivation edge only removes deps that are part of a
> DERIVATION's flattened closure. The arch-doc dep-kind split (sampled, n=1) is StructProj 48.4%,
> SPA 34.9%, FileBytes 7.0%, DerivedStorePath 4.8%, ImplicitStruct 4.3%. SPA (34.9%) is
> per-derivation → removed. But **StructProj is the PLURALITY at 48.4%, and whether it is
> derivation-closure (removed by an edge) or the CONSUMER's own structured reads (e.g. `fromJSON`
> on a package-set, NOT removed) is UNVERIFIED.** So the fraction of the 36.5M flattening a
> derivation edge actually removes is **35–90%, unpinned** (35% = SPA only; 42% = +FileBytes;
> 90% = if StructProj is also derivation-closure), NOT ~100%. Determining which requires decoding
> real python3Packages StructProj keys (needs a python3Packages DB / generate run — not cheap).
> So the BENEFIT MAGNITUDE is a SECOND unmeasured number, alongside the hot-path cost. N≫1 sharing
> is unaffected; only the "how much does the edge save" claim is corrected.

**Method note (the lesson applied):** the sharing number came from re-deriving over
FAITHFULLY-DECODED stored artifacts (the arch doc's blob decode), NOT a new live probe — after
three probe confounds, the artifact-based path is the trustworthy one. It needed no new build or
run. (And this adversarial pass shows even the artifact-derived number needed scrutiny: the
arithmetic was right but the INTERPRETATION — SPA-sharing ⇒ edge-removes-607× — over-reached.)

**What this resolves and what it does NOT.** RESOLVED: the sharing premise (§7.7b) holds, at
scale (N≫1). NOT resolved: (1) the args-force sub-scope HOT-PATH COST (§6); (2) the BENEFIT
MAGNITUDE (what % of flattening a derivation edge removes — 35–90%, gated on what StructProj is).
Both need work touching real python3Packages data / the hot path (RFC §8 step 3 prototype). The
cheap analysis took sharing as far as artifacts allow and surfaced that benefit magnitude is a
separate open question; it did NOT establish the edge collapses the whole 607×.

### 2026-05-31 follow-up #12: §8-step-3 prototype, part 1 — benefit-magnitude probe (dep-kind breakdown)

Following the user's go-ahead to build the §8-step-3 prototype + measure the two open
go/no-go numbers (hot-path cost #6; benefit magnitude — the 35–90% StructProj question from
#11's correction). This is PART 1: the benefit-magnitude probe (lower-risk; reads the dep
range the conservative hook already snapshots, no hot-path restructure).

**Instrumentation:** `src/libexpr/eval-trace/drv-benefit-probe.{hh,cc}`, env-gated
`NIX_MEASURE_DRV_BENEFIT=1` (no-op otherwise; independent of NIX_ENABLE_CA_PRODUCER). At
`prim_derivationStrict` finalize, tallies the dep-KIND histogram of the epoch-log range that
grew during the call — the derivation-closure deps a producer-edge would remove from the
consumer. Dumps at atexit.

**Validated against known-answer cases BEFORE trusting (the recurring lesson):**
- Single derivation reading one file: range = {fileBytes×1, storePathAvailability×1} — exactly
  the input file + the `.drv` dep. Faithful.
- NESTED derivations (outer has an inner derivation as buildInput): 2 derivations, total 6 deps,
  outer's range (max=4) INCLUDES inner's closure (inner forced inside outer's window). So ranges
  NEST/overlap — outer's range double-counts inner's deps.

**Interpretation caveat (from the nested-validation):** the per-derivation ranges OVERLAP
(an outer derivation's range contains its nested derivations' ranges), so the ABSOLUTE total
across all ranges is inflated by nesting and must NOT be summed to "total flattening removed."
What IS faithful is the per-KIND fraction (what kinds of deps populate derivation closures) —
which is exactly what the §0.5/#11 benefit-magnitude question needs: is the plurality of a
derivation's flattened closure FileBytes/SPA (derivation-closure, edge-removable) or StructProj
(possibly consumer-intrinsic)?

python3Packages measurement: IN PROGRESS (next follow-up records the breakdown + verdict).

### 2026-05-31 follow-up #12 part 2: benefit-probe RESULT + an adversarial self-correction (the "benefit" it measures is narrower than benefit)

python3Packages run (52,184 derivationStrict calls, complete — atexit dump fired, eval exited
clean). Per-kind breakdown of deps captured INSIDE derivationStrict ranges:

| kind | % | edge-removable? |
|---|---:|---|
| storePathAvailability | 43.6% | yes (the `.drv` deps) |
| structuredProjection | 40.3% | (see correction) |
| derivedStorePath | 6.8% | yes |
| fileBytes | 6.7% | yes |
| implicitStructure | 2.4% | yes |
| (rest) | <0.1% | — |

mean 16.4 deps per derivationStrict range; max 10,695 (a top-level derivation whose range nests
all its transitive derivations).

**What this DOES establish (refutes #11's specific fear):** #11's correction worried StructProj
(~48% arch-doc-wide) might be CONSUMER-INTRINSIC (the consumer's own `fromJSON`, NOT removable by
a derivation edge). This shows StructProj appears at 40.3% INSIDE derivationStrict ranges — i.e.
it IS substantially part of derivation closures (structured reads during derivation construction),
not purely consumer-side. So the "StructProj is unremovable" pessimism is not supported.

**ADVERSARIAL SELF-CORRECTION (caught before recording a circular claim):** it is TEMPTING to read
this as "the edge removes ~100% of the flattening (everything in the range)." That is CIRCULAR —
the range IS what the edge removes, by the probe's definition. The actual BENEFIT is
(flattening removed from CONSUMER traces) / (total consumer flattening), and this probe measured
the DERIVATION-RANGE composition, NOT the consumer-trace composition. They are different objects.

**The number that should give pause:** mean 16.4 deps/derivation-range vs the arch doc's ~3,376
deps/consumer-trace — a ~200× gap. A consumer trace's 3,376 deps are NOT one derivation's 16; they
are the union of MANY derivations' ranges (a package transitively forces hundreds of derivations)
plus the consumer's own reads. So benefit = (sum of the consumer's referenced derivation-ranges,
deduped to edges) / 3,376. Whether that is most of the 3,376 (high benefit) or a minority (low
benefit) is NOT answered by this probe — it needs correlating which derivation ranges a consumer
trace actually flattens. UNMEASURED.

**Honest status of benefit magnitude:** narrowed, not resolved. #11's worst-case fear (StructProj
unremovable) is eased, but the consumer-trace removable FRACTION — the real benefit number — still
needs a measurement that correlates derivation ranges with consumer-trace contents (or, more
simply, the prototype that actually emits edges and measures the consumer-trace size delta).
The clean way to get it is the EDGE prototype itself (measure consumer trace dep-count with vs
without edges), which folds benefit and the part-2 hot-cost question into one build.

### 2026-05-31 follow-up #13: benefit magnitude MEASURED (both halves) — hard lower bound 37%, plausibly ~99% on this workload

Added the benefit DENOMINATOR (consumer-trace dep-kind tally at `evaluateResolvedTarget`
finalize) to the probe and re-ran full python3Packages. Both halves now complete (consumer
total 36,457,312 deps over 11,062 traces — matches the arch doc's 36.5M exactly → complete run).

| kind | derivation-range % | CONSUMER-trace % |
|---|---:|---:|
| storePathAvailability | 43.6% | 36.8% |
| structuredProjection | 40.3% | 45.9% |
| fileBytes | 6.7% | 7.3% |
| derivedStorePath | 6.8% | 5.8% |
| implicitStructure | 2.4% | 3.6% |
| (dir/existence/env/nar/raw/parentSlot) | <0.1% | <0.6% |

**Apparent-contradiction resolved (adversarial check):** the summed derivation-range total
(857K) is only ~2.4% of the consumer total (36.5M), which naively reads as "derivations explain
2.4% of flattening" — CONTRADICTING the benefit thesis. Resolution: 857K = per-`derivationStrict`
OWN-range deps (52,184 calls × mean 16.4); a consumer flattens the UNION of its whole transitive
derivation closure (~3,296 deps via `replayMemoizedRange` accumulation up the tree), NOT one
16.4-dep range. So drv-range-total is NOT the benefit numerator and is not comparable to the
consumer total. The reliable signal is the per-KIND MATCH between the two breakdowns.

**Benefit magnitude (honestly bounded):**
- **HARD lower bound 36.8% (MEASURED, not inferred):** consumer SPA deps = 36.8% are
  `.drv`-existence deps; a `.drv` dep exists IFF a derivation was referenced, so ≥36.8% of consumer
  flattening is DEFINITELY derivation-attributable and edge-collapsible. (Note: lands right at #11's
  feared low end — but now measured, not a coincidence.)
- **Plausibly ~99%:** the consumer and derivation-range KIND mixes nearly match (if consumer
  flattening were dominated by consumer-OWN reads, the mix would differ — it doesn't). And the
  workload-specific argument: the consumer expr is `v.outPath` (pure derivation access, does NO
  `fromJSON`), so the 45.9% StructProj is overwhelmingly derivation-closure (mkDerivation's
  structured reads), not consumer-own. Non-removable kinds (dir/existence/env/nar/raw/parentSlot)
  total only ~0.6%.

So #11's speculative 35–90% band is REFINED to **a measured hard floor of 37% and a workload-justified
ceiling near 99%** for the python3Packages-outPath shape. **Benefit is HIGH on this workload.**

**Caveat (workload-specificity, stated honestly):** the ~99% upper rests on the consumer doing no
own structured reads — true for `v.outPath` mapAttrs, NOT general. A consumer that `fromJSON`s its
own config would have consumer-own StructProj that an edge does NOT remove, pulling benefit toward
the 37% floor. So: benefit is 37%–99%, high for the canonical nix-eval-jobs shape, lower for
consumers with heavy own-structured-reads. The FLOOR (37%) is general; the ceiling is shape-dependent.

**Status of the two go/no-go numbers:** benefit magnitude = RESOLVED (37% floor, ~99% on the target
workload). Remaining: the args-force sub-scope HOT-PATH COST (#6) — still needs the actual sub-scope
build + Ledger-D timing. That is the last open number.

### 2026-05-31 follow-up #14: hot-path COST measured — scope-structural ≈ 11% (after a 23× proxy artifact was caught + corrected)

Built the per-derivation sub-scope (the aggressive shape's structural requirement; the
vptr-in-hot-loop hazard the RFC flags) env-gated at prim_derivationStrict, to measure its cost.

**First proxy was a 23× ARTIFACT — caught before recording.** Initial design: open the
sub-scope, capture the derivation's deps, RE-RECORD them into the consumer (so net behaviour
is soundness-neutral — verified byte-identical on numpy/scipy/pandas). Measured OFF 13s →
ON 305s (**23×**). Diagnosed: the re-record loop is O(closure) per derivation and QUADRATIC
under nesting (a top-level derivation re-records its whole ~10K-dep transitive closure, at
every nesting level). That is the PROXY's cost, not the design's — the real aggressive shape
emits ONE edge (O(1)), not N re-records. 4th confound in this arc: a measurement choice that
looks faithful (soundness-neutral) but measures the wrong thing.

**Corrected measurement (NIX_PROTOTYPE_DRV_SUBSCOPE=cost):** open the sub-scope, take+DISCARD
its deps (no re-record). This measures scope ctor/push/accumulate-into-sub/take/dtor — the
structural per-derivationStrict cost — and the accumulate-into-sub IS part of the real design
(it's where the producer trace's deps come from). NOT soundness-neutral (consumer loses deps),
so it is a COST microbenchmark ONLY.

Result (python3Packages, 8-package map, fresh cache, 3 runs each, low variance):

| mode | runs | mean |
|---|---|---:|
| OFF | 12.91 / 13.51 / 13.06 | 13.16s |
| COST (sub-scope) | 14.64 / 14.64 / 14.69 | 14.66s |

**Overhead ≈ 1.5s on 13.2s = ~11%.** Confirmed the sub-scopes actually fired (unambiguous,
not inferred from timing): `depContextScopes` 971 → 3072 at identical nrThunks (745,708), the
+2,101 being the per-derivationStrict sub-scopes.

**Honest bounding:**
- ~11% is the SCOPE-STRUCTURAL cost ALONE. The real aggressive shape ADDS, on top: the
  producer-trace recordSync per distinct derivation (which §3b measured as the dominant
  net-loss cost) + 1 edge emit. So **~11% is a LOWER bound** on the real aggressive hot cost.
- Indicative, not the Ledger-D number — measured on python3Packages (derivation-dense);
  closures.gnome has different derivation-density-per-eval-second. Right ORDER (~10⁴
  derivations/eval), not the exact closures.gnome figure.

**Interpretation:** the scope structure alone is ~11% — already non-trivial for a hot-path
change, and on TOP of it sits the §3b-measured producer-record cost that made §3b net-negative.
So the aggressive shape's hot cost is *at least* ~11% and realistically more once producer
recording is added. This does NOT by itself kill the direction (benefit is high, #13: 37–99%),
but it confirms the hazard is real and the net win requires the producer-record cost to come
WAY down (async/batched — the §3b condition (a) that was never built). The two go/no-go numbers
are now BOTH measured: benefit 37–99% (high), hot cost ≥11% structural + producer-record on top.

### 2026-05-31 follow-up #15: adversarial pass on #13/#14 — corrects benefit "~99%" → ~63% (edges-per-derivation, not kind-fraction)

The mandated adversarial pass on the combined prototype findings (#13 benefit + #14 cost) found
a real refinement to #13. NOT done — addressed here.

**Attack 2 (material): #13's "~99%" conflated "fraction of consumer deps that is
derivation-closure" with "fraction REMOVED."** The edge model does NOT collapse a consumer's
derivation-closure deps to ONE edge — it keeps ONE EDGE PER DISTINCT DERIVATION the consumer
references. Measured: a consumer references mean ~1,213 distinct derivations (its SPA count =
distinct derivations, within-trace deduped). So the edge model collapses mean ~3,296 consumer
deps → ~1,213 edges:

- **Per-consumer dep-count reduction ≈ 63%** (3,296 → 1,213), bounded by
  distinct-derivations-per-consumer — NOT the 99% dep-kind fraction (#13 wrong) and NOT the 37%
  SPA-only floor (that was a strict subset). **~63% is the honest realistic figure.**
- **Storage: flattened-dep count 36.5M → ~13.4M edge-deps (≈63% fewer), PLUS the separate
  producer-dedup win** (each of the ~1,213 producer closures recorded ONCE globally rather than
  copied into every consumer — the 607× storage win, which is distinct from and larger than the
  per-consumer count reduction). #13 conflated these two wins.

So the corrected benefit picture: **~63% per-consumer dep reduction + a large separate
producer-closure storage dedup.** Still substantial, materially below #13's "~99%", above the
37% floor. The number is set by distinct-derivations-per-consumer (~1,213), a measured quantity.

**Attack 1 (gap, lower severity): the ~11% cost (#14) was measured on python3Packages, not the
Ledger-D/closures.gnome anchor** where §3b's 14× hot blowup was measured. The per-derivation
scope cost is workload-portable in order-of-magnitude (~10⁴ derivations/eval both), but the exact
closures.gnome figure for THIS prototype is unmeasured. Noted as a gap, not corrected (would need
a bench-harness run; the order is established).

**Both numbers, final honest state:**
- Benefit: **~63%** per-consumer dep reduction (+ separate producer-storage dedup). [floor 37% SPA-only; #13's 99% retracted]
- Cost: **≥11%** scope-structural (python3Packages; lower bound — real adds §3b producer-recordSync on top).

The direction is **plausibly net-positive on dep-count/storage but gated on the producer-record
cost** (§3b's measured dominant cost) coming down via async/batched recording — the never-built
§3b condition (a). The prototype has measured what it can without building async recording.

### 2026-05-31 follow-up #16: adversarial pass on #15 — the ~63% survives; new insight: heavy cross-derivation leaf overlap within a consumer

Next mandated pass, on #15's two new claims (~63% benefit, "plausibly net-positive").

**Attack 3 — ~63% STRESS-TESTED, stands; but surfaced a structural insight.** Checked whether
"~63%" wrongly assumes an edge removes a derivation's NON-SPA flattened deps (FileBytes/StructProj
from the derivation's inputs) and not just the SPA. It does remove them — that IS the
aggressive-shape mechanism (the sub-scope isolates ALL of D's input-reads, replaced by 1 edge),
and the 63% arithmetic (consumer-distinct-deps → consumer-distinct-derivations) is correct.

The NEW insight from the check: mean deps-per-derivation IN THE CONSUMER = 3,296/1,213 = **2.7**,
vs the benefit probe's **16.4** deps-per-derivation-RANGE. The 6× gap means derivation ranges
OVERLAP heavily within a consumer — a consumer's flattened closure dedups shared leaves (stdenv
files etc.) that belong to MANY derivations' ranges. Consequence: you cannot "remove a shared leaf
twice"; the per-consumer edge win (~63%) is genuinely smaller than the raw 607× duplication would
naively suggest, because much of a consumer's flattening is shared-leaf deps that multiple
derivation-edges each cover. The ~63% already accounts for this (distinct-deps → distinct-derivations);
the insight is WHY 63% and not more — and it tempers any "collapse the whole 607×" framing.

**Attack 4 — "plausibly net-positive" hedge is appropriate.** Dep-count/storage reduction (~63%)
helps VERIFY time + storage, but §3b's net-loss was RECORD time (producer recordSync) + hot
re-eval. So net-positive requires verify-savings > record-cost, which is UNMEASURED and (per #15)
gated on async/batched producer recording. The hedge stands; no correction.

Net of this pass: ~63% confirmed (not corrected); one structural insight added (cross-derivation
leaf overlap, 2.7 vs 16.4). The benefit/cost numbers are stable. Remaining genuine unknown is
unchanged: the record-cost-vs-verify-savings net, gated on async recording (not built).
