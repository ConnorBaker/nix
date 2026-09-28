# Evaluation performance and preserved behavior

This report records the revisions named below. It predates eager Text publication and the shared traversal/publication refactor; these timings and compiled-code findings do not measure that implementation.

Measured **407f5d23e8470e01b1c4b52fbfe02db5ce09de14** retains three cache synchronization flags relative to **e38b9b3327f827e58fbf00f3419362042a8cdda0**. The Pending flat-set experiment was withdrawn. Fixed-base wall results improve across the nine cases; the direct incremental results remain mixed.

## Protocol and all eighteen measurements

Both independent series evaluate Nixpkgs `6aefcda9401be8acc2b74244fb3b37520ea1f0a8` on aarch64 Linux: five cold pairs per source and ten pairs per warm count/source, **156 evaluations including six excluded warm seeds, 75 retained pairs**. The [driver](runs/nixpkgs-evaluation.py) retains all scheduled observations. Cold means fresh private application state, not evicted OS caches; cold GitHub includes acquisition/network work. Affinity does not reserve the host.

Evaluation uses `eval-cache=false`, so these timings do not exercise the evaluator-cache flag. The unpinned `git+file:` URL reads live tracked files, the GitHub URL pins the stated Nixpkgs revision, and the path fixture is unchanged.

Rows show baseline→candidate medians. Separate wall change divides the separate medians; paired change takes the median of pair ratios. CPU adds recorded user/system centiseconds per observation before taking the median; component medians need not add. RSS is peak resident memory, not allocation volume. Exact values determine signs despite display rounding.

### Fixed base 8aad447f0bace6be413963e897ad690247ad6aa2 → 407f

| Case | Wall s | Separate / paired | Faster pairs | CPU s | RSS MiB |
| --- | --- | --- | --- | --- | --- |
| cold 32 path | 3.806437→3.218231 | -15.453% / -14.624% | 5/5 | 3.540→5.650 | 445.21→506.46 |
| cold 32 git | 2.858528→2.269099 | -20.620% / -17.612% | 5/5 | 2.710→2.570 | 479.60→480.60 |
| cold 32 github | 11.193846→8.627363 | -22.928% / -22.005% | 4/5 | 16.960→5.270 | 951.18→962.48 |
| warm 8 path | 3.285546→0.380393 | -88.422% / -88.444% | 10/10 | 3.140→0.825 | 224.27→204.62 |
| warm 8 git | 0.805282→0.747277 | -7.203% / -7.456% | 10/10 | 0.765→1.240 | 254.77→259.71 |
| warm 8 github | 0.298039→0.256020 | -14.099% / -13.623% | 10/10 | 0.285→0.240 | 197.32→201.44 |
| warm 32 path | 3.840247→0.930501 | -75.770% / -75.653% | 10/10 | 3.690→1.380 | 444.90→424.15 |
| warm 32 git | 1.375308→1.341269 | -2.475% / -3.516% | 8/10 | 1.300→1.825 | 479.33→481.35 |
| warm 32 github | 0.861912→0.764764 | -11.271% / -10.190% | 10/10 | 0.835→0.755 | 444.08→446.71 |

### Measured e38 → 407f

| Case | Wall s | Separate / paired | Faster pairs | CPU s | RSS MiB |
| --- | --- | --- | --- | --- | --- |
| cold 32 path | 3.277643→3.208389 | -2.113% / +4.912% | 2/5 | 5.810→5.830 | 490.68→502.58 |
| cold 32 git | 2.329074→2.237354 | -3.938% / -3.938% | 3/5 | 2.710→2.590 | 480.48→480.72 |
| cold 32 github | 4.720514→4.146550 | -12.159% / -10.681% | 4/5 | 6.920→6.830 | 942.71→940.73 |
| warm 8 path | 0.480053→0.464932 | -3.150% / -1.685% | 6/10 | 0.835→0.840 | 205.34→205.50 |
| warm 8 git | 0.847701→0.864841 | +2.022% / -0.119% | 5/10 | 1.315→1.300 | 259.91→260.75 |
| warm 8 github | 0.274797→0.268177 | -2.409% / +0.579% | 4/10 | 0.265→0.260 | 201.77→200.00 |
| warm 32 path | 1.052331→1.028456 | -2.269% / +2.872% | 2/10 | 1.405→1.435 | 428.32→427.10 |
| warm 32 git | 1.402551→1.402651 | +0.007% / +1.650% | 4/10 | 1.835→1.900 | 480.47→480.43 |
| warm 32 github | 0.816435→0.816361 | -0.009% / +0.135% | 5/10 | 0.805→0.805 | 446.91→445.76 |

All nine main wall medians and paired ratios are lower, **72/75 pairs faster**, but **three CPU and seven RSS medians rise**. Cold path and both warm Git counts have higher user, system and total CPU in every pair. Cold Git system time rises in all five pairs despite a lower total-CPU median.

Direct results give **seven lower separate wall medians versus four lower paired ratios, 35/75 pairs faster**. Cold path, warm 8 GitHub, warm 32 path and warm 32 GitHub reverse from lower separate medians to higher paired ratios; warm 8 Git reverses the other way. CPU separate medians are **4 lower / 1 equal / 4 higher**, paired ratios **3/2/4**; RSS is **5/0/4** under both reductions, with different case membership. Direct user ratios are **4/1/4**, paired deltas **3/2/4**. Cold-path CPU and RSS rise despite its lower separate wall median. The [complete numeric review](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-407f5d23e/COMPLETED-REVIEW.md) retains all five metrics, ranges, ties, thirteen classification differences and fourteen all-pairs-higher cells. These results support no uniform incremental cache-speedup claim.

## Model and implementation

The [specification](01-specification.md), [derivation](04-derivation.md) and [store model](08-store-model.md) define the contracts. Mutable worktrees retain live-byte semantics with fixed tracked membership; clean Git status cannot prove raw-byte identity. Immutable object accessors reuse proven identities. Address memos still root and check destinations; filtered/asserted/reference-bearing/repair requests bypass completed-request reuse. Overlay authority remains upper-only.

The three retained changes omit only private SQLite connection mutexes in evaluator, fetcher and NarInfo caches. Existing owner locks or exclusive construction/destruction cover operations and cleanup; returned values are owned. Allocator/shared-Btree/file/WAL locks remain, and SQLite's global threading mode is unchanged. Sequential tests support the implementation, not every concurrent schedule. Mutex instrumentation and allocation-failure traces can differ.

Earlier holder spelling, single-owner SQLite Use, fresh-publication eligibility and regular path snapshots remain. The [e38 compiled review](/home/connorbaker/Benchmarks/nix-performance-fixes/assembly-sqlite-target-e38b9b332-vs-02775/assembly/comparison/READOUT.md) covers all eighteen changed Use sites, including test support: copied owners/pre-step resets disappear, final resets remain. Selected code shrinks 276 bytes, while Target/registration families grow 620/104 bytes and regular/registration frames grow 16/32 bytes. The fresh validity probe is added versus 02775; named-root Target cleanup moves after putRootTree. These prior static facts do not isolate current timing effects. Installed Use copy construction is deliberately rejected, a source-API compatibility change.

Canonical raw modes and the complete git-object-v2 namespace repair child/legacy-parent memo reuse; typed loading still validates bodies. CanonPath final-capacity reservation is supported by a separate [4,225-pair allocation control](/home/connorbaker/Benchmarks/nix-performance-fixes/canonpath-allocation-control-f6325db78-vs-4264dcc2c/COMPARISON-REVIEW.json): scalar-new requests 8,644→4,119 with every result matching. Its native allocator route and repeated-byte length domain do not establish production RSS or speed.

## Withdrawn experiment and validation

The [a88 experiment](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-a88ecdb3a/COMPLETED-REVIEW.md) combined these flags with a private flat Pending set. Against e38, cold-path separate wall rose **9.995%**, CPU **2.985%**, RSS **3.209%**; four of five wall pairs lost. Its [all-nine user counters](/home/connorbaker/Benchmarks/nix-performance-fixes/perf-stat-a88ecdb3a-vs-e38b9b332/readout/README.md) show cold-path instructions **−0.038941%**, with cycles mixed. This neither explains away the slower observation nor isolates flags/container/layout. Flat storage removes individual nodes but requires at least **29 Value slots per occupied shard** and relocates values during growth; distinct-key pin, error and resource traces can change. The substitution was withdrawn, restoring the full tested 7baf tree. Complete rejected-candidate records remain intact. The [withdrawn assembly review](/home/connorbaker/Benchmarks/nix-performance-fixes/assembly-pending-flat-set-a88ecdb3a-vs-e38b9b332/assembly/comparison/READOUT.md) finds **5,080 more selected text bytes**, local insertion frames **144→352 / 256→448 bytes**, and a **4,560-byte minimum first array** plus shard storage, with resident relocation during growth; these static costs do not establish the timing cause.

[Existing cache validation](/home/connorbaker/Benchmarks/nix-performance-fixes/cache-external-synchronization-validation/validation/independent-review.json) applies through that exact full-tree identity: **2,524/915 passes**, zero failures, skips **2/1**, disabled **3/1**, **111/39 mandatory cases**, **17 functionals / 12 real overlays**, all seven stages zero. No fresh validation execution is claimed. The new 407f optimized release and canonical seven-stage pipeline pass; its packaged functional suite is **207 pass / 36 skip**. The [comparison archive](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-407f5d23e/INDEX.md) binds exact source/release/dependency identities, all 312 outputs and schedules, graph associations and independently replayed arithmetic. Local preparation failures and corrections remain separate from successful target runs.

The [old-code memo control](/home/connorbaker/Benchmarks/nix-performance-fixes/git-fingerprint-validation/INDEX.md), [parser diagnostic-oracle correction](/home/connorbaker/Benchmarks/nix-performance-fixes/three-simplifications-validation/INDEX.md) and [f632 wrapper-path correction](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-f6325db78/COMMAND-CORRECTION.json) remain preserved. Candidate-only 65,536-mode coverage is not old-control coverage. Skipped/disabled/device-unavailable and unexecuted platforms are not coverage; Linux fallback does not exercise a platform lacking SOCK_CLOEXEC. Rejected [membership](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-40cf5d888/source-audits/workdir-ancestor.md), [metadata substitution](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-40cf5d888/source-audits/directory-permission.md), [prefix hashing](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-945b6fd2b/source-audits/prefix-decision.md) and [regular-status removal](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-407f5d23e/source-audits/regular-status.md) explain retained checks without new complexity.

## Profiles, controls and limits

The earlier [02775→e38 user counters](/home/connorbaker/Benchmarks/nix-performance-fixes/perf-stat-e38b9b332-vs-02775ad52/readout/README.md) show cold-path instructions **−3.686%**, other eight differences within **±0.06%**. Both counter experiments use one whole-command pair per case, including wrappers/descendants; they establish user-only instruction/cycle attributes, not pure-user task-clock time. They collect no kernel counters and are not explanations of timing medians.

The [02775 DWARF pair](/home/connorbaker/Benchmarks/nix-performance-fixes/cold-path-sqlite-callchains-02775ad52/analysis/README.md) retains **648/937 user samples**, **57/158 SQLite leaves** and every unknown/unwind/failure record. Twelve candidate SQLite samples bind the coalescing collision-SELECT caller, not its invocation count. The [40cf IP-only profile](/home/connorbaker/Benchmarks/nix-performance-fixes/ip-profile-40cf5d888/capture/analysis/READOUT.md) retains **16,586 user samples**, sixteen unresolved sample occurrences, all vdso, and no unknown DSO/reported loss. Period-ns weights are not exact CPU/cycles/calls; named self samples do not bound inclusive cost.

Adverse [945b store-library](/home/connorbaker/Benchmarks/nix-performance-fixes/store-library-control-945b6fd2b/review/READOUT.md) and [a207 util-library](/home/connorbaker/Benchmarks/nix-performance-fixes/cold-path-util-control-a2077a82a/README.md) controls remain distinct: respectively 39/75 wall wins with seven higher RSS medians, and eight of ten wall losses. Historical [FD/libc routes](/home/connorbaker/Benchmarks/nix-performance-fixes/fd-attribution-8eae31e61/analysis/README.md), [hardware-IP quality failure](/home/connorbaker/Benchmarks/nix-performance-fixes/ip-profile-8eae31e61/capture/analysis-corrected/READOUT.md), [syscalls](/home/connorbaker/Benchmarks/nix-performance-fixes/syscall-counts-8dbd3414f-vs-base/analysis/README.md) and [frame-pointer evidence](/home/connorbaker/Benchmarks/nix-performance-fixes/resource-profile-user-fp-daa1ff079/independent-review/interpretation/README.md) retain overlap/unknown/kernel limits; diagnostics are not pooled.

Branch tarball `pack.window=0` and [patch 0003](../../packaging/patches/0003-packbuilder-correct-config.patch) skip delta preparation without isolated timing attribution. Base libgit2 **1.9.7** versus branch **2.0.0-rc1 plus six patches** confounds fixed-base results; direct e38 holds the dependency constant. Base revision-keyed Git reuse and branch live-byte naming differ. Shared-host finite series establish neither significance, every resource ordering nor individual-change causation.
