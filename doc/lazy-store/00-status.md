# Lazy-store status

Measured revision `7a6fb248d` has lower separate wall medians and median paired wall-time ratios in **all nine cases** against branch base `8aad447`, with **70/75 pairs faster**. All 156 output checks and eighteen complete store comparisons pass. CPU medians remain higher in three cases and peak RSS in seven; resource reduction remains unresolved.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash is computed from that tree, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file blobs live in store paths themselves. The database retains candidate holders per object, mode and ingestion; lookup considers only registered valid paths. Reuse checks physical metadata and records the recipient's own witness. Publication reconciles concurrent inode classes before exposing candidates, and a complete re-ingestion replaces that path's previous projection. Trees and symlink-target blobs live in the auxiliary object namespace. Holds protect typed auxiliary addresses through repair and collection; temporary roots belong to individual live store handles. The [auxiliary representation analysis](/home/connorbaker/Benchmarks/nix-performance-fixes/auxiliary-representation-e77d0c12e/README.md) separates required retrievable bodies and live claims from their current file-per-body layout; it proposes no replacement and leaves regular-file inode sharing intact.

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Source-to-store memos retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

Completed evaluator requests are a separate kind of fact: the result of an earlier operation, identified by resolved source, method and output name. Reuse does not identify current mutable bytes. Filtered, asserted, reference-bearing and repair requests bypass this memo; their existing validation and destination-reuse rules still apply. Mounting or explicit reset retires completed requests.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Current validation

Validated source `7a6fb248d` carries the observed parent name directly and projects recorded logical store-path basenames in both holder lookup and coalescing, preserving descriptor ownership and native physical joins. Its [whole-checkout gates](/home/connorbaker/Benchmarks/nix-performance-fixes/holder-projection-completion-validation/README.md) pass **2,514 utility/store/fetcher/expression cases**, with **zero failures, two skips and three disabled cases**, including all 23 required cases; **sixteen functional and twelve real-overlay targets pass without skips**. The exact optimized release passes **207 packaged tests, zero failures and 36 skips**. This two-file cleanup adds no tests.

[Source domains and compiled limits](09-performance.md#current-source-validation) remain explicit. Windows runtime remains unverified. Tests check documented behavior and lifetimes, not exact internal allocations or stack frames; numerical and profile evidence remains bound to the revisions it names.

## Performance

The workload retains five cold and ten warm pairs per case. Cold means fresh private application state with warm OS caches; GitHub cold timings include network access. Reductions in **median paired wall-time ratio against branch base** are:

| Workload | `path:` | `git+file:` | `github:` |
| --- | ---: | ---: | ---: |
| Cold · 32 attributes | 1.28% | 20.20% | 29.52% |
| Warm · 8 attributes | 87.17% | 7.31% | 14.55% |
| Warm · 32 attributes | 73.71% | 4.36% | 9.00% |

Cold-path CPU rises **3.10→6.37 s** and RSS **445.21→486.76 MiB**, both higher in every pair. Both warm Git cases also use more CPU. Lower wall time has not resolved these resource costs.

The [direct 7a817 comparison](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-7a6fb248d-vs-7a817d25e-audit/README.md) passes another 156 exact outputs but has only **33/75 faster wall pairs**: seven lower separate wall medians versus five lower paired ratios. Cold and warm-32 GitHub reverse from lower separate wall medians to higher paired ratios. Cold-path paired wall/CPU rise **119.69 ms/0.58 s**, while RSS falls **14.07 MiB**. Paired CPU has three lower, three tied and three higher cases; RSS has five lower and four higher. No overall direct resource benefit is established.

The current [7a6 user-cycle profiles](09-performance.md#current-7a6base-user-cycle-profiles) cover all nine cases in eighteen captures, six warm seeds and 24 exact outputs. They retain one empty stack, unknown frames and incomplete callers; overlapping user routes cannot attribute system CPU, allocated bytes or peak RSS. Historical syscall and PMU observations retain their own revision boundaries.

The [performance report](09-performance.md), [current samples](runs/2026-09-21-performance/paired-7a6fb248d.json), [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/paired-7a6fb248d-audit/README.md) and [release provenance](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-7a6fb248d.json) retain exact bindings and limits. Historical comparisons and the retained interference/repetition incident remain linked there; separate series are not pooled.
