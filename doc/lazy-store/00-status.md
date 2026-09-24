# Lazy-store status

Revision `e77d0c12e` has lower separate wall medians and median paired wall-time ratios in **all nine benchmark cases** against branch base `8aad447`, with **70 of 75 pairs faster**. The [complete optimized comparison](runs/2026-09-21-performance/paired-e77d0c12e.json) passes all 156 output checks and eighteen complete store comparisons. Median CPU remains higher in four cases and peak RSS in six. Further source investigation continues; changes after this measured revision remain unmeasured.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash preserves that meaning, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file blobs live in store paths themselves. The database retains candidate holders per object, mode and ingestion; lookup considers only registered valid paths. Reuse checks physical metadata and records the recipient's own witness. Publication reconciles concurrent inode classes before exposing candidates, and a complete re-ingestion replaces that path's previous projection. Trees and symlink-target blobs live in the auxiliary object namespace. Holds protect typed auxiliary addresses through repair and collection; temporary roots belong to individual live store handles.

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Source-to-store memos retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

Completed evaluator requests are a separate kind of fact: the result of an earlier operation, identified by resolved source, method and output name. Reuse does not identify current mutable bytes. Filtered, asserted, reference-bearing and repair requests bypass this memo; their existing validation and destination-reuse rules still apply. Mounting or explicit reset retires completed requests.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Completed validation and limits

The [measured e77 source](/home/connorbaker/Benchmarks/nix-performance-fixes/scoped-borrows-validation/README.md) passes **2,501 primary unit tests** (453 evaluator, 159 fetcher, 897 utility and 992 store), **897 fallback utility tests**, **eight functional targets** and **all twelve real overlay tests**. Two primary skips and three disabled cases remain separate; fallback repeats one of each. Actual namespace/mount/overlay-write probes precede those gates. The exact optimized release passes **207 packaged tests, zero failures and 36 skips**. Earlier failures and superseded corrections remain bound to their historical sources.

The [performance report](09-performance.md#verification) preserves finite-domain checks, faulty variants with exact restorations, source ownership evidence and their limits. Complete store comparisons check contents, modes, references and outputs under an injective substitution of store addresses; they do not establish equivalence of arbitrary embedded programs. The [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/paired-e77d0c12e-audit/README.md) independently checks recorded mappings and all 156 outputs, without repeating physical graph walks.

These results apply to aarch64 Linux; **Windows remains unverified**. Overlay's lack of cross-layer regular-holder sharing and disabled auxiliary verification/materialisation remain explicit in the store model. Original fixture/checker failures and corrected runs are retained in their archives, rather than counted as passes. Older [validation](05-validation.md) and [holder investigations](06-holder-index.md) are evidence for their recorded implementations.

## Performance and remaining costs

The pinned workload has five paired cold trials and ten paired warm trials per case. Cold means fresh private store/fetcher state with warm operating-system caches; GitHub cold timings include network access. Reductions in the **median paired wall-time ratio against branch base** are:

| Workload | `path:` | `git+file:` | `github:` |
| --- | ---: | ---: | ---: |
| Cold · 32 attributes | 16.33% | 17.87% | 40.23% |
| Warm · 8 attributes | 88.15% | 2.92% | 13.49% |
| Warm · 32 attributes | 74.44% | 3.17% | 7.92% |

Both warm Git cases win 8/10 pairs and warm GitHub32 wins 9/10; all other retained pairs are faster. Cold-path CPU rises 3.31→6.27 s and RSS 445.21→478.74 MiB; every cold-path pair uses more CPU and RSS. Cold Git and both warm Git cases also use more CPU. Lower wall time does not establish lower resource use.

The [direct e77/77 comparison](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-e77d0c12e-vs-77d874507-audit/README.md) checks another 156 outputs, including exact derivation paths. All nine separate wall medians are lower, but only four median paired ratios are lower and only 38/75 pairs are faster. CPU medians are higher in five cases, lower in two and tied in two; RSS medians are higher in six. Cold-path paired wall/CPU rise 61.33 ms/0.46 s while paired RSS falls 8.07 MiB (4/5 lower). These mixed observations do not establish a causal speedup for any individual change.

The measured six-file bundle removes unused workdir projections, borrows registration graph inputs, notifies workers after unlocking and borrows a local filter handler through the synchronous directory callback. [Compiled handler evidence](/home/connorbaker/Benchmarks/nix-performance-fixes/scoped-handler-e77d0c12e-compiled/ARCHIVE.md) shows one particular closure-construction allocation replaced by stack storage and a reference wrapper. It supplies no executed-call count or whole-operation allocation/RSS measure; the bundle timing does not isolate any one change. Further audits remain active.

The [four-file follow-up](/home/connorbaker/Benchmarks/nix-performance-fixes/blob-hasher-value-validation/README.md) stores the streaming hasher's SHA-256 state directly and declares OpenSSL as a public build dependency. Its full debug gates pass; installed-consumer validation and performance remain pending. The unstable C++ layout changes, so consumers must rebuild with the required OpenSSL declarations visible.

The [historical 77/795 comparison](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-77d874507-vs-795ae18d9-audit/README.md) has only 44/75 wall-time wins, six higher CPU medians and mixed RSS results. Cold-path paired wall rose 35.16 ms and CPU medians 6.26→6.49 s; paired RSS rose 0.11 MiB despite lower separate RSS medians. Earlier [795/83 regressions](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-795ae18d9-vs-83d0a172d-audit/README.md) remain retained separately.

The [historical 77/795 syscall census](/home/connorbaker/Benchmarks/nix-performance-fixes/syscalls-77d874507-vs-795ae18d9/README.md) records 2,058,336→2,043,036 selected calls: database writes and hold-descriptor duplication fell, while reads and auxiliary locking rose; all nine source-tree call kinds were unchanged. The [77/base user-cycle profiles](/home/connorbaker/Benchmarks/nix-performance-fixes/resource-profile-77d874507/ARCHIVE.md) retain 20 captures, 22 outputs and 32,024 samples, with two base frame-cap hits and other incomplete chains. These older observations do not measure e77 or assign system CPU/RSS costs.

## Evidence and reproduction

- [Performance report](09-performance.md), [current sample record](runs/2026-09-21-performance/paired-e77d0c12e.json) and [release provenance](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-e77d0c12e.json): exact source/binaries, inputs, raw observations and validation.
- [Evaluation driver](runs/nixpkgs-evaluation.py): three URI schemes, cold 32 and warm 8/32 attributes, balanced order and CPU affinity. Builds/profiles run separately; shared-host exclusivity is not claimed.
- [Complete store comparison](runs/compare-store-graphs.py) and [methodology counterchecks](runs/check-benchmark-methodology.py): exact graph/content/output checks and planted faults. Each cold store has its own comparison; copying an indexed store changes holder inodes and is not a representative warm fixture.
- [Validation commands](05-validation.md) and [overlay host driver](runs/overlay-suite-on-host.sh): actual mount tests are required; a skipped probe is not a pass.

When resuming, bind each result to its tested source and binary. Re-run affected checks and the full optimized comparison after runtime changes; retain regressions until subsequent measurements resolve them.
