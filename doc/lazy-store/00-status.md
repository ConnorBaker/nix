# Lazy-store status

Measured revision `b7dd176b8` has lower separate wall medians and median paired wall-time ratios in **all nine cases** against branch base `8aad447`, with **70/75 pairs faster**. All 156 output checks and eighteen complete store comparisons pass. CPU medians remain higher in three cases and peak RSS in seven; resource reduction remains unresolved.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash is computed from that tree, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file blobs live in store paths themselves. The database retains candidate holders per object, mode and ingestion; lookup considers only registered valid paths. Reuse checks physical metadata and records the recipient's own witness. Publication reconciles concurrent inode classes before exposing candidates, and a complete re-ingestion replaces that path's previous projection. Trees and symlink-target blobs live in the auxiliary object namespace. Holds protect typed auxiliary addresses through repair and collection; temporary roots belong to individual live store handles. The [auxiliary representation analysis](/home/connorbaker/Benchmarks/nix-performance-fixes/auxiliary-representation-e77d0c12e/README.md) separates required retrievable bodies and live claims from their current file-per-body layout; it proposes no replacement and leaves regular-file inode sharing intact.

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Source-to-store memos retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

Completed evaluator requests are a separate kind of fact: the result of an earlier operation, identified by resolved source, method and output name. Reuse does not identify current mutable bytes. Filtered, asserted, reference-bearing and repair requests bypass this memo; their existing validation and destination-reuse rules still apply. Mounting or explicit reset retires completed requests.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Current validation

b7dd keeps the filtering accessor's owner guard local to six forwarding methods and the public admission method and factors permission checking into a private `void` helper. Its [source gates](/home/connorbaker/Benchmarks/nix-performance-fixes/filter-local-owner-validation/README.md) pass **614 fetchers/expression unit tests** (one skip, one disabled), **sixteen functional targets** and **twelve real overlay targets without skips**. The exact optimized release passes **207 packaged tests, zero failures and 36 skips**. No tests were added; other unit suites were not rerun. The tests check documented lifetime guarantees, not exact internal reference counts or stack-frame identity.

[Earlier broader unit gates](09-performance.md#current-source-validation) remain bound to their historical sources. Windows remains unverified. Overlay's regular-holder sharing and auxiliary-operation limits remain explicit in the store model. Any subsequent source changes remain outside these b7dd performance measurements.

## Performance

The pinned workload retains five cold and ten warm pairs per case. Cold means fresh private state with warm operating-system caches; GitHub cold timings include network access. Reductions in **median paired wall-time ratio against branch base** are:

| Workload | `path:` | `git+file:` | `github:` |
| --- | ---: | ---: | ---: |
| Cold · 32 attributes | 1.32% | 19.77% | 49.36% |
| Warm · 8 attributes | 87.16% | 6.46% | 14.65% |
| Warm · 32 attributes | 73.69% | 4.93% | 8.63% |

Cold-path CPU rises **3.24→6.59 s** and RSS **445.22→480.74 MiB**, both higher in every pair. Both warm Git cases also use more CPU. Lower wall time has not resolved these resource costs.

The [direct comparison with cc59](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-b7dd176b8-vs-cc59f1bf9-audit/README.md) passes another 156 exact output checks but has only **43/75 faster wall pairs** and four higher median paired wall ratios. Four cases reverse direction between separate and paired wall statistics. CPU has four higher separate medians but six higher paired medians; RSS has seven higher separately and five higher paired. Cold-path paired wall/CPU rise **153.56 ms/0.58 s**, with CPU higher in every pair, while paired RSS falls 3.82 MiB despite a higher separate median. These mixed results establish no overall direct resource benefit or individual causal saving.

The current [user-cycle profiles](09-performance.md#current-b7ddbase-user-cycle-profiles) cover all nine cases in eighteen captures with six seeds and 24 exact outputs. They retain user-space routes, unknown/clipped caller limits and one pair per case; they cannot attribute system CPU, allocated bytes or peak RSS. Older syscall and PMU diagnostics remain historical.

The [performance report](09-performance.md), [current samples](runs/2026-09-21-performance/paired-b7dd176b8.json), [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/paired-b7dd176b8-audit/README.md) and [release provenance](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-b7dd176b8.json) retain exact source/binary bindings, checks and limits. Earlier comparisons and the retained interference/repetition incident remain linked there; they do not measure later source changes.
