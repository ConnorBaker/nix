# Evaluation performance and preserved behavior

Latest measured runtime **a2077a82a3a4f86c249ae394857b8e9d8ebb3f6c** lowers all nine wall medians against fixed base **8aad447f0bace6be413963e897ad690247ad6aa2**, with **71/75 faster pairs**, while **three CPU and six peak-RSS medians rise**. Directly against **f8f029117efdfd33fb4a516780f1b079fa3ca0d0**, three separate wall medians and four paired ratios are lower, with **39/75 faster pairs**. Direct CPU medians are three lower, three equal and three higher; two RSS medians rise. These observations establish no universal resource improvement.

## Protocol and all eighteen measurements

The fixed domain evaluates Nixpkgs `6aefcda9401be8acc2b74244fb3b37520ea1f0a8` on aarch64 Linux. Each independent series retains five cold pairs per source and ten pairs per warm count/source: **156 evaluations, six excluded warm seeds and 75 retained pairs**. All scheduled observations remain. The [driver](runs/nixpkgs-evaluation.py) records commands, identities and samples. Cold means fresh private application state, not evicted OS caches; cold GitHub includes acquisition/network work. Target affinity does not reserve the host.

Rows show baseline→a207 separate medians. Wall changes distinguish candidate-median/baseline-median from the median of paired ratios, each minus one. CPU adds recorded user and system centiseconds per observation before taking the median; component medians need not add. RSS is peak resident memory, not allocated bytes. Rounded displays do not classify signs.

### Fixed branch base → a207

| Case | Wall s | Separate / paired | Faster pairs | CPU s | RSS MiB |
| --- | --- | --- | --- | --- | --- |
| cold 32 path | 3.509541→3.167673 | -9.741% / -9.741% | 4/5 | 3.150→5.500 | 444.55→500.51 |
| cold 32 git | 2.714084→2.185839 | -19.463% / -20.095% | 5/5 | 2.620→2.580 | 478.97→482.43 |
| cold 32 github | 11.886682→8.481818 | -28.644% / -25.959% | 4/5 | 17.620→5.330 | 990.50→974.31 |
| warm 8 path | 3.109468→0.383039 | -87.682% / -87.872% | 10/10 | 2.925→0.840 | 223.73→203.33 |
| warm 8 git | 0.844917→0.766229 | -9.313% / -11.129% | 10/10 | 0.780→1.255 | 254.10→260.49 |
| warm 8 github | 0.291135→0.254492 | -12.586% / -12.675% | 9/10 | 0.280→0.240 | 196.79→201.36 |
| warm 32 path | 3.617520→0.926667 | -74.384% / -74.234% | 10/10 | 3.460→1.360 | 444.55→426.20 |
| warm 32 git | 1.433239→1.323585 | -7.651% / -9.441% | 9/10 | 1.340→1.780 | 478.86→480.15 |
| warm 32 github | 0.869507→0.808800 | -6.982% / -7.376% | 10/10 | 0.855→0.795 | 444.13→446.69 |

Cold-path CPU rises **3.150→5.500 s** and RSS **444.55→500.51 MiB**, both higher in every pair. Both warm Git cases also have higher CPU in every pair. Cold Git system time is higher in every pair despite its lower total CPU median. Lower wall medians describe this batch, not every execution.

### Previous runtime f8 → a207

| Case | Wall s | Separate / paired | Faster pairs | CPU s | RSS MiB |
| --- | --- | --- | --- | --- | --- |
| cold 32 path | 3.034929→3.111187 | +2.513% / +2.513% | 0/5 | 5.260→5.320 | 494.31→492.33 |
| cold 32 git | 2.257496→2.318440 | +2.700% / +1.848% | 1/5 | 2.670→2.690 | 480.42→480.31 |
| cold 32 github | 5.019535→5.454911 | +8.674% / +3.380% | 2/5 | 5.240→5.240 | 974.58→810.52 |
| warm 8 path | 0.383568→0.383536 | -0.008% / +1.454% | 4/10 | 0.835→0.835 | 207.40→201.40 |
| warm 8 git | 0.775164→0.781431 | +0.809% / +0.740% | 4/10 | 1.265→1.265 | 261.38→260.48 |
| warm 8 github | 0.253551→0.255752 | +0.868% / -1.073% | 7/10 | 0.245→0.240 | 201.56→200.64 |
| warm 32 path | 0.924763→0.919373 | -0.583% / -0.573% | 7/10 | 1.370→1.360 | 428.25→426.20 |
| warm 32 git | 1.351140→1.311144 | -2.960% / -2.910% | 9/10 | 1.805→1.780 | 480.16→480.21 |
| warm 32 github | 0.786284→0.796835 | +1.342% / -0.010% | 5/10 | 0.775→0.780 | 446.00→446.17 |

Direct cold-path wall rises **2.513%** separately and paired, with **0/5 faster pairs**; CPU rises **5.260→5.320 s** while RSS falls **494.31→492.33 MiB**. Cold Git and GitHub wall also rise under both reductions. Warm path8 reverses from **−0.008% separately to +1.454% paired**; warm GitHub8/32 reverse in the other direction. Warm Git8 is higher by both wall reductions, while warm path32/Git32 are lower.

Direct CPU cases are **3 lower/3 equal/3 higher** separately versus **5/2/2** paired, with **34/18/23** lower/equal/higher pairs. Cold Git CPU is higher separately but lower paired; cold GitHub is equal separately but lower paired; warm Git8 is equal separately but higher paired. RSS cases are **7/0/2** under both reductions, with **41/0/34** pairs; warm Git32 and GitHub32 are higher, and the latter is higher in every pair. Lower RSS medians do not identify allocation volume or ownership.

Direct user cases are **5/2/2** under both reductions; system cases are **6/0/3** separately versus **7/0/2** paired. Cold-path system time reverses from higher separately to lower paired. The [complete review](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-a2077a82a/COMPLETED-REVIEW.md), [ninety-cell export](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-a2077a82a/METRIC-CELLS.csv), [exact result](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-a2077a82a/result.json) and [independent review](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-a2077a82a/peer/REVIEW.json) retain every metric, range, tie and all ten classification differences. Checks cover **312 observations, 150 pairs, ninety cells and 1,481 numeric fields**. The two series are not pooled; neither statistical significance nor an individual change's speed effect follows from these finite batches.

## Source meaning, implementation and compiled evidence

The [specification](01-specification.md), [derivation](04-derivation.md) and [store model](08-store-model.md) define identities, contents and ownership. `path:` reads mutable filesystem bytes. Local `git+file:` without ref/rev selects fixed tracked membership over live filesystem bytes, with no revision-derived fingerprint. Pinned GitHub uses the archive cache and its object-backed Git accessor. [Clean status and raw-byte identity differ](/home/connorbaker/Benchmarks/nix-performance-fixes/job-permission-cloexec-validation/source-audits/warm-git-status-traversal.md): staged changes, clean filters, index flags, deletions, conflicts and gitlinks prevent substituting status metadata for traversal. This rules out the inspected deletions, not every possible optimization.

Source-to-store memos retain addresses, so destination reuse still roots and checks the requested path. Completed evaluator requests describe an earlier operation; filtered, asserted, reference-bearing and repair requests bypass that memo. Regular holders require physical and valid-path checks. Owned directory/symlink bodies belong to complete ingestion projections, with leases and stale-ingestion collection; overlay mutation stays upper-only. Hashing/materialisation retain ordered child completion, first-failure selection and separate concurrency permissions. Resource-failure traces and optional early effects are not asserted identical.

The measured five-file cohort has three changes. CopyPool passes the already-removed two-word Job by value to `execute`, retaining its borrowed group lifetime and existing lock/join/failure protocol. Materialisation obtains the default worker count only after callbacks, filter and accessor permit concurrency; a denied operation still uses one worker, and the existing test override remains. Unix sockets already request atomic `SOCK_CLOEXEC` where available, so that branch omits the redundant post-creation close-on-exec call; the non-Windows branch without the flag retains it. The new test checks `FD_CLOEXEC` on the returned descriptor. These source changes do not by themselves quantify dynamic cost.

The [actual f8→a207 compiled comparison](/home/connorbaker/Benchmarks/nix-performance-fixes/assembly-job-permission-cloexec-a2077a82a-vs-f8/assembly/comparison/ROUTES.md) retains **297 union rows / 568 body observations**: selected bytes fall **179,828→179,700**, while all three whole DSO sizes stay unchanged. The eleven-body pool family falls **7,444→7,428 bytes**; Job backing stores and descriptor reloads disappear. `execute` frame grows **80→96 bytes**, while `work` falls **288→272** and `wait` **112→96**. Local caller+execute segments remain **368/192 bytes**, excluding imported calls and reentrant nesting.

The three socket bodies fall **1,136→1,024 bytes**, and both formerly emitted descriptor-flag query/set pairs disappear. Countervailing costs remain: bare creation's frame grows **48→64**, and the pathname overload grows **472→588 code bytes** through inlining. `materialiseThroughObjects` falls **1,152→1,128 bytes**, frame **480 unchanged**; its one `defaultWorkers` site now follows permissions and a zero override. Six support NOPs add 24 bytes and offset that store-family reduction. Existing lock, callback and exception cleanup remain. Static removals, frame shifts and restricted call reachability do not measure route frequency or explain the timing changes.

A subsequent [util-library control](/home/connorbaker/Benchmarks/nix-performance-fixes/cold-path-util-control-a2077a82a/README.md) kept the a207 executable and all 76 other startup libraries fixed. A207 util was slower than f8 util in **8/10 cold-path pairs**, with **+3.745% median paired wall ratio** and CPU medians **5.845→6.075 s**. All twenty complete outputs matched. Both execution orders had four adverse pairs out of five, but their magnitudes differed and host load rose. This is evidence against the util bundle's presumed benefit, not isolated attribution to Job passing. The current source restores `const Job &`; its new code generation and performance remain unmeasured here.

The inherited [libgit2 scratch change](/home/connorbaker/Benchmarks/nix-performance-fixes/libgit2-pack-scratch/INDEX.md) owns one lazy 1 MiB heap buffer per pack-write operation, reused across ordinary/delta-base compression and released on exit. Empty/precompressed-only operations avoid it; nested builders remain separate. Seven isolated controls pass; the broader 47-case run includes them and has 46 passes/one existing skip. Old production fails exactly the two intended allocation/reference controls after the same setup correction. Eight primary bodies total 4,736→4,720 bytes; `write_one` frame falls 208→192 while `write_pack` rises 160→176. Longer scratch lifetime proves no RSS reduction.

## Validation and evidence boundaries

The [actual debug validation](/home/connorbaker/Benchmarks/nix-performance-fixes/job-permission-cloexec-validation/validation/independent-review.json) reconciles **2,518 primary / 913 fallback passes**, zero failures, skips **2/1**, disabled **3/1**, all **104/37 mandatory cases**, **17 functionals** and **12 real overlays**. All seven stages pass. The [packaged functional suite](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-a2077a82a/packaged-gate.json) adds **207 passes, zero failures and 36 skips**. Skipped, disabled, unavailable-device and unexecuted platform cases are not coverage; the Linux fallback suite does not establish execution on a platform without `SOCK_CLOEXEC`, and allocator test values do not measure release resources.

The [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-a2077a82a/main-audit.json) checks 156 complete outputs and eighteen actual store-graph associations; [direct audit](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-a2077a82a/direct-audit.json) checks another 156 outputs against exact own-release/prior references, including drvPath, without supplemental graph walks. Independent output/order review checks both complete schedules and all 312 outputs. This is not arbitrary embedded-program equivalence. The [strict release proof](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-a2077a82a.json) and [dependency binding](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-a2077a82a/libgit2-binding.json) bind the measured source, binaries and selected libgit2. All current pipeline stages pass; the earlier [8eae checker failure and replacement](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-62146bc7c/prior-completion/effective-completion.json) remain historical.

## Historical diagnostics and limits

The [8eae FD attribution](/home/connorbaker/Benchmarks/nix-performance-fixes/fd-attribution-8eae31e61/analysis/README.md) retains all nine cases and 24 outputs. Cold-path extra `fcntl` is dominated by database-shm `F_SETLK` (**43,298→174,926**); warm Git instead adds input `F_GETFL`/`F_SETFD` while database locks fall. Cold `pwrite` growth is mainly database/WAL. Selected errors, restarts, unresolved FDs and short writes are zero. The [selected-libc review](/home/connorbaker/Benchmarks/nix-performance-fixes/libc-directory-assembly-8eae31e61/INDEX.md) confirms `fdopendir` can make the GETFL→SETFD pair; it does not uniquely attribute dynamic calls or elapsed time. These are 8eae diagnostics, not a207 measurements or SQL transaction counts.

The [8eae IP readout](/home/connorbaker/Benchmarks/nix-performance-fixes/ip-profile-8eae31e61/capture/analysis-corrected/READOUT.md) retains **27,428 samples: 24,789 user and 2,639 native kernel**. Capture/decoding and the corrected header analysis pass, but **strict pure-user quality fails**; the original failure and full mixed-mode partition are preserved. Unknowns and incomplete callers remain in denominators. Samples/periods are not calls or exact cycles, inclusive routes overlap, and neither FD counts nor profiles assign the incremental timing differences.

Older [8db syscall totals](/home/connorbaker/Benchmarks/nix-performance-fixes/syscall-counts-8dbd3414f-vs-base/analysis/README.md), [daa1 frame-pointer findings](/home/connorbaker/Benchmarks/nix-performance-fixes/resource-profile-user-fp-daa1ff079/independent-review/interpretation/README.md) and [6043 kernel findings](/home/connorbaker/Benchmarks/nix-performance-fixes/syscalls-6043a65c5-vs-base/interpretation/README.md) remain separate historical collections, with their overlap, unknown-frame and empty-chain limits. The branch [tarball cache](../../src/libfetchers/git-utils.cc) sets `pack.window=0`, and [patch 0003](../../packaging/patches/0003-packbuilder-correct-config.patch) skips delta preparation at zero, consistent with the historical base delta-creation versus branch compression routes without isolating a timing effect. Base libgit2 **1.9.7** versus branch **2.0.0-rc1 plus six patches** confounds the fixed-base comparison; direct f8→a207 holds that dependency constant. Base revision-keyed Git reuse and branch live-byte naming also follow different identity rules. No collection is pooled or treated as a universal resource ordering.
