# Lazy-store status

The optimized revision `58a7bb613` has lower median wall time and lower median paired ratios in **all nine benchmark cases** against branch base `8aad447`, with 71 of 75 pairs faster. Its [complete comparison](runs/2026-09-21-performance/paired-58a7bb613.json) passes all 156 output checks and eighteen complete store comparisons. CPU rises in four cases and peak RSS in six. A separate direct comparison against `d600a9662` is mixed: the resource simplifications do not uniformly improve CPU, memory or elapsed time.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash preserves that meaning, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file blobs live in store paths themselves. The database retains candidate holders per object, mode and ingestion; lookup considers only registered valid paths. Reuse checks physical metadata and records the recipient's own witness. Publication reconciles concurrent inode classes before exposing candidates, and a complete re-ingestion replaces that path's previous projection. Trees and symlink-target blobs live in the auxiliary object namespace. Holds protect typed auxiliary addresses through repair and collection; temporary roots belong to individual live store handles.

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Source-to-store memos retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

Completed evaluator requests are a separate kind of fact: the result of an earlier operation, identified by resolved source, method and output name. Reuse does not identify current mutable bytes. Filtered, asserted, reference-bearing and repair requests bypass this memo; their existing validation and destination-reuse rules still apply. Mounting or explicit reset retires completed requests.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Completed validation and limits

The [archived debug source](/home/connorbaker/Benchmarks/nix-performance-fixes/resource-final-validation/README.md) passes **2,496 unit tests with two existing runtime skips**: 453 evaluator, 159 fetcher, 895 utility and 989 store tests. Three disabled tests remain excluded. All 895 utility tests also pass with newer syscalls disabled. Eight targeted functional tests pass, including the language suite and fourteen execution-store shell cases, and **all twelve real overlay tests pass on actual mounts**. The exact `58a7bb613` optimized release passes 207 packaged tests, with zero failures and 36 skips. The archive retains the initial property failure and the corrected exact-token replay's diagnostic failure; the positive original-seed replay passes all 989 store tests. Overlay-specific limits, including no cross-layer regular-holder sharing and the disabled auxiliary verification/materialisation routes, remain explicit in the store model.

The [performance and verification report](09-performance.md#verification) records the finite-domain checks, negative controls, syscall evidence and source-review limits. The latest optimized comparison additionally passes all eighteen complete store-graph comparisons and binds all 156 evaluation outputs to their actual store pairs. These checks compare exact contents, modes, references and outputs under an injective substitution of store addresses. They do not establish equivalence of arbitrary embedded programs.

These results apply to aarch64 Linux. **Windows has not been runtime-verified.** The two unit-test runtime skips belong to the fetcher and utility suites. Older platform counts and experiments in [validation](05-validation.md) and the [holder-index investigation](06-holder-index.md) are evidence for their recorded implementations, not current cross-platform gates. The current source retains the cache-identity, workdir-policy, canonical-name, publication and fetched-input mount corrections. Older profiles remain explicitly attributed to their revisions.

The [current source audit](09-performance.md#current-source-validation) makes fetched inputs authoritative and derives every accessor operation from a total tree graft. It preserves evaluation memoization and reads shell metadata from the execution store. Earlier audits check cache identity and publication, registration domains, workdir membership and canonical names.

A generic regular-entry cache was rejected: a synchronous same-size source mutation between copies with different names produces `BBB` on branch base `8aad447` but stale `AAA` with that candidate. The [counterexample](09-performance.md#completed-request-reuse-and-holder-selection) is retained. Its replacement memoizes complete evaluator requests; the [restored public case](/home/connorbaker/Benchmarks/nix-performance-fixes/evaluator-request-counterexample-restored-e470a5169/README.md) returns `BBB`. This memo and the holder-selection minimum fold pass the current debug gates, three targeted negative controls and the optimized comparison.

## Performance and remaining costs

The measured resource pass removes duplicated workdir membership, unused buffering and hashing allocations, repeated closure captures, inline parser storage and the asynchronous holder-index writer. Candidate rows now remain private until each completed-path boundary, preserving reconciliation and holder visibility. The following results measure the complete `58a7bb613` bundle, not the separate effect of any one simplification.

The pinned Nixpkgs workload uses five paired cold trials and ten paired warm trials per case. Cold means a fresh private store and fetcher cache, with warm operating-system caches; GitHub cold timings include network access. Reductions in the median paired wall-time ratio against branch base are:

| Workload | `path:` | `git+file:` | `github:` |
| --- | ---: | ---: | ---: |
| Cold · 32 attributes | 3.22% | 16.94% | 27.40% |
| Warm · 8 attributes | 86.92% | 5.70% | 12.81% |
| Warm · 32 attributes | 72.88% | 3.98% | 7.72% |

Cold path wins four of five pairs, warm Git8 nine of ten and warm Git32 eight of ten; all other retained pairs are faster. These wall-time gains retain resource costs: cold-path CPU rises from 3.15 to 6.40 s and peak RSS from 445.11 to 500.52 MiB against branch base. CPU is also higher for cold Git and both warm Git cases.

The [direct resource comparison against `d600a9662`](09-performance.md#measurements) retains every case and all 156 outputs. Cold-path CPU medians fall 6.55→6.26 s and peak RSS 517.09→498.77 MiB, but paired wall time rises 98.7 ms (+3.09%; only 1/5 pairs faster). Warm Git32 has lower peak RSS but paired wall time rises 24.1 ms and CPU 0.02 s. Other cases are mixed or have small differences relative to observed run variation. Paired differences and differences of separate medians are distinct statistics; neither identifies allocation ownership or the cause of a bundle's resource change. Supplemental stores were not independently graph-walked.

Earlier exact-`d600a9662` phase and user-cycle profiles remain attributed to that revision in the [remaining-cost analysis](09-performance.md#remaining-cost-measured-separately). They expose raw worktree naming, materialisation and SQLite routes; user-cycle samples do not attribute kernel time or peak RSS.

## Evidence and reproduction

- [Performance report](09-performance.md) and [latest sample record](runs/2026-09-21-performance/paired-58a7bb613.json): exact revisions, binaries, input provenance, raw samples, commands, resource usage and graph-check results.
- [Nixpkgs evaluation driver](runs/nixpkgs-evaluation.py): all three URI schemes, cold 32-attribute and warm 8-/32-attribute cases, balanced executable order and explicit CPU affinity. Builds and profiles run separately from timing.
- [Complete store comparison](runs/compare-store-graphs.py) and [methodology counterchecks](runs/check-benchmark-methodology.py): graph/content/output equivalence and planted failures. Each cold store requires its own comparison; copying an indexed store changes holder inodes and is not a representative warm fixture.
- [Validation commands](05-validation.md) and [overlay host driver](runs/overlay-suite-on-host.sh): reproduce the applicable gates. Overlay tests must actually mount overlayfs; a skipped namespace probe is not a passing test.

When resuming, identify the tested binary and source tree before attributing any result to current code. Re-run the checks affected by subsequent changes, then repeat the complete optimized matrix and store comparisons. Keep regressions in the report until the measured results close them.
