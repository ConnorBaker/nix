# Lazy-store status

Latest measured runtime **daa1ff079** lowers all nine wall medians against fixed branch base **8aad447**, with **69/75 faster pairs**, while **three CPU medians and seven peak-RSS medians rise**. Directly against **0871ab183**, three separate wall medians and three paired ratios are lower, with **29/75 faster pairs**; the lower-case sets differ. Cold-path wall is higher in all five direct pairs. These changes establish no universal resource improvement.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash is computed from that tree, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file contents live in store paths. The database retains candidate holders per object, mode and ingestion; lookup requires a registered valid path. Reuse checks physical metadata and records the recipient's own witness, and publication reconciles inode classes before exposing candidates. Directory bodies and symlink targets are owned metadata in `AuxValues`, published with the same completed ingestion. Every reused body becomes part of the recipient's projection. Locked ingestion leases protect completed outputs before validity registration; whole-store collection removes stale projections after dead-lease cleanup, without an auxiliary mark graph. Upper-only overlay authority and regular-file sharing are unchanged (`08-store-model.md`).

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Source-to-store memos retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

Completed evaluator requests are a separate kind of fact: the result of an earlier operation, identified by resolved source, method and output name. Reuse does not identify current mutable bytes. Filtered, asserted, reference-bearing and repair requests bypass this memo; their existing validation and destination-reuse rules still apply. Mounting or explicit reset retires completed requests.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Current validation

[Current validation](/home/connorbaker/Benchmarks/nix-performance-fixes/empty-copy-groups-validation/README.md) passes **2,516 primary / 911 fallback tests**, zero failures, with skips 2/1 and disabled 3/1. All **99/32 required cases**, **17 functionals** and **12 real overlays** pass. The new 72-case oracle checks first-child read/fold failure selection; existing header, advisory-flag, listing, SQL and ownership cases remain. The packaged release passes 207 tests, zero failures and 36 skips. Skipped/disabled/device-unavailable and Windows cases are not executed coverage; debug allocator observations do not measure release resources.

The measured four-file change uses scheduler groups only with more than one worker, more than one child and at least one directory hint; other groups read/fold inline even with an existing pool. It also uses bounded backward decimal-header conversion. First failing child/read-before-own-fold selection is preserved. Later unstarted effects and resource failure traces are not asserted identical.

## Latest completed performance

Each independent series retains five cold and ten warm pairs per case. Cold means fresh private application state, not evicted OS caches; GitHub cold includes network access. Median paired wall changes against fixed base (negative means lower):

| Workload | `path:` | `git+file:` | `github:` |
| --- | ---: | ---: | ---: |
| Cold · 32 attributes | -6.538% | -20.022% | -40.703% |
| Warm · 8 attributes | -87.082% | -6.244% | -10.745% |
| Warm · 32 attributes | -73.745% | -3.690% | -8.047% |

Cold-path CPU rises **3.190→5.630 s** and peak RSS **445.22→484.70 MiB**, both higher in every pair. Both warm Git cases also have higher CPU in every pair.

Direct cold-path wall worsens **1.093% separately / 2.809% paired**, with **0/5 faster pairs**, CPU **5.750→5.500 s**, and RSS **492.72→502.74 MiB**. Cold GitHub wall worsens **19.796% / 14.120%**. Warm Git8 and GitHub8 reverse in opposite directions between separate and paired wall summaries. Direct CPU/RSS remain mixed.

The [performance report](09-performance.md) retains all eighteen rows and limits. The [complete numeric review](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-daa1ff079/README.md) preserves all ninety metric cells, ties, reversals and ranges. The [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/paired-daa1ff079-audit/README.md) checks 156 outputs and eighteen store graphs; the [direct audit](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-daa1ff079-vs-0871ab183-audit/README.md) checks another 156 outputs without supplemental graph walks. The [release proof](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-daa1ff079.json) binds measured sources/outputs. Separate series are not pooled and do not isolate individual edits.

Fresh [daa1 user-FP findings](/home/connorbaker/Benchmarks/nix-performance-fixes/resource-profile-user-fp-daa1ff079/independent-review/interpretation/README.md) retain all nine cases, 26,518 samples, incomplete callers, 2,530 unknown leaves and two zero-IP observations. Hashing, CopyPool and SQL publication remain visible; sampled shares do not isolate the direct regressions. The [completed compiled comparison](/home/connorbaker/Benchmarks/nix-performance-fixes/assembly-empty-groups-header-daa1ff079-vs-0871/comparison/ROUTES.md) finds header bytes 724→304 and frame bytes 320→64; both consumer frames remain 592. Selected instruction bytes fall 756 while the store ELF grows 80 bytes. For at least two children with all-false hints, the warmed-pool bypass adds a hint scan and avoids empty group machinery; no dynamic benefit follows from those static facts. Earlier 6043 kernel/syscall observations remain separately identified in the report.

Base libgit2 1.9.7 versus branch 2.0.0-rc1 plus patches is a dependency confound; the direct comparison controls these versions while retaining the combined edits/layout. Base Git revision-keyed reuse also differs from the branch's absence of a worktree fingerprint: clean revision metadata does not name live/rendered bytes.
