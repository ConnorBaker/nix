# Lazy-store status

The correctness fixes have passed the checks below, but the requirement that **every benchmark case beat the branch base remains open**. The comparison uses branch base `8aad447`, not a moving `master`. The latest [optimized comparison](runs/2026-09-21-performance/paired-e6d34dfdc.json) records six faster cases and three regressions. The subsequent fourth-pass changes have passed correctness checks but are not covered by those performance measurements.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash preserves that meaning, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file blobs live in store paths themselves. The database retains candidate holders per object, mode and ingestion; lookup considers only registered valid paths. Reuse checks physical metadata and records the recipient's own witness. Publication reconciles concurrent inode classes before exposing candidates, and a complete re-ingestion replaces that path's previous projection. Trees and symlink-target blobs live in the auxiliary object namespace. Holds protect typed auxiliary addresses through repair and collection; temporary roots belong to individual live store handles.

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Naming caches retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Completed validation and limits

The latest completed debug checks report **2,421 passing unit tests and two existing skips**: 435 evaluator, 145 fetcher, 869 utility and 972 store tests. The utility suite also passes with newer syscalls disabled. Five targeted object-store/hash/optimisation/GC functional tests and **all twelve real overlay tests pass**. The previously failing overlay assertions and registration dispatch are resolved. Overlay-specific limits, including no cross-layer regular-holder sharing and the disabled auxiliary verification/materialisation routes, remain explicit in the store model.

The [performance and verification report](09-performance.md#verification) records the finite-domain checks, negative controls, syscall evidence and source-review limits. The latest optimized comparison additionally passes all eighteen complete store-graph comparisons and binds all 156 evaluation outputs to their actual store pairs. These checks compare exact contents, modes, references and outputs under an injective substitution of store addresses. They do not establish equivalence of arbitrary embedded programs.

These results apply to aarch64 Linux. **Windows has not been runtime-verified.** Older platform counts and experiments in [validation](05-validation.md) and the [holder-index investigation](06-holder-index.md) are evidence for their recorded implementations, not current cross-platform gates. Pending changes need their own checks; a passing earlier binary is not evidence for an edited source tree.

## Remaining performance work

The latest report uses pinned Nixpkgs, five paired cold trials and ten paired warm trials per case. Cold means a fresh private store and fetcher cache, with warm operating-system caches; GitHub cold timings include network access. The three remaining regressions are:

| Warm workload | Base median | Branch median | Change in median elapsed time |
| --- | ---: | ---: | ---: |
| `git+file:` · 8 attributes | 0.815 s | 0.895 s | +9.7% |
| `git+file:` · 32 attributes | 1.366 s | 1.600 s | +17.2% |
| `github:` · 32 attributes | 0.840 s | 0.876 s | +4.2% |

The percentages are derived from the report's unrounded median ratios. All three cold cases, both warm `path:` cases and warm eight-attribute GitHub improve by median elapsed time. This does not satisfy the all-cases-faster requirement. Current profiling and implementation work must preserve raw worktree observations, source ownership and GC protection while removing unnecessary work.

## Evidence and reproduction

- [Performance report](09-performance.md) and [latest sample record](runs/2026-09-21-performance/paired-e6d34dfdc.json): exact revisions, binaries, input provenance, raw samples, commands, resource usage and graph-check results.
- [Nixpkgs evaluation driver](runs/nixpkgs-evaluation.py): all three URI schemes, cold 32-attribute and warm 8-/32-attribute cases, balanced executable order and explicit CPU affinity. Builds and profiles run separately from timing.
- [Complete store comparison](runs/compare-store-graphs.py) and [methodology counterchecks](runs/check-benchmark-methodology.py): graph/content/output equivalence and planted failures. Each cold store requires its own comparison; copying an indexed store changes holder inodes and is not a representative warm fixture.
- [Validation commands](05-validation.md) and [overlay host driver](runs/overlay-suite-on-host.sh): reproduce the applicable gates. Overlay tests must actually mount overlayfs; a skipped namespace probe is not a passing test.

When resuming, identify the tested binary and source tree before attributing any result to current code. Re-run the checks affected by subsequent changes, then repeat the complete optimized matrix and store comparisons. Keep regressions in the report until the measured results close them.
