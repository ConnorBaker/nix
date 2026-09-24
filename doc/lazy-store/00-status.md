# Lazy-store status

Revision `77d874507` has lower median paired wall-time ratios in **all nine benchmark cases** against branch base `8aad447`, with **70 of 75 pairs faster**. Eight separate wall medians are lower; cold GitHub instead rises 7.856→8.751 s despite a lower paired ratio. The [complete optimized comparison](runs/2026-09-21-performance/paired-77d874507.json) passes all 156 output checks and eighteen complete store comparisons. Median CPU remains higher in three cases and peak RSS in six. Changes after this measured revision remain unmeasured.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash preserves that meaning, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file blobs live in store paths themselves. The database retains candidate holders per object, mode and ingestion; lookup considers only registered valid paths. Reuse checks physical metadata and records the recipient's own witness. Publication reconciles concurrent inode classes before exposing candidates, and a complete re-ingestion replaces that path's previous projection. Trees and symlink-target blobs live in the auxiliary object namespace. Holds protect typed auxiliary addresses through repair and collection; temporary roots belong to individual live store handles.

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Source-to-store memos retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

Completed evaluator requests are a separate kind of fact: the result of an earlier operation, identified by resolved source, method and output name. Reuse does not identify current mutable bytes. Filtered, asserted, reference-bearing and repair requests bypass this memo; their existing validation and destination-reuse rules still apply. Mounting or explicit reset retires completed requests.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Completed validation and limits

The [measured 77 source](/home/connorbaker/Benchmarks/nix-performance-fixes/resource-identities-validation/README.md) passes **2,501 primary unit tests** (453 evaluator, 159 fetcher, 897 utility and 992 store), **897 fallback utility tests**, **eight functional targets** and **all twelve real overlay tests**. Two primary skips and three disabled cases remain separate; fallback repeats one of each. Actual namespace/mount/overlay-write probes precede the functional and overlay gates. The exact optimized release passes **207 packaged tests, zero failures and 36 skips**. The initial callback-relocation failure and superseded correction remain separately retained.

The [performance report](09-performance.md#verification) preserves finite-domain checks, faulty variants with exact restorations, source ownership evidence and their limits. Complete store comparisons check contents, modes, references and outputs under an injective substitution of store addresses; they do not establish equivalence of arbitrary embedded programs. The [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/paired-77d874507-audit/README.md) independently checks recorded mappings and all 156 outputs, without repeating physical graph walks.

These results apply to aarch64 Linux; **Windows remains unverified**. Overlay's lack of cross-layer regular-holder sharing and disabled auxiliary verification/materialisation remain explicit in the store model. Original fixture/checker failures and corrected runs are retained in their archives, rather than counted as passes. Older [validation](05-validation.md) and [holder investigations](06-holder-index.md) are evidence for their recorded implementations.

## Performance and remaining costs

The pinned workload has five paired cold trials and ten paired warm trials per case. Cold means fresh private store/fetcher state with warm operating-system caches; GitHub cold timings include network access. Reductions in the **median paired wall-time ratio against branch base** are:

| Workload | `path:` | `git+file:` | `github:` |
| --- | ---: | ---: | ---: |
| Cold · 32 attributes | 12.71% | 18.95% | 28.89% |
| Warm · 8 attributes | 88.14% | 7.63% | 12.71% |
| Warm · 32 attributes | 76.05% | 5.70% | 7.70% |

Cold GitHub wins 4/5 pairs, warm Git8 and GitHub8 each win 9/10, and warm Git32 wins 8/10; every other retained pair is faster. Cold GitHub's ratio of separate wall medians is 1.114, opposite to its median paired ratio of 0.711. All observations remain included. Cold-path CPU is 3.44→6.14 s and RSS 445.16→486.62 MiB; both warm Git cases also use more CPU.

The [direct comparison with 795](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-77d874507-vs-795ae18d9-audit/README.md) checks another 156 outputs, including exact derivation paths. Only 44/75 wall-time pairs improve: five median paired ratios are lower and four higher. CPU medians are lower in two cases, higher in six and tied in one. RSS has six lower separate medians but only five lower median paired differences. Cold-path paired wall rises 35.16 ms (2/5 faster), CPU medians rise 6.26→6.49 s, and paired RSS rises 0.11 MiB despite lower separate RSS medians. The [centisecond correction](/home/connorbaker/Benchmarks/nix-performance-fixes/cpu-centiseconds-77d874507/result.json) preserves exact recorded CPU ties; previous raw floats remain retained.

The measured 77 bundle removes duplicate pending keys, blob rowids and tree-name copies, bounds stored-tree framing to parsing, and consumes the existing hold descriptor during cleanup. Callbacks borrow live call-frame functions so recursion preserves their identity and state. The full bundle comparison does not isolate any one change. The separate [six-file follow-up](/home/connorbaker/Benchmarks/nix-performance-fixes/scoped-borrows-validation/README.md) removes unused workdir projections, borrows registration graph inputs, notifies workers after unlocking and passes a borrowed local filter handler through synchronous directory callbacks. Its debug gates pass 2,501 primary and 897 fallback unit tests, eight functional targets and twelve overlay tests; performance remains unmeasured. The [previous 795/83 comparison](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-795ae18d9-vs-83d0a172d-audit/README.md) retains six higher paired wall medians, cold-path CPU and warm-GitHub32 wall/RSS regressions. Contrasts are not pooled.

The [complete selected-syscall census for 77/795](/home/connorbaker/Benchmarks/nix-performance-fixes/syscalls-77d874507-vs-795ae18d9/README.md) records 2,058,336→2,043,036 calls. Database `pwrite64` falls 175,323→168,657 and hold-descriptor duplication 553→0, while database `pread64` rises 12,594→13,083 and auxiliary-GC-lock `flock` rises 26,822→27,358. All nine source-tree syscall kinds are unchanged (562,404 calls per side). The single traced pair locates operations without assigning kernel CPU cost; the [older 83/base census](/home/connorbaker/Benchmarks/nix-performance-fixes/syscalls-83d0a172d/README.md) remains separately retained. The [exact 77 user-cycle profiles](/home/connorbaker/Benchmarks/nix-performance-fixes/resource-profile-77d874507/ARCHIVE.md) retain 20 captures, 22 checked outputs and 32,024 samples. They show materialisation/SQLite routes in cold path and raw-worktree hashing in warm Git, with overlapping inclusive shares and incomplete caller chains. Two base cold chains hit the 1,024-frame cap; captured stacks can also clip at large local buffers. These samples cannot attribute system CPU or peak RSS.

## Evidence and reproduction

- [Performance report](09-performance.md), [current sample record](runs/2026-09-21-performance/paired-77d874507.json) and [release provenance](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-77d874507.json): exact source/binaries, inputs, raw observations and validation.
- [Evaluation driver](runs/nixpkgs-evaluation.py): three URI schemes, cold 32 and warm 8/32 attributes, balanced order and CPU affinity. Builds/profiles run separately; shared-host exclusivity is not claimed.
- [Complete store comparison](runs/compare-store-graphs.py) and [methodology counterchecks](runs/check-benchmark-methodology.py): exact graph/content/output checks and planted faults. Each cold store has its own comparison; copying an indexed store changes holder inodes and is not a representative warm fixture.
- [Validation commands](05-validation.md) and [overlay host driver](runs/overlay-suite-on-host.sh): actual mount tests are required; a skipped probe is not a pass.

When resuming, bind each result to its tested source and binary. Re-run affected checks and the full optimized comparison after runtime changes; retain regressions until subsequent measurements resolve them.
