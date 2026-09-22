# Lazy-store status

The current source passes the validation below, but the requirement that **every benchmark case beat the branch base remains open**. The comparison uses branch base `8aad447`, not a moving `master`. The latest measured revision, `16ed142be`, records eight lower medians and one regression in its [optimized comparison](runs/2026-09-21-performance/paired-16ed142be.json). Warm 32-attribute Git remains 8.53% slower; the small warm eight-attribute Git improvement has weak paired evidence.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash preserves that meaning, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file blobs live in store paths themselves. The database retains candidate holders per object, mode and ingestion; lookup considers only registered valid paths. Reuse checks physical metadata and records the recipient's own witness. Publication reconciles concurrent inode classes before exposing candidates, and a complete re-ingestion replaces that path's previous projection. Trees and symlink-target blobs live in the auxiliary object namespace. Holds protect typed auxiliary addresses through repair and collection; temporary roots belong to individual live store handles.

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Source-to-store memos retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Completed validation and limits

The latest completed debug checks report **2,456 passing unit tests and two existing runtime skips**: 439 evaluator, 149 fetcher, 888 utility and 980 store tests. Three pre-existing disabled tests remain excluded. All 888 utility tests also pass with newer syscalls disabled. Five targeted object-store/hash/optimisation/GC functional tests and **all twelve real overlay tests pass**. The optimized release's packaged checks report 207 passes and 36 skips out of 243 tests; the separate host overlay run exercises all twelve real mounts. The previously failing overlay assertions and registration dispatch are resolved. Overlay-specific limits, including no cross-layer regular-holder sharing and the disabled auxiliary verification/materialisation routes, remain explicit in the store model.

The [performance and verification report](09-performance.md#verification) records the finite-domain checks, negative controls, syscall evidence and source-review limits. The latest optimized comparison additionally passes all eighteen complete store-graph comparisons and binds all 156 evaluation outputs to their actual store pairs. These checks compare exact contents, modes, references and outputs under an injective substitution of store addresses. They do not establish equivalence of arbitrary embedded programs.

These results apply to aarch64 Linux. **Windows has not been runtime-verified.** The two unit-test runtime skips belong to the fetcher and utility suites. Older platform counts and experiments in [validation](05-validation.md) and the [holder-index investigation](06-holder-index.md) are evidence for their recorded implementations, not current cross-platform gates. The current validation and optimized measurements belong to `16ed142be`; older profiles remain explicitly attributed to their revisions.

The [validated source audit](09-performance.md#current-source-validation) reuses optional naming observations within the evaluator's existing invalidation boundary, keeps compound callbacks alive through completion, removes redundant forwarding ownership, preserves libgit2 callback cleanup, and enforces canonical stored-tree encoding. All 6,663 tree cases pass. Eight isolated faulty variants fail and their exact restorations pass. These changes are included in the completed optimized comparison below.

## Remaining performance work

The latest report uses pinned Nixpkgs, five paired cold trials and ten paired warm trials per case. Cold means a fresh private store and fetcher cache, with warm operating-system caches; GitHub cold timings include network access. The remaining median regression is:

| Warm workload | Base median | Branch median | Change in median elapsed time |
| --- | ---: | ---: | ---: |
| `git+file:` · 32 attributes | 1.403 s | 1.523 s | +8.53% |

The percentage uses the report's unrounded median ratio; the branch loses all ten pairs. The other eight cases have lower medians. Warm eight-attribute Git is 2.33% lower by ratio of medians, but wins only five of ten pairs and has a median paired ratio of 0.999: weak evidence, not a robust win. The all-cases-faster requirement remains unmet. Further work must preserve raw worktree observations, source ownership and GC protection while removing unnecessary work.

## Evidence and reproduction

- [Performance report](09-performance.md) and [latest sample record](runs/2026-09-21-performance/paired-16ed142be.json): exact revisions, binaries, input provenance, raw samples, commands, resource usage and graph-check results.
- [Nixpkgs evaluation driver](runs/nixpkgs-evaluation.py): all three URI schemes, cold 32-attribute and warm 8-/32-attribute cases, balanced executable order and explicit CPU affinity. Builds and profiles run separately from timing.
- [Complete store comparison](runs/compare-store-graphs.py) and [methodology counterchecks](runs/check-benchmark-methodology.py): graph/content/output equivalence and planted failures. Each cold store requires its own comparison; copying an indexed store changes holder inodes and is not a representative warm fixture.
- [Validation commands](05-validation.md) and [overlay host driver](runs/overlay-suite-on-host.sh): reproduce the applicable gates. Overlay tests must actually mount overlayfs; a skipped namespace probe is not a passing test.

When resuming, identify the tested binary and source tree before attributing any result to current code. Re-run the checks affected by subsequent changes, then repeat the complete optimized matrix and store comparisons. Keep regressions in the report until the measured results close them.
