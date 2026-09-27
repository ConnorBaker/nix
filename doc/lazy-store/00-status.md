# Lazy-store status

The latest measured runtime is **29f2fc433**, against fixed branch base **8aad447**. All nine cases have lower separate wall medians and paired wall ratios, with **70/75 faster pairs**, but **three CPU medians and seven peak-RSS medians rise**. Directly against **a357d6454**, only three separate wall medians and five paired ratios are lower, with **36/75 faster pairs**. These observations do not establish an overall resource improvement. The SQL auxiliary representation described below has passed validation but has no completed release or timing results yet.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash is computed from that tree, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file contents live in store paths. The database retains candidate holders per object, mode and ingestion; lookup requires a registered valid path. Reuse checks physical metadata and records the recipient's own witness, and publication reconciles inode classes before exposing candidates. Directory bodies and symlink targets are owned metadata in `AuxValues`, published with the same completed ingestion. Every reused body becomes part of the recipient's projection. Locked ingestion leases protect completed outputs before validity registration; whole-store collection removes stale projections after dead-lease cleanup, without an auxiliary mark graph. Upper-only overlay authority and regular-file sharing are unchanged (`08-store-model.md`).

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Source-to-store memos retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

Completed evaluator requests are a separate kind of fact: the result of an earlier operation, identified by resolved source, method and output name. Reuse does not identify current mutable bytes. Filtered, asserted, reference-bearing and repair requests bypass this memo; their existing validation and destination-reuse rules still apply. Mounting or explicit reset retires completed requests.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Current validation

The nineteen-file SQL auxiliary change passes **2,512 primary unit tests** and **909 fallback-util tests**, with zero failures. Primary skips/disabled are **2/3**, fallback **1/1**; those counts remain separate. All **94 primary and 29 fallback required cases**, **17 functionals** and **12 real-overlay targets** pass. The [completed independent review](/tmp/nix-auxiliary-sql-validation-review-v2/result.json) binds the tested source and reconciles all recorded cases and outcomes. It covers typed keys/chunks, owned snapshots, complete publication, corruption/repair, ingestion leases, bounded legacy cleanup and overlay authority. This is source/debug validation, not an optimized-release or performance result. Windows execution remains unverified.

## Latest completed performance

Five cold and ten warm pairs per case retain every observation. Cold means fresh private application state, not evicted OS caches; GitHub cold includes network access. Median paired wall changes against fixed base (negative means lower):

| Workload | `path:` | `git+file:` | `github:` |
| --- | ---: | ---: | ---: |
| Cold · 32 attributes | -4.911% | -19.627% | -40.997% |
| Warm · 8 attributes | -85.449% | -6.864% | -13.762% |
| Warm · 32 attributes | -74.014% | -5.514% | -7.959% |

Cold-path CPU rises **3.190→6.570 s** and peak RSS **445.21→474.34 MiB**, both higher in every pair. Both warm Git cases also have higher CPU in every pair. Direct wall medians are higher for all three cold cases and warm path8. Warm Git8/Git32 have higher separate wall medians but lower paired ratios; direct CPU and RSS are mixed.

The [complete 29f2 numeric review](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-29f2fc433/README.md) retains all eighteen cases, five metrics, ties, reversals and raw ranges. The [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/paired-29f2fc433-audit/README.md) checks 156 outputs and eighteen store-graph comparisons; the [direct audit](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-29f2fc433-vs-a357d6454-audit/README.md) checks another 156 outputs, without new supplemental graph walks. The [strict release proof](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-29f2fc433.json) binds the actual measured sources and outputs.

Earlier f83, 117 and 110 profiles and syscall traces are historical diagnostics, not measurements of 29f2 or SQL storage. Incomplete callers, unknown/empty frames, overlapping routes and different collection environments limit attribution. [09-performance.md](09-performance.md) is awaiting its compact 29f2 replacement; its older measured revision must not be read as the current source or latest result. No series is pooled and no individual edit is assigned causal credit.
