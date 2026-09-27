# Evaluation performance and preserved behavior

Latest measured runtime **8dbd3414f6d607fc378709faa7e0c80a23f08d24** lowers all nine wall medians against fixed branch base **8aad447f0bace6be413963e897ad690247ad6aa2**, with **68/75 faster pairs**. **Three CPU medians and seven peak-RSS medians rise.** Directly against **a12e3dfabba797bd81c3544568760f57af932ced**, six separate wall medians and six paired ratios are lower, with **44/75 faster pairs**; CPU medians are six lower, two equal and one higher, while six RSS medians rise. These combined changes establish no universal performance or resource improvement.

## Protocol and measurements

The fixed nine-case domain evaluates Nixpkgs `6aefcda9401be8acc2b74244fb3b37520ea1f0a8` on aarch64 Linux. Each series has five cold pairs per source and ten pairs per warm count/source: **156 evaluations, six excluded warm seeds and 75 retained pairs**. Every scheduled observation is retained. The [driver](runs/nixpkgs-evaluation.py) records commands, identities and samples. Cold means fresh private application state, not evicted OS caches; GitHub acquisition includes network work. Target affinity does not reserve the host.

Rows show baseline→8dbd separate medians. Wall changes distinguish the ratio of separate medians from the median of paired ratios, each minus one; negative is lower. CPU adds recorded user and system centiseconds for each observation before taking the median; component medians need not add. RSS is peak resident memory, not allocated bytes. Rounded displays do not decide signs.

### Fixed branch base → 8dbd

| Case | Wall s | Wall change separate / paired | Faster pairs | CPU s | RSS MiB |
| --- | --- | --- | --- | --- | --- |
| cold 32 path | 4.095549→3.801950 | -7.169% / -14.628% | 4/5 | 3.420→5.520 | 439.86→484.38 |
| cold 32 git | 3.093098→2.628947 | -15.006% / -14.153% | 4/5 | 2.790→2.710 | 479.10→478.50 |
| cold 32 github | 6.990010→4.224880 | -39.558% / -41.454% | 5/5 | 17.810→5.420 | 941.65→949.59 |
| warm 8 path | 3.592236→0.639032 | -82.211% / -82.747% | 10/10 | 3.105→0.820 | 223.82→207.78 |
| warm 8 git | 0.918399→0.783594 | -14.678% / -11.942% | 10/10 | 0.815→1.270 | 254.33→259.11 |
| warm 8 github | 0.306117→0.263502 | -13.921% / -14.372% | 10/10 | 0.290→0.250 | 197.04→200.51 |
| warm 32 path | 4.239329→1.062735 | -74.932% / -71.734% | 10/10 | 3.605→1.385 | 392.10→424.86 |
| warm 32 git | 1.475405→1.340140 | -9.168% / -4.651% | 7/10 | 1.390→1.815 | 478.92→480.15 |
| warm 32 github | 0.909042→0.838234 | -7.789% / -7.876% | 8/10 | 0.880→0.815 | 444.31→444.79 |

Cold-path wall is lower by both reductions, while CPU rises **3.420→5.520 s** and RSS **439.86→484.38 MiB**. Every cold-path pair has higher CPU and RSS; both warm Git cases also have higher CPU in every pair. Cold Git CPU falls overall but system time rises **1.170→1.270 s**. All nine lower wall medians describe this batch, not every execution.

### Previous runtime a12 → 8dbd

| Case | Wall s | Wall change separate / paired | Faster pairs | CPU s | RSS MiB |
| --- | --- | --- | --- | --- | --- |
| cold 32 path | 3.386493→3.205255 | -5.352% / -2.521% | 3/5 | 5.330→4.920 | 499.97→490.43 |
| cold 32 git | 2.781779→2.601610 | -6.477% / -6.477% | 3/5 | 2.710→2.660 | 480.05→480.46 |
| cold 32 github | 9.178208→4.361612 | -52.479% / -45.943% | 5/5 | 7.600→6.300 | 880.46→932.44 |
| warm 8 path | 0.388680→0.387309 | -0.353% / -0.710% | 7/10 | 0.840→0.840 | 205.00→203.12 |
| warm 8 git | 0.787756→0.798235 | +1.330% / +1.875% | 4/10 | 1.290→1.275 | 259.21→259.26 |
| warm 8 github | 0.260434→0.264068 | +1.395% / +0.941% | 4/10 | 0.250→0.250 | 199.82→200.46 |
| warm 32 path | 0.936221→0.936834 | +0.065% / +0.207% | 5/10 | 1.380→1.365 | 424.82→425.97 |
| warm 32 git | 1.375393→1.360736 | -1.066% / -0.184% | 6/10 | 1.805→1.810 | 480.83→479.78 |
| warm 32 github | 0.803770→0.794486 | -1.155% / -1.709% | 7/10 | 0.785→0.780 | 444.71→446.09 |

Direct cold GitHub wall falls **9.178208→4.361612 s**, with **5/5 faster pairs** and CPU **7.600→6.300 s**; RSS rises **880.46→932.44 MiB** and is higher in all five pairs. Network/acquisition variability remains in this five-pair case; the combined dependency and Nix changes do not isolate a compression effect. Warm Git8, warm GitHub8 and warm path32 have higher separate and paired wall medians. Warm path32 divides **5 faster / 5 slower** despite its higher medians.

Direct CPU cases are **6 lower/2 equal/1 higher** separately versus **5/2/2** paired. Warm path8 is equal separately but higher paired; warm path32 is lower separately but equal paired. Its system time reverses from higher separately to lower paired; its user time is equal separately and by paired delta, but its paired ratio is **0.031270% lower**. This near-tie shows that paired ratios and paired deltas can also differ. Direct CPU pairs are **36/15/24** and RSS **24/0/51**. RSS cases are **3/0/6** under both median reductions; warm GitHub8/32 RSS is higher in every pair.

The [complete numeric review](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-8dbd3414f/COMPLETED-REVIEW.md) and [exact result](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-8dbd3414f/result.json) retain all eighteen cases × five metrics, user/system CPU, ranges, dispersion, ties and reversals. Independent checking covers **312 raw observations, 150 pairs, ninety cells and 1,481 numeric fields**. Main and direct series remain separate. These finite observations establish neither significance nor causal attribution to one edit.

## Source meaning and actual routes

The [specification](01-specification.md), [derivation](04-derivation.md) and [store model](08-store-model.md) define identity, contents, effects and ownership. Source schemes exercise different paths:

| Source | Selection and data route |
| --- | --- |
| Path | Mutable filesystem accessor; naming/materialisation when required, then store accessor. |
| Local Git | `git+file://…?shallow=1`, without ref/rev, selects fixed tracked membership over mutable filesystem bytes, with no cached fingerprint. |
| GitHub | Pinned archive resolved through the canonical tarball cache and its object-backed Git accessor. |

The [fetcher selection](../../src/libfetchers/git.cc) and [accessors](../../src/libfetchers/git-utils.cc) prevent conflating immutable Git-object improvements with local-worktree behavior. Clean status does not prove byte identity: attributes and index flags affect comparisons. Repair skips the source-known regular shortcut; supplied tree namers can still run for roots/directories, while fetch-layer repair policy separately forces misses.

Base 8aad adds clean-worktree revision metadata and derives a revision-based fingerprint. The branch explicitly caches no fingerprint: revision metadata does not identify live or attribute-rendered bytes. Base revision-keyed reuse and the branch's repeated walks therefore follow different identity rules. This explains route selection without assigning elapsed time to sampled weights or claiming every repeated walk is unavoidable.

Six separate [base/daa1 debug evaluations](/home/connorbaker/Benchmarks/nix-performance-fixes/warm-git-route-base-daa1/nix-warm-git-route-diagnostic-base-daa1-interpretation/README.md) confirm persistent root-cache hits on both warm base runs and live-root hashing on both warm daa1 runs. Daa1 also completes 237/553 requested-subpath copy operations, with destinations and hashes matching its seed. These messages do not count physical writes: regular roots buffered within 1 MiB can return for an already-valid destination before creating temporary files. Directory and larger-file routes can still materialise temporary state. These diagnostics are not 8dbd timings or a fresh filesystem census.

The measured seven-file cohort gives traversal's directory operation a lexical scope: the root borrows its existing filter prefix, while a nonroot join lives through the complete directory callback and joined child visits. This removes the prior optional prefix owner without introducing a cache. FilteringSourceAccessor inherits the identical base fingerprint implementation; a filtered subtree still cannot claim the inner subtree's identity merely because it wraps it.

CopyPool snapshots its completed failure index after joining, so opaque consume callbacks do not force repeated loads of that fixed bound. Launch-failure priority, earliest child failure, ordered folding and exception cleanup remain. Hash construction initializes its width directly for the valid algorithm domain; the feature gate and final initialized value remain, without promising identical bytes in an object whose construction fails.

The packaged libgit2 change gives each pack-write operation one lazily allocated **1 MiB heap scratch buffer**, reused across ordinary compressed objects and recursive delta-base writes, then released at the operation's exit. Empty and precompressed-only writes need no scratch; nested distinct builders have distinct buffers. Callback/error cleanup retains ownership, including releasing the acquired ODB object if the first scratch allocation fails. This removes repeated allocation requests but extends a successful buffer's lifetime; it proves neither lower peak RSS nor unchanged allocation-failure traces.

The inherited SQL representation owns typed, chunked directory bodies and symlink targets per ingestion. Reads own and verify a complete body snapshot; reused bodies enter the recipient's projection. Leases and complete publication replace per-body files and auxiliary graph marking. Regular holders retain physical checks and valid-path eligibility; overlay mutation remains upper-only. Optional early-hit traces and resource failures are not asserted identical.

### Actual compiled evidence

The [completed release dependency binding](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-8dbd3414f/libgit2-binding.json) verifies the exact new libgit2 derivation, all six ordered committed patches, declared development dependency, registered dev/lib outputs, both fetchers/release runtime closures and the selected shared-library digest. It complements the unchanged strict Nix component-source proof; it is not a dynamic-loader trace or a timing attribution.

The retained [libgit2 comparison](/home/connorbaker/Benchmarks/nix-performance-fixes/libgit2-pack-scratch/files/tmp/nix-libgit2-scratch-assembly-v3/README.md) shows eight primary bodies totaling **4,736→4,720 bytes**. `write_one` shrinks **1,408→1,356 bytes**, with frame **208→192 bytes**; `write_pack` grows **1,484→1,520 bytes**, with frame **160→176 bytes**. One active `write_pack`/`write_one` pair therefore remains **368 frame bytes**; this is not a complete stack bound. The other six primary bodies and 27 immediate local support bodies retain their sizes. Lazy scratch ownership and cleanup are emitted; these static changes establish no execution frequency, cycle or RSS improvement.

The [isolated libgit2 controls](/home/connorbaker/Benchmarks/nix-performance-fixes/libgit2-pack-scratch/INDEX.md) pass all **seven new tests**. The broader pack/packed-ODB run includes those seven: **47 cases, 46 passed, one existing conditional skip, zero failures**. With the same corrected setup, original production passes five and fails exactly two intended controls: repeated scratch allocation and the acquired ODB reference left held after scratch-allocation failure. The initial seven setup failures are retained separately.

The completed [a12→8dbd util comparison](/home/connorbaker/Benchmarks/nix-performance-fixes/assembly-scoped-ownership-8dbd3414f/util/comparison/ROUTES-v2.md) selects **138→135 bodies**, totaling **67,168→67,224 bytes**, while the whole util ELF shrinks **192 bytes**. Hash construction shrinks **452→376 bytes**, frame **112→96**; BLAKE3 now looks up width before its feature check, so the smaller body does not mean every algorithm performs less work. CopyPool's ordered fold shrinks **1,124→1,120 bytes** with frame **224 unchanged**; the completed failure bound is loaded once across consume calls. Traversal instead grows **12,760→12,952 bytes**, despite **29→27 bodies**: callback inlining duplicates root/nonroot setup. The monostate and HashedNode direct wrapper/callee frame pairs increase **272→352** and **400→544 bytes**. Root-prefix equality now emits a `strlen` call; all four enclosing caller families retain their entry sizes and frames. These bounded code/frame observations establish neither a complete stack maximum nor a runtime cost attribution. The pending MemoVisitor lookup, exact-view root comparison and parsed-name checks are unmeasured and excluded.

## Validation and evidence boundaries

[Current validation](/home/connorbaker/Benchmarks/nix-performance-fixes/scoped-ownership-validation/REVIEW.md) independently reconciles **2,517 primary / 912 fallback passes**, zero failures, skips **2/1** and disabled **3/1**. All **103/36 required cases**, **17 functionals** and **12 real overlays** pass; configure, build and the five remaining stages all exit zero. The existing reentrant-exception, ordered-fold, callback-lifetime, SQL, naming and header oracles remain. The packaged release passes **207 tests**, zero failures and **36 skips**. Skipped/disabled/device-unavailable and Windows cases are not executed coverage; debug allocator observations do not measure release resources.

Validation records the new libgit2 headers/link input and the root's successful loader resolution to that library. The first configure attempt exited zero but was rejected for old headers before build/tests; its history remains separate. Isolated libgit2 regression results are not pooled with Nix unit counts. The optimized release's registered dependency association is independently checked before either timed matrix.

The [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-8dbd3414f/main-audit.json) checks 156 outputs and eighteen complete store-graph comparisons under injective address mappings. The runner walked graphs; subsequent artifact checking verifies association. The [direct audit](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-8dbd3414f/direct-audit.json) checks another 156 outputs against own-release and prior references, including drvPath; supplemental stores were not graph-walked. Independent completed-output checks rechecked both schedules, commands, time fields, reference mappings and all 312 complete outputs. This does not prove arbitrary embedded-program equivalence. The [strict release proof](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-8dbd3414f.json) binds actual measured component sources/outputs.

## Current syscall counts

The completed [8dbd versus fixed-base review](/home/connorbaker/Benchmarks/nix-performance-fixes/syscall-counts-8dbd3414f-vs-base/analysis/README.md) checks all **144 capture files, 24 outputs and 648 cells across 72 syscall labels**. There is one traced execution per side/case and six untraced seeds. These are aggregate Nix/descendant counts, separate from the timed matrices and the a12 comparison.

| Case | Calls base→8dbd | Delta |
| --- | ---: | ---: |
| cold 32 path | 1,487,667→1,723,218 | +235,551 |
| cold 32 git | 1,295,175→1,210,936 | -84,239 |
| cold 32 github | 668,626→737,553 | +68,927 |
| warm 8 path | 1,693,990→583,004 | -1,110,986 |
| warm 8 git | 388,149→860,964 | +472,815 |
| warm 8 github | 28,532→8,688 | -19,844 |
| warm 32 path | 1,727,869→604,753 | -1,123,116 |
| warm 32 git | 422,348→887,073 | +464,725 |
| warm 32 github | 55,604→24,514 | -31,090 |

Combined `openat`+`openat2` counts fall in seven cases and rise in both warm Git cases; treating `openat2` alone as removed opens would be wrong. Total reported errors fall in all nine, while individual families rise. These summaries have no path, FD, errno, thread, byte or SQL-statement partition and provide no cost attribution. The complete call/error matrices retain adverse and unchanged rows. Tracer timing is not used as a benchmark; concurrent artifact review and a 0.19-second formatter on helper CPUs are recorded, and affinity does not provide host isolation.

## Historical diagnostics and limits

The [daa1 user-FP interpretation](/home/connorbaker/Benchmarks/nix-performance-fixes/resource-profile-user-fp-daa1ff079/independent-review/interpretation/README.md) and [6043 kernel/syscall findings](/home/connorbaker/Benchmarks/nix-performance-fixes/syscalls-6043a65c5-vs-base/interpretation/README.md) are **historical, not 8dbd profiles**. Each full profile series has eighteen captures, six seeds and 24 output checks. One instrumented pair per case gives no variance estimate; these observations cannot establish the incremental effect of 8dbd.

The daa1 user-FP collection has **26,518 samples**, 4,607 missing user entries, 2,530 unknown leaves/13,253 unknown frames, 2,641 chains at exactly depth 127, maximum 128 and two zero-IP frames. The 6043 kernel collection has **24,948 samples**, one unknown leaf and one unknown frame, and 1,885 empty chains carrying 6.345344% of period.

Recorded loss/unparsed records and decoder/header stderr are zero, but callers remain incomplete. Exactly 127 is not proof of truncation. Unknown/empty samples stay in denominators; high-bit user unknowns are not assigned to kernel code. Inclusive routes overlap. Sample counts and period weights are not invocation counts or exact cycles; separate collections cannot be pooled. Endpoint identity and startup loader checks do not establish complete mappings or isolation.

Historical cold Git includes status and traversal; warm Git still traverses mutable bytes. Cold GitHub's delta construction/flush/compression differs from warm GitHub's evaluator/parser/inflate route. Base uses **libgit2 1.9.7**; 8dbd uses **2.0.0-rc1 (`ae45d0d`) plus six patches**. Fixed-base results compare dependency bundles. Direct a12→8dbd also changes the libgit2 patch set, alongside Nix source and code layout, so it does not isolate individual effects. Historical [allocator attribution](/home/connorbaker/Benchmarks/nix-performance-fixes/allocation-symbols-110adbaf7-complete-attribution/README.md) cautions that a shared operator-new[] label does not identify array-buffer cost or RSS ownership.
