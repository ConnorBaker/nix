# Evaluation performance and preserved behavior

Measured **f3079efbeb7889474ae31e08ff8a04dec4541bc4**, built from [sealed source 0ffeaf50](/home/connorbaker/Benchmarks/nix-performance-fixes/combined-simplification-20260929/release-snapshot/SNAPSHOT.json), has lower wall medians in all nine cases against branch base **8aad** under both statistics, with **71/75** faster pairs. This does not meet the uniform-performance goal: cold-path and both warm-Git CPU measurements are higher in every pair, and three RSS medians rise. Against preceding release **U/f2fe32368**, the results are mixed; five paired wall medians and four paired CPU medians are higher. The source remains **9,570 physical runtime-source lines above 8aad**. Historical U and 469699 evidence appears below with its original identities.

## Current complete repeated measurements

Both comparisons evaluate Nixpkgs `6aefcda9401be8acc2b74244fb3b37520ea1f0a8` on aarch64 Linux through `path:`, live-worktree unpinned `git+file:`, and pinned `github:`. Each reference has five cold pairs per scheme and ten warm pairs per width/scheme (8 and 32 attributes): 156 evaluations including six seeds excluded from timing aggregation, 150 measured observations and 75 pairs. The evaluator cache is disabled. Cold means fresh private application state, not evicted OS caches; cold GitHub includes acquisition/network work. Affinity does not reserve the shared host.

[Completion 4e3894e2](/home/connorbaker/Benchmarks/nix-performance-fixes/combined-simplification-20260929/comparison/COMPLETE.json) binds the [release/source/binaries c43b9d51](/home/connorbaker/Benchmarks/nix-performance-fixes/combined-simplification-20260929/comparison/BINDING.json), [8aad report 8d6212cd](/home/connorbaker/Benchmarks/nix-performance-fixes/combined-simplification-20260929/comparison/vs-8aad.json), and [U report a2442081](/home/connorbaker/Benchmarks/nix-performance-fixes/combined-simplification-20260929/comparison/vs-f2fe32368.json). All 312 outputs pass. Eighteen base graph-verification records cover every base-comparison sample and establish equivalent complete contents after address rewriting. All 156 direct-U output values, and all 78 candidate values in the base comparison, equal the retained full-output oracle. The [independent audit 7bc0ee85](/home/connorbaker/Benchmarks/nix-performance-fixes/combined-simplification-20260929/numeric-audit/RESULT.json) rechecks all eighteen case/reference cells, ninety metric cells and complete schedules; it checks retained graph-verification records without rewalking graphs. Verified first-phase cold scratch states were retired with a retained receipt; outputs and graph records remain bound.

Changes below are percentages, **separate/paired**, rounded to three decimals; negative is lower. Separate divides candidate/reference medians; paired takes the median of matched-trial ratios. L/E/H counts exact candidate lower/equal/higher observations. CPU sums user/system centiseconds per observation before aggregation; component medians need not add. RSS is peak resident memory, not allocation volume. No observation is excluded or pooled with another cohort.

### Against branch base 8aad

| Case | Wall separate/paired | Wall L/E/H | CPU separate/paired | CPU L/E/H | RSS separate/paired |
|---|---:|---:|---:|---:|---:|
| cold 32 path | -23.272%/-23.270% | 5/0/0 | +46.106%/+48.607% | 0/0/5 | +9.788%/+12.780% |
| cold 32 git | -19.660%/-17.526% | 5/0/0 | -4.000%/-4.240% | 3/0/2 | -1.084%/-1.048% |
| cold 32 github | -17.554%/-16.232% | 4/0/1 | -67.367%/-68.326% | 5/0/0 | -18.666%/-16.565% |
| warm 8 path | -88.122%/-88.279% | 10/0/0 | -70.833%/-71.052% | 10/0/0 | -13.948%/-13.950% |
| warm 8 git | -8.051%/-8.319% | 10/0/0 | +56.250%/+56.250% | 0/0/10 | +1.401%/+1.400% |
| warm 8 github | -14.712%/-14.282% | 10/0/0 | -14.754%/-14.961% | 10/0/0 | +2.169%/+2.171% |
| warm 32 path | -75.288%/-75.337% | 10/0/0 | -59.064%/-58.981% | 10/0/0 | -6.948%/-6.023% |
| warm 32 git | -5.543%/-4.277% | 9/0/1 | +32.847%/+32.978% | 0/0/10 | -0.706%/-0.707% |
| warm 32 github | -9.502%/-7.543% | 8/0/2 | -8.523%/-7.252% | 10/0/0 | -3.217%/-3.216% |

Cold-path CPU rises **+46.106%/+48.607%**, warm-8 Git **+56.250%/+56.250%**, and warm-32 Git **+32.847%/+32.978%**; every pair is higher in each domain. RSS rises for cold path, warm-8 Git and warm-8 GitHub; cold path and warm-8 GitHub are higher in every pair. Lower wall time does not erase these costs.

### Against U/f2fe32368

| Case | Wall separate/paired | Wall L/E/H | CPU separate/paired | CPU L/E/H | RSS separate/paired |
|---|---:|---:|---:|---:|---:|
| cold 32 path | -11.243%/-11.119% | 5/0/0 | -5.794%/-5.009% | 4/0/1 | -0.386%/-1.667% |
| cold 32 git | +1.257%/-1.652% | 3/0/2 | -0.722%/-2.787% | 3/0/2 | -0.411%/-0.007% |
| cold 32 github | +1.118%/+4.820% | 2/0/3 | +5.700%/+2.146% | 2/0/3 | +1.694%/+1.201% |
| warm 8 path | -0.228%/-0.432% | 6/0/4 | -0.599%/-0.581% | 5/2/3 | -0.588%/-0.659% |
| warm 8 git | -0.743%/+0.526% | 4/0/6 | +1.200%/-0.403% | 5/1/4 | +0.027%/+0.071% |
| warm 8 github | +2.845%/+2.241% | 4/0/6 | +4.000%/+3.923% | 0/4/6 | +0.436%/+0.422% |
| warm 32 path | -1.300%/-0.820% | 6/0/4 | -0.717%/+0.000% | 3/3/4 | -0.013%/-0.270% |
| warm 32 git | +1.705%/+1.676% | 3/0/7 | +0.820%/+0.832% | 3/0/7 | +0.374%/+0.231% |
| warm 32 github | +2.770%/+0.388% | 5/0/5 | +1.818%/+0.575% | 2/3/5 | +0.087%/+0.080% |

Paired wall medians regress for **cold GitHub, warm-8 Git, warm-8 GitHub, warm-32 Git and warm-32 GitHub**; four improve, with **38/75** faster pairs. Separate wall medians instead regress for cold Git, cold GitHub, warm-8 GitHub, warm-32 Git and warm-32 GitHub: cold Git reverses **+1.257%/−1.652%**, while warm-8 Git reverses **−0.743%/+0.526%**. Both views remain in the table.

Paired CPU medians regress for **cold GitHub, warm-8 GitHub, warm-32 Git and warm-32 GitHub**; four improve and warm-32 path ties. Warm-8 Git CPU reverses **+1.200%/−0.403%**, and warm-32 path is **−0.717%/0.000%**. RSS is higher under both statistics for cold GitHub and all four warm Git/GitHub cases; both warm GitHub widths are higher in every pair. The audit retains all ten sign/tie disagreements among separate ratios, paired ratios and paired deltas across the five metrics, including user/system components.

Cold path improves against U in all five wall pairs (**−11.119%** paired median; range **−16.196% to −5.643%**), but CPU improves in four of five (**−5.009%**; range **−16.376% to +6.186%**). These direct results describe the combined change, not any individual edit. Recorded one-minute load ranges are 3.01–5.54 for the base comparison and 3.29–6.43 for U. Five cold pairs, ten warm pairs and the retained spreads limit broader claims; adverse observations are not dismissed as noise. Historical base-relative percentages are not subtracted across different host windows.

## Current source, validation and implementation

The [commit binding f67d1358](/home/connorbaker/Benchmarks/nix-performance-fixes/combined-simplification-20260929/commit-binding.json) joins all 3,783 committed leaves to the frozen source. [Validation 6d521f0b](/home/connorbaker/Benchmarks/nix-performance-fixes/combined-simplification-20260929/validation-combined-20260929/result.json) records **2,580 primary / 923 fallback unit passes**, **114/39 required cases**, **18 functionals** and **12 real-overlay tests**; unit skips are 2/1, disabled cases 3/1, and all seven commands pass. The [release binding 9130344a](/home/connorbaker/Benchmarks/nix-performance-fixes/combined-simplification-20260929/release/release-binding.json) identifies the actual optimized binaries. Older packaged-test records remain specific to their revisions.

The [same-definition source census c73b45e7](/home/connorbaker/Benchmarks/nix-performance-fixes/combined-simplification-20260929/analysis/source-census/census.json) uses tracked runtime C/C++ implementation/headers, parser/lexer, SQL, Rust and assembly suffixes, excluding tests, docs, build and tooling. It counts physical changed lines, including comments/blanks, not logical SLOC or emitted bytes. Against exact `8aad447f0bace6be413963e897ad690247ad6aa2`, **179 runtime files add 13,089 and delete 3,519 lines: net +9,570**. Files outside `.cc`/`.hh` contribute +110. Against U, the two changed runtime files add 48/delete 38, net +10. The complete frozen leaf/patch domain was checked before and after this census.

The [combined source patch](/home/connorbaker/Benchmarks/nix-performance-fixes/combined-simplification-20260929/release-snapshot/source.patch) orders initial regular/auxiliary SQL publication by its primary keys, borrows synchronous directory callbacks, and rejects a detected multi-node registration cycle before committing publication or client-cache changes. U already directly folds verified Merkle metadata during named materialisation. These source changes and the passing tests establish their tested behavior; the combined timings do not isolate which edit accounts for a cost difference. Historical selected-body assembly findings below apply only to their bound releases.

The [matched cold-path syscall diagnostic](/home/connorbaker/Benchmarks/nix-performance-fixes/combined-simplification-20260929/analysis/cold-cpu-followup/ARGUMENT-COMPARISON.md) uses one additional fresh-state `path:` evaluation per endpoint, with exact output checks and retired private states. Of the combined release's extra `fcntl` calls, **112,745/114,956 (98.1%)** address the local SQLite shared-memory file; **61,386/61,500 (99.8%)** extra `pwrite64` calls address its WAL or database. Source-directory flag calls are equal. The resulting database has 56,335 regular and 37,121 auxiliary rows. The sealed five-pair timings, not traced syscall durations, establish the cold-path **+1.30 s system / +0.31 s user** paired medians. The trace has descriptor arguments but no SQL stacks, so it locates the new I/O without assigning every call to a specific query. [The holder-index experiment](06-holder-index.md) already measured the per-file read-lock cost and rejected a transaction spanning filesystem probes because it changes concurrent candidate visibility and retry behavior. No semantics-preserving subtraction of these calls is established.

## U and the separate publication-order pilot

The [earlier U audit](/home/connorbaker/Benchmarks/nix-performance-fixes/direct-materialisation-20260929/numeric-audit/READOUT.md), for **f2fe32368c0b3e136860c3f61143609d574bf8d5** and source **05b59ce4**, retains its own two full-nine comparisons against 8aad and T/be522720. U’s paired cold-path CPU was **+53.82% versus 8aad** and **+9.75% versus T**; both warm-Git CPU widths were higher in every base pair. U’s cold-Git CPU was −3.40% versus T with all five pairs lower. Those earlier results are not the combined release’s results.

The [isolated publication-order pilot 309ec5ef](/home/connorbaker/Benchmarks/nix-performance-fixes/publication-sort-counterfactual-20260929/READOUT.md) compares U with snapshot44505c7a in five cold32-path pairs. Paired wall is **−10.87%** (4/5 lower; range −18.23% to +0.23%), CPU **−2.51%** (3/5 lower; −8.98% to +1.37%), and RSS **+0.87%** (1/5 lower; −2.72% to +13.62%). All ten full outputs match U. Both endpoints become faster after the first pairs. This one-domain counterfactual was not pooled into either complete comparison and does not establish a general CPU reduction.

The [six U user FP profiles](/home/connorbaker/Benchmarks/nix-performance-fixes/direct-materialisation-20260929/fp-diagnostics/READOUT.md) observe raw worktree hashing and cold holder/SQLite publication routes. All 2,951 samples reconcile; four unknown frames/chains and 1,006 depth-bound chains remain. Inclusive routes overlap, samples are not calls, zero markers do not prove absence, and user profiles do not attribute kernel work or causal fractions of the timing gap. These are U profiles, not profiles of the combined release.

## Historical 469699 measurements

Measured **46969915647c2a542172b42f0fd4b2511ecb1947**, source manifest `437bcb41d62ee71bad52c1f3ddc9822edf40454e10e6e192560dfb4992fb08ee`, does **not** improve every case. Warm-8 Git wall time is higher than branch base 8aad; cold GitHub, warm-8 Git and warm-32 path are higher than E8. The following results and source counts apply only to this historical release.

### Complete repeated measurements

Both comparisons evaluate Nixpkgs `6aefcda9401be8acc2b74244fb3b37520ea1f0a8` on aarch64 Linux through `path:`, live-worktree unpinned `git+file:`, and pinned `github:`. Each reference has five cold pairs per scheme and ten warm pairs per width/scheme (8 and 32 attributes): 156 evaluations including six seeds excluded from timing aggregation, 150 measured observations and 75 pairs. The evaluator cache is disabled. Cold means fresh private application state, not evicted OS caches; cold GitHub includes acquisition/network work. Affinity does not reserve the shared host.

[Completion](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/comparison/COMPLETE.json) binds the [release/source/binaries](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/comparison/BINDING.json), [8aad report](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/comparison/vs-8aad.json), and [E8 report](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/comparison/vs-E8.json). All 312 outputs pass: eighteen complete graph comparisons against 8aad establish equivalence after address rewriting; all 156 direct E8 outputs exactly match the retained 407f reference. The [independent audit](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/analysis/numeric-audit-469699/READOUT.md) rechecks every output and all ninety case/metric cells. Verified cold scratch states were retired after complete checking; logs, outputs and graph maps remain bound. The independent audit reuses these maps without rerunning physical graph walks.

Changes below are percentages, **separate/paired**; negative is better. Separate divides candidate/reference medians; paired takes the median of matched-trial ratios. W/T/L counts exact candidate lower/equal/higher wall observations. CPU sums exact GNU-time user/system centiseconds per observation before aggregation; component medians need not add. RSS is peak resident memory, not allocation volume. No observation is excluded or pooled with another cohort.

#### Against 8aad

| Case | Wall separate/paired | Wall W/T/L | CPU separate/paired | RSS separate/paired |
|---|---:|---:|---:|---:|
| cold 32 path | -12.27%/-15.23% | 5/0/0 | +69.94%/+68.93% | +20.79%/+20.36% |
| cold 32 git | -9.27%/-9.29% | 5/0/0 | -0.36%/+0.72% | -0.57%/-0.56% |
| cold 32 github | -44.97%/-41.09% | 4/0/1 | -73.63%/-73.63% | +4.68%/+5.72% |
| warm 8 path | -87.65%/-87.78% | 10/0/0 | -76.98%/-76.86% | -12.94%/-12.44% |
| warm 8 git | +1.82%/+2.67% | 5/0/5 | +48.82%/+50.89% | -0.39%/-0.37% |
| warm 8 github | -13.73%/-13.96% | 10/0/0 | -14.29%/-14.93% | +2.00%/+2.00% |
| warm 32 path | -75.16%/-74.96% | 10/0/0 | -65.90%/-66.02% | +6.37%/+6.30% |
| warm 32 git | -0.86%/-0.48% | 7/0/3 | +27.89%/+27.21% | -0.65%/-0.65% |
| warm 32 github | -7.26%/-7.94% | 9/0/1 | -7.49%/-7.49% | -2.36%/-2.36% |

#### Against E8

| Case | Wall separate/paired | Wall W/T/L | CPU separate/paired | RSS separate/paired |
|---|---:|---:|---:|---:|
| cold 32 path | -1.41%/-0.36% | 3/0/2 | +10.22%/+10.39% | -1.26%/-0.85% |
| cold 32 git | -4.65%/-7.61% | 4/0/1 | -2.04%/-4.76% | -0.06%/-0.06% |
| cold 32 github | +2.81%/+0.19% | 2/0/3 | +1.10%/+0.56% | -0.38%/+2.07% |
| warm 8 path | -0.73%/-0.23% | 5/0/5 | +0.00%/-0.58% | -2.13%/-2.08% |
| warm 8 git | +3.55%/+1.13% | 3/0/7 | +1.20%/+0.39% | -0.09%/-0.48% |
| warm 8 github | -1.40%/-1.38% | 7/0/3 | -1.92%/+0.00% | +0.06%/+0.05% |
| warm 32 path | +0.97%/+1.73% | 3/0/7 | +0.71%/+0.01% | -0.46%/-0.49% |
| warm 32 git | -1.84%/-1.11% | 7/0/3 | +0.00%/-0.54% | -0.44%/-0.43% |
| warm 32 github | -0.38%/-0.50% | 5/0/5 | -0.60%/-1.18% | +0.05%/+0.05% |

Against 8aad, cold-path and both warm-Git CPU measurements are higher in every pair. Cold Git CPU reverses −0.3571% separately to +0.7220% paired. RSS rises in four cases; cold path, warm-8 GitHub and warm-32 path are higher in every pair. Lower wall time does not erase those costs.

Against E8, six wall cases improve and three regress under both statistics, with 39/75 wins. Paired CPU has four lower/one tied/four higher cases; cold-path CPU rises **+10.22%/+10.39%**. RSS is lower in seven cases separately and six paired: cold GitHub reverses −0.3761%/+2.0678%. Warm-32 GitHub RSS is higher in every pair. All eleven disagreements among separate ratios, paired ratios and paired deltas remain in the [full five-metric tables](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/analysis/numeric-audit-469699/TABLES.md) and [exact sample arithmetic](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/analysis/numeric-audit-469699/audit.json); for example, warm-32 path CPU has zero paired median delta but a +0.0074925% paired median ratio. These are retained results, without a noise explanation or selected preferred statistic.

### Source and generated code

The [469699 source census](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/analysis/source-census-successor/READOUT.md) covers all runtime C/C++, parser/lexer, SQL, Rust and assembly suffixes. It counts physical lines including comments/blanks, excluding tests, docs, build and tooling; all 3,828 sealed leaves and the patch were checked.

| Measured source vs reference | Added | Deleted | Net |
|---|---:|---:|---:|
| Branch base 8aad |13,351 |3,481 |+9,870 |
|407f |1,970 |3,227 |−1,257 |
| E8 |140 |287 |−147 |

In the measured 469699 source, the largest additions over base are optimise-store.cc (+1,252), local-store.cc (+1,180), and git-object-store.cc (+986). Deleting source does not establish a smaller implementation than base or a speedup.

The [actual E8→469699 assembly review](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/successor-assembly-plan/ACTUAL-RELEASE-READOUT.md) finds emitted removals: pending storage falls 2,088→232 bytes, shard scans and two publication sorts disappear, synchronous scope vectors disappear, and Hash transfers shrink 80→68 bytes. Costs remain: ordered-set nodes grow, tree comparisons and a common regular mutex replace hashing/shards, Git listing gains emitted work, and some copy/formatting bodies grow. Three avoidable costs are proved: all three Copy node actions construct an extra owned coordinate; staged NAR restoration gains a 24-byte heap callable; and compact TreeEntry/HashedNode placement cannot pass the existing Hash operators’ 16-byte SIMD-alignment gate, selecting scalar comparisons. Buffered Flat-Git validity misses can also hash before and during restoration.

The [assembly-derived correction patch](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/successor-assembly-plan/all-assembly-corrections.patch) records follow-up corrections after 469699; its source proposal alone established no new validation or speed result. Later measured releases are distinguished above. The [sealed assembly manifest](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/successor-assembly-plan/ASSEMBLY-REVIEW-MANIFEST.json) retains complete selected-body inventories, actual source/DSO bindings, adverse frames and explicit partial/unread boundaries. Selected emitted sizes and finite residue/length proofs establish operations and possible paths, not dynamic frequencies, whole-library equivalence or timing attribution.

Forty-six historical generated JSON reports are retained in the [archive index](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/generated-evidence-retired/INDEX.md), with original paths, Git blobs and hashes in its manifest. They remain recoverable from commit `46969915647c2a542172b42f0fd4b2511ecb1947`; the [dated-run README](runs/2026-09-21-performance/README.md) records the archive and verified retirement.

### Counters and observed routes

The separate [E8→469699 PMU audit](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/analysis/counter-independent-review/READOUT.md) covers one whole-command observation per side in all nine cases. User instructions rise in seven cases, including every warm case (+0.01158% to +0.10810%); only cold Git and cold GitHub fall. User cycles fall in five cases and rise in four. All eighteen exact outputs, 54 raw/aggregate/rendered event records, strong hardware groups, inheritance and positive enabled==running times pass checks. Counts include inherited wrappers/descendants and exclude kernel/hypervisor work; `task-clock:u` is not proved pure user CPU. No sample ring was requested, so sampled-record loss is inapplicable.

The cold-path counter run reports 9.68→5.70 GNU-time CPU seconds, unlike the repeated cohort’s +10.39% paired regression. This single diagnostic observation does not replace repeated timing evidence or establish a cause. Warm states continue earlier private states; equal outputs do not prove equal caches. One pair per case supplies neither significance nor function/libgit2 attribution. Runtime/source checks retain their before/after and later-dlopen limits; the independent audit used recorded runtime hashes during concurrent diagnostics rather than rereading binary pages.

The new [warm-Git diagnostics](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/warm-git-diagnostics/READOUT.md) compare 8aad with **469699**, at both attribute widths. All eight instrumented outputs match their references. Four user profiles directly observe raw worktree hashing through MemoVisitor, regular hashing and filesystem access, separately from Git status. Separate traced runs count 388,273→860,847 syscalls at 8 attributes and 422,503→887,396 at 32. The endpoints load libgit2 1.9.7 and 2.0.0-rc.1 respectively; OpenSSL, SQLite and GC match. Aggregate syscall differences and lower status sample counts do not isolate a library-version effect. Sampling counts are not calls, tracing changes timing, and three unknown frames plus 550 depth-bound chains limit attribution.

The [cold-path diagnostic](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/cold-path-e8-diagnostics/READOUT.md) compares E8 with **469699**. Pending-holder leaf samples rise 8→24; nineteen candidate samples map to the executable-mode load inside the ordered-set search loop. This locates observed implementation work, not cache-miss latency, invocation frequency or a causal fraction of the CPU regression. Global memcmp leaf samples remain 16/16. Both fresh outputs match, and all 1,683 samples reconcile; three unknown frames and 514 depth-bound chains remain. Its two endpoints share actual libgit2, OpenSSL, SQLite and GC libraries. Each profile is a single user-only observation and does not attribute kernel work.

Earlier [Q→E8 counters](/home/connorbaker/Benchmarks/nix-performance-fixes/representation-simplification-20260929/counter-independent-review/READOUT.md), [407f→Q sampled profiles](/tmp/nix-full-nine-fp-profile-407f-q/readout/READOUT.md), and [E8 timings](/home/connorbaker/Benchmarks/nix-performance-fixes/representation-simplification-20260929/analysis/numeric-audit-e8c/READOUT.md) remain separate revision-specific evidence, including their regressions and limitations. Zero observed cached-tree markers does not prove absence. Static assembly, counters, samples, tracing and repeated timings answer different questions; none supplies performance results for a different release.
