# Lazy-store status

Measured revision `dbc65fd81` has lower separate wall medians and median paired wall-time ratios in **all nine cases** against branch base `8aad447`, with **70/75 pairs faster**. All 156 output checks and eighteen complete store comparisons pass. CPU medians remain higher in four cases and peak RSS in seven; resource reduction remains unresolved.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash is computed from that tree, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file blobs live in store paths themselves. The database retains candidate holders per object, mode and ingestion; lookup considers only registered valid paths. Reuse checks physical metadata and records the recipient's own witness. Publication reconciles concurrent inode classes before exposing candidates, and a complete re-ingestion replaces that path's previous projection. Trees and symlink-target blobs live in the auxiliary object namespace. Holds protect typed auxiliary addresses through repair and collection; temporary roots belong to individual live store handles. The [auxiliary representation analysis](/home/connorbaker/Benchmarks/nix-performance-fixes/auxiliary-representation-e77d0c12e/README.md) separates required retrievable bodies and live claims from their current file-per-body layout; it proposes no replacement and leaves regular-file inode sharing intact.

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Source-to-store memos retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

Completed evaluator requests are a separate kind of fact: the result of an earlier operation, identified by resolved source, method and output name. Reuse does not identify current mutable bytes. Filtered, asserted, reference-bearing and repair requests bypass this memo; their existing validation and destination-reuse rules still apply. Mounting or explicit reset retires completed requests.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Current validation

dbc delivers owned immutable Git/NAR bytes directly, retains callback owners, propagates consumer `EndOfFile` and validates filenames as unchanged single components. Its [source gates](/home/connorbaker/Benchmarks/nix-performance-fixes/byte-filename-validation-6739a4955/README.md) pass **2,055 unit tests** (two skips, two disabled) and **eight functional targets without skips**; the old-production negative phase remains separate. The exact optimized release passes **207 packaged tests, zero failures and 36 skips**.

[Earlier full-suite and overlay gates](09-performance.md#current-source-validation) remain bound to their historical sources; they were not rerun as current full gates. Windows remains unverified. Overlay's regular-holder sharing and auxiliary-operation limits remain explicit in the store model.

## Performance

The pinned workload retains five cold and ten warm pairs per case. Cold means fresh private state with warm operating-system caches; GitHub cold timings include network access. Reductions in **median paired wall-time ratio against branch base** are:

| Workload | `path:` | `git+file:` | `github:` |
| --- | ---: | ---: | ---: |
| Cold · 32 attributes | 11.97% | 16.30% | 27.84% |
| Warm · 8 attributes | 87.81% | 4.37% | 11.82% |
| Warm · 32 attributes | 75.32% | 4.46% | 6.58% |

Cold-path CPU rises **3.32→6.39 s** and RSS **445.21→470.71 MiB**, both higher in every pair. Cold Git and both warm Git cases also use more CPU. Lower wall time has not resolved these resource costs.

The [direct comparison with 6739](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-dbc65fd81-vs-6739a4955-audit/README.md) passes another 156 exact output checks, with **41/75 faster wall pairs**, six lower and three higher median paired wall ratios. CPU medians rise in three cases and RSS in four; cold-path paired CPU/RSS rise 0.08 s/1.81 MiB despite lower wall time. These mixed results establish no all-metric improvement or individual-change effect.

The [performance report](09-performance.md), [current samples](runs/2026-09-21-performance/paired-dbc65fd81.json), [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/paired-dbc65fd81-audit/README.md) and [release provenance](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-dbc65fd81.json) retain exact source/binary bindings, checks and limits. Earlier comparisons, the retained interference/repetition incident and historical profiles remain in that report; they do not measure later source changes.
