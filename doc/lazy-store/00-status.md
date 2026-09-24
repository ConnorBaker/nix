# Lazy-store status

Measured revision `6739a4955` has lower separate wall medians and median paired wall-time ratios in **all nine cases** against branch base `8aad447`, with **66/75 pairs faster**. All 156 output checks and eighteen complete store comparisons pass. CPU medians remain higher in three cases and peak RSS in seven; resource reduction remains unresolved.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash is computed from that tree, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file blobs live in store paths themselves. The database retains candidate holders per object, mode and ingestion; lookup considers only registered valid paths. Reuse checks physical metadata and records the recipient's own witness. Publication reconciles concurrent inode classes before exposing candidates, and a complete re-ingestion replaces that path's previous projection. Trees and symlink-target blobs live in the auxiliary object namespace. Holds protect typed auxiliary addresses through repair and collection; temporary roots belong to individual live store handles. The [auxiliary representation analysis](/home/connorbaker/Benchmarks/nix-performance-fixes/auxiliary-representation-e77d0c12e/README.md) separates required retrievable bodies and live claims from their current file-per-body layout; it proposes no replacement and leaves regular-file inode sharing intact.

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Source-to-store memos retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

Completed evaluator requests are a separate kind of fact: the result of an earlier operation, identified by resolved source, method and output name. Reuse does not identify current mutable bytes. Filtered, asserted, reference-bearing and repair requests bypass this memo; their existing validation and destination-reuse rules still apply. Mounting or explicit reset retires completed requests.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Current validation

6739 reuses an acquired stored accessor for legacy hash computation, exposes known NAR listing types and transfers owned fetcher results. Its [targeted source gates](/home/connorbaker/Benchmarks/nix-performance-fixes/source-projections-final-validation-3fedcc95f/README.md) pass **five GTests and six functional targets, with no skips**. The exact optimized release passes **207 packaged tests, zero failures and 36 skips**.

[Earlier full-suite and overlay gates](09-performance.md#current-source-validation) remain bound to their historical sources; they were not rerun as current full gates. Windows remains unverified. Overlay's regular-holder sharing and auxiliary-operation limits remain explicit in the store model.

## Performance

The pinned workload retains five cold and ten warm pairs per case. Cold means fresh private state with warm operating-system caches; GitHub cold timings include network access. Reductions in **median paired wall-time ratio against branch base** are:

| Workload | `path:` | `git+file:` | `github:` |
| --- | ---: | ---: | ---: |
| Cold · 32 attributes | 16.17% | 19.14% | 49.05% |
| Warm · 8 attributes | 87.98% | 9.62% | 13.94% |
| Warm · 32 attributes | 77.98% | 3.33% | 6.89% |

Cold-path CPU rises **3.77→6.82 s** and RSS **444.88→484.35 MiB**, both higher in every pair. Both warm Git cases also use more CPU. Lower wall time has not resolved these resource costs.

The [direct comparison with 3fed](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-6739a4955-vs-3fedcc95f-audit/README.md) passes another 156 exact output checks but has only **30/75 faster wall pairs**, five higher median paired wall ratios, five higher CPU medians and six higher RSS medians. The mixed results do not establish a general improvement over 3fed or isolate any individual change's effect.

The [performance report](09-performance.md), [current samples](runs/2026-09-21-performance/paired-6739a4955.json), [independent main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/paired-6739a4955-audit/README.md) and [release provenance](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-6739a4955.json) retain exact source/binary bindings, checks and limits. Earlier comparisons, the retained interference/repetition incident and historical profiles remain in that report; they do not measure later source changes.
