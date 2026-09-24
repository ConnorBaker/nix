# Lazy-store status

Revision `795ae18d9` has lower separate wall medians and lower median paired ratios in **all nine benchmark cases** against branch base `8aad447`, with **70 of 75 pairs faster**. The [complete optimized comparison](runs/2026-09-21-performance/paired-795ae18d9.json) passes all 156 output checks and eighteen complete store comparisons. Median CPU remains higher in three cases and peak RSS in six; these elapsed-time improvements do not imply uniformly lower resource use. Changes after this measured revision remain unmeasured.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash preserves that meaning, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file blobs live in store paths themselves. The database retains candidate holders per object, mode and ingestion; lookup considers only registered valid paths. Reuse checks physical metadata and records the recipient's own witness. Publication reconciles concurrent inode classes before exposing candidates, and a complete re-ingestion replaces that path's previous projection. Trees and symlink-target blobs live in the auxiliary object namespace. Holds protect typed auxiliary addresses through repair and collection; temporary roots belong to individual live store handles.

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Source-to-store memos retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

Completed evaluator requests are a separate kind of fact: the result of an earlier operation, identified by resolved source, method and output name. Reuse does not identify current mutable bytes. Filtered, asserted, reference-bearing and repair requests bypass this memo; their existing validation and destination-reuse rules still apply. Mounting or explicit reset retires completed requests.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Completed validation and limits

The [current source archive](/home/connorbaker/Benchmarks/nix-performance-fixes/resource-identities-validation/README.md) passes **2,501 primary unit tests**, **897 fallback utility tests**, **eight functional targets** and **all twelve real overlay tests**. The added boundaries cover pending-key projection, transactional migration in both journal modes, recovered insertion errors and stateful recursive callbacks. It retains the initial callback-relocation failure and the superseded local correction separately. This source removes duplicate pending keys, blob rowids, callback storage and tree-name copies; its optimized performance remains unmeasured.

The [measured 795 source](/home/connorbaker/Benchmarks/nix-performance-fixes/resource-complete-validation/README.md) passes **2,498 primary unit tests** (453 evaluator, 159 fetcher, 895 utility and 991 store), **895 syscall-fallback utility tests**, **eight functional targets** and **all twelve real overlay tests**. Two runtime skips and three disabled cases remain separate. Actual namespace/mount/overlay-write probes precede the functional and overlay gates. The exact optimized release also passes 207 packaged tests, with zero failures and 36 skips.

The [performance report](09-performance.md#verification) preserves finite-domain checks, faulty variants with exact restorations, source ownership evidence and their limits. Complete store comparisons check contents, modes, references and outputs under an injective substitution of store addresses; they do not establish equivalence of arbitrary embedded programs. The [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/paired-795ae18d9-audit/README.md) independently checks recorded mappings and all 156 outputs, without repeating physical graph walks.

These results apply to aarch64 Linux; **Windows remains unverified**. Overlay's lack of cross-layer regular-holder sharing and disabled auxiliary verification/materialisation remain explicit in the store model. Original fixture/checker failures and corrected runs are retained in their archives, rather than counted as passes. Older [validation](05-validation.md) and [holder investigations](06-holder-index.md) are evidence for their recorded implementations.

## Performance and remaining costs

The pinned workload has five paired cold trials and ten paired warm trials per case. Cold means fresh private store/fetcher state with warm operating-system caches; GitHub cold timings include network access. Reductions in the **median paired wall-time ratio against branch base** are:

| Workload | `path:` | `git+file:` | `github:` |
| --- | ---: | ---: | ---: |
| Cold · 32 attributes | 14.63% | 20.20% | 44.50% |
| Warm · 8 attributes | 88.35% | 5.46% | 11.72% |
| Warm · 32 attributes | 75.69% | 5.13% | 6.49% |

Both warm Git cases win 8/10 pairs and warm GitHub32 wins 9/10; every other retained pair is faster. CPU remains higher for cold path (3.43→6.72 s) and both warm Git cases. Cold-path peak RSS is 445.18→474.49 MiB. These resource regressions remain in the full report.

The [direct 795/83 comparison](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-795ae18d9-vs-83d0a172d-audit/README.md) retains another 156 exact output checks, including `drvPath`. It is mixed: only 32/75 pairs are faster, and six cases have higher median paired wall ratios. Cold path has paired wall −13.02 ms and RSS −12.50 MiB (both lower in 3/5 pairs), but CPU +0.17 s; its separate wall medians rise. Warm GitHub32 regresses by 9.44 ms paired wall and 1.82 MiB RSS, with RSS higher in all ten pairs. Supplemental stores were not graph-walked again; their complete outputs match both releases' case references.

The measured bundle keeps encoded directory bodies only through hashing/publication, writes already complete regular-file buffers without a second buffering layer, and constructs known child paths directly. These reductions preserve callback completion, reentrant ownership and canonical metadata; the bundle comparison does not isolate their costs. [Earlier comparisons](09-performance.md#measurement-limits-and-earlier-evidence), including f4's cold-path RSS regression and the focused 83/f4 tradeoff, remain separate and are not pooled.

The [complete selected-syscall census for 83/base](/home/connorbaker/Benchmarks/nix-performance-fixes/syscalls-83d0a172d/README.md) records 1,487,089→2,050,705 calls, with all selected records and pathname/descriptor observations resolved. Added work includes database locking/I/O, auxiliary publication and physical witnesses, despite fewer source-directory opens. This census predates 795 and locates operations without assigning kernel CPU cost. The older [58a7 user-cycle samples](09-performance.md#remaining-cost-measured-separately) have incomplete caller chains and cannot attribute kernel time or peak RSS. Neither diagnostic measures the subsequent unmeasured changes.

## Evidence and reproduction

- [Performance report](09-performance.md), [current sample record](runs/2026-09-21-performance/paired-795ae18d9.json) and [release provenance](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-795ae18d9.json): exact source/binaries, inputs, raw observations and validation.
- [Evaluation driver](runs/nixpkgs-evaluation.py): three URI schemes, cold 32 and warm 8/32 attributes, balanced order and CPU affinity. Builds/profiles run separately; shared-host exclusivity is not claimed.
- [Complete store comparison](runs/compare-store-graphs.py) and [methodology counterchecks](runs/check-benchmark-methodology.py): exact graph/content/output checks and planted faults. Each cold store has its own comparison; copying an indexed store changes holder inodes and is not a representative warm fixture.
- [Validation commands](05-validation.md) and [overlay host driver](runs/overlay-suite-on-host.sh): actual mount tests are required; a skipped probe is not a pass.

When resuming, bind each result to its tested source and binary. Re-run affected checks and the full optimized comparison after runtime changes; retain regressions until subsequent measurements resolve them.
