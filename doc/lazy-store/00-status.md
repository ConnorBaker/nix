# Lazy-store status

Revision `83d0a172d` has lower separate wall medians and lower median paired ratios in **all nine benchmark cases** against branch base `8aad447`, with **69 of 75 pairs faster**. The [complete optimized comparison](runs/2026-09-21-performance/paired-83d0a172d.json) passes all 156 output checks and eighteen complete store comparisons. Median CPU remains higher in three cases and peak RSS in seven; these elapsed-time improvements do not imply uniformly lower resource use.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash preserves that meaning, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file blobs live in store paths themselves. The database retains candidate holders per object, mode and ingestion; lookup considers only registered valid paths. Reuse checks physical metadata and records the recipient's own witness. Publication reconciles concurrent inode classes before exposing candidates, and a complete re-ingestion replaces that path's previous projection. Trees and symlink-target blobs live in the auxiliary object namespace. Holds protect typed auxiliary addresses through repair and collection; temporary roots belong to individual live store handles.

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Source-to-store memos retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

Completed evaluator requests are a separate kind of fact: the result of an earlier operation, identified by resolved source, method and output name. Reuse does not identify current mutable bytes. Filtered, asserted, reference-bearing and repair requests bypass this memo; their existing validation and destination-reuse rules still apply. Mounting or explicit reset retires completed requests.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Completed validation and limits

The [current frozen source](/home/connorbaker/Benchmarks/nix-performance-fixes/resource-node-validation/README.md) passes **2,498 primary unit tests** (453 evaluator, 159 fetcher, 895 utility and 991 store), **895 syscall-fallback utility tests**, **eight functional targets** and **all twelve real overlay tests**. Two runtime skips and three disabled cases remain separate. Actual namespace/mount/overlay-write probes precede the functional and overlay gates. The exact optimized release also passes 207 packaged tests, with zero failures and 36 skips.

The [performance report](09-performance.md#verification) preserves finite-domain checks, faulty variants with exact restorations, source ownership evidence and their limits. Complete store comparisons check contents, modes, references and outputs under an injective substitution of store addresses; they do not establish equivalence of arbitrary embedded programs. The [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/paired-83d0a172d-audit/README.md) independently checks recorded mappings and all 156 outputs, without repeating physical graph walks.

These results apply to aarch64 Linux; **Windows remains unverified**. Overlay's lack of cross-layer regular-holder sharing and disabled auxiliary verification/materialisation remain explicit in the store model. Original fixture/checker failures and corrected runs are retained in their archives, rather than counted as passes. Older [validation](05-validation.md) and [holder investigations](06-holder-index.md) are evidence for their recorded implementations.

## Performance and remaining costs

The pinned workload has five paired cold trials and ten paired warm trials per case. Cold means fresh private store/fetcher state with warm operating-system caches; GitHub cold timings include network access. Reductions in the **median paired wall-time ratio against branch base** are:

| Workload | `path:` | `git+file:` | `github:` |
| --- | ---: | ---: | ---: |
| Cold · 32 attributes | 14.02% | 22.59% | 23.35% |
| Warm · 8 attributes | 86.70% | 6.73% | 14.25% |
| Warm · 32 attributes | 72.81% | 4.86% | 7.59% |

Cold GitHub wins four of five pairs, warm Git8 eight of ten and warm Git32 seven of ten; all other retained pairs are faster. Every slower trial remains included. Cold-path CPU rises 3.41→6.59 s and peak RSS 445.17→490.43 MiB against branch base. Both warm Git cases also use more CPU. The full report retains all CPU/RSS observations and both ratio statistics.

The final representation reductions replace redundant traversal context, child-name copies, callback captures, hash machinery and pending-row storage. The last standard-container substitution changes allocation, hashing and iteration together. A [focused five-pair 83/f4 comparison](/home/connorbaker/Benchmarks/nix-performance-fixes/node-83d0a172d-vs-f4da49de9/verified-resources.json) finds a median paired RSS reduction of 19.77 MiB and wall reduction of 60.21 ms, both lower in 4/5 pairs, but CPU rises 0.04 s. Separate wall medians instead rise slightly. All ten outputs match their references; these focused stores were not graph-walked again. This is evidence for the tested contrast, not uniform improvement or allocation-level attribution.

The immediate [f4 predecessor](runs/2026-09-21-performance/paired-f4da49de9.json) also beat branch base in all nine elapsed cases. Its [direct comparison with 58a7](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-f4da49de9-vs-58a7bb613/verified-resources.json) was mixed: cold-path paired wall rose 83.48 ms and peak RSS 32.00 MiB, higher in every pair, while CPU fell 0.06 s. Those regressions remain in the report. The focused 83/f4 comparison and current 83/base matrix are separate measurements; observations are not pooled across runs.

The latest [user-cycle profiles](09-performance.md#remaining-cost-measured-separately) measure 58a7, before these changes. They expose materialisation, SQLite and raw-worktree hashing, with incomplete caller chains. User-cycle samples cannot attribute kernel time, allocation volume or peak RSS. The initial raw fold follows the current content-addressed input contract; clean Git status cannot supply the missing immutable identity.

## Evidence and reproduction

- [Performance report](09-performance.md), [current sample record](runs/2026-09-21-performance/paired-83d0a172d.json) and [release provenance](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-83d0a172d.json): exact source/binaries, inputs, raw observations and validation.
- [Evaluation driver](runs/nixpkgs-evaluation.py): three URI schemes, cold 32 and warm 8/32 attributes, balanced order and CPU affinity. Builds/profiles run separately; shared-host exclusivity is not claimed.
- [Complete store comparison](runs/compare-store-graphs.py) and [methodology counterchecks](runs/check-benchmark-methodology.py): exact graph/content/output checks and planted faults. Each cold store has its own comparison; copying an indexed store changes holder inodes and is not a representative warm fixture.
- [Validation commands](05-validation.md) and [overlay host driver](runs/overlay-suite-on-host.sh): actual mount tests are required; a skipped probe is not a pass.

When resuming, bind each result to its tested source and binary. Re-run affected checks and the full optimized comparison after runtime changes; retain regressions until subsequent measurements resolve them.
