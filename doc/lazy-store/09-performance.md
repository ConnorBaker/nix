# Evaluation performance and preserved behavior

Measured **46969915647c2a542172b42f0fd4b2511ecb1947**, source manifest `437bcb41d62ee71bad52c1f3ddc9822edf40454e10e6e192560dfb4992fb08ee`, does **not** improve every case. Warm-8 Git wall time is higher than branch base 8aad; cold GitHub, warm-8 Git and warm-32 path are higher than E8. The following results apply to this release. Current fixes are applied but **UNBUILT and unmeasured**; neither these results nor the source counts below describe the current checkout.

## Complete repeated measurements

Both comparisons evaluate Nixpkgs `6aefcda9401be8acc2b74244fb3b37520ea1f0a8` on aarch64 Linux through `path:`, live-worktree unpinned `git+file:`, and pinned `github:`. Each reference has five cold pairs per scheme and ten warm pairs per width/scheme (8 and 32 attributes): 156 evaluations including six seeds excluded from timing aggregation, 150 measured observations and 75 pairs. The evaluator cache is disabled. Cold means fresh private application state, not evicted OS caches; cold GitHub includes acquisition/network work. Affinity does not reserve the shared host.

[Completion](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/comparison/COMPLETE.json) binds the [release/source/binaries](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/comparison/BINDING.json), [8aad report](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/comparison/vs-8aad.json), and [E8 report](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/comparison/vs-E8.json). All 312 outputs pass: eighteen complete graph comparisons against 8aad establish equivalence after address rewriting; all 156 direct E8 outputs exactly match the retained 407f reference. The [independent audit](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/analysis/numeric-audit-469699/READOUT.md) rechecks every output and all ninety case/metric cells. Verified cold scratch states were retired after complete checking; logs, outputs and graph maps remain bound. The independent audit reuses these maps without rerunning physical graph walks.

Changes below are percentages, **separate/paired**; negative is better. Separate divides candidate/reference medians; paired takes the median of matched-trial ratios. W/T/L counts exact candidate lower/equal/higher wall observations. CPU sums exact GNU-time user/system centiseconds per observation before aggregation; component medians need not add. RSS is peak resident memory, not allocation volume. No observation is excluded or pooled with another cohort.

### Against 8aad

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

### Against E8

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

## Source and generated code

The [469699 source census](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/analysis/source-census-successor/READOUT.md) covers all runtime C/C++, parser/lexer, SQL, Rust and assembly suffixes. It counts physical lines including comments/blanks, excluding tests, docs, build and tooling; all 3,828 sealed leaves and the patch were checked.

| Measured source vs reference | Added | Deleted | Net |
|---|---:|---:|---:|
| Branch base 8aad |13,351 |3,481 |+9,870 |
|407f |1,970 |3,227 |−1,257 |
| E8 |140 |287 |−147 |

In the measured 469699 source, the largest additions over base are optimise-store.cc (+1,252), local-store.cc (+1,180), and git-object-store.cc (+986). Deleting source does not establish a smaller implementation than base or a speedup.

The [actual E8→469699 assembly review](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/successor-assembly-plan/ACTUAL-RELEASE-READOUT.md) finds emitted removals: pending storage falls 2,088→232 bytes, shard scans and two publication sorts disappear, synchronous scope vectors disappear, and Hash transfers shrink 80→68 bytes. Costs remain: ordered-set nodes grow, tree comparisons and a common regular mutex replace hashing/shards, Git listing gains emitted work, and some copy/formatting bodies grow. Three avoidable costs are proved: all three Copy node actions construct an extra owned coordinate; staged NAR restoration gains a 24-byte heap callable; and compact TreeEntry/HashedNode placement cannot pass the existing Hash operators’ 16-byte SIMD-alignment gate, selecting scalar comparisons. Buffered Flat-Git validity misses can also hash before and during restoration.

The [assembly-derived correction patch](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/successor-assembly-plan/all-assembly-corrections.patch) is now applied among the **UNBUILT** changes after 469699. No new validation or speed result is established. The [sealed assembly manifest](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/successor-assembly-plan/ASSEMBLY-REVIEW-MANIFEST.json) retains complete selected-body inventories, actual source/DSO bindings, adverse frames and explicit partial/unread boundaries. Selected emitted sizes and finite residue/length proofs establish operations and possible paths, not dynamic frequencies, whole-library equivalence or timing attribution.

Forty-six historical generated JSON reports are retained in the [archive index](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/generated-evidence-retired/INDEX.md), with original paths, Git blobs and hashes in its manifest. They remain recoverable from commit `46969915647c2a542172b42f0fd4b2511ecb1947`; the [dated-run README](runs/2026-09-21-performance/README.md) records the archive and verified retirement.

## Counters and observed routes

The separate [E8→469699 PMU audit](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/analysis/counter-independent-review/READOUT.md) covers one whole-command observation per side in all nine cases. User instructions rise in seven cases, including every warm case (+0.01158% to +0.10810%); only cold Git and cold GitHub fall. User cycles fall in five cases and rise in four. All eighteen exact outputs, 54 raw/aggregate/rendered event records, strong hardware groups, inheritance and positive enabled==running times pass checks. Counts include inherited wrappers/descendants and exclude kernel/hypervisor work; `task-clock:u` is not proved pure user CPU. No sample ring was requested, so sampled-record loss is inapplicable.

The cold-path counter run reports 9.68→5.70 GNU-time CPU seconds, unlike the repeated cohort’s +10.39% paired regression. This single diagnostic observation does not replace repeated timing evidence or establish a cause. Warm states continue earlier private states; equal outputs do not prove equal caches. One pair per case supplies neither significance nor function/libgit2 attribution. Runtime/source checks retain their before/after and later-dlopen limits; the independent audit used recorded runtime hashes during concurrent diagnostics rather than rereading binary pages.

The new [warm-Git diagnostics](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/warm-git-diagnostics/READOUT.md) compare 8aad with **469699**, at both attribute widths. All eight instrumented outputs match their references. Four user profiles directly observe raw worktree hashing through MemoVisitor, regular hashing and filesystem access, separately from Git status. Separate traced runs count 388,273→860,847 syscalls at 8 attributes and 422,503→887,396 at 32. The endpoints load libgit2 1.9.7 and 2.0.0-rc.1 respectively; OpenSSL, SQLite and GC match. Aggregate syscall differences and lower status sample counts do not isolate a library-version effect. Sampling counts are not calls, tracing changes timing, and three unknown frames plus 550 depth-bound chains limit attribution.

The [cold-path diagnostic](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-reuse-20260929/cold-path-e8-diagnostics/READOUT.md) compares E8 with **469699**. Pending-holder leaf samples rise 8→24; nineteen candidate samples map to the executable-mode load inside the ordered-set search loop. This locates observed implementation work, not cache-miss latency, invocation frequency or a causal fraction of the CPU regression. Global memcmp leaf samples remain 16/16. Both fresh outputs match, and all 1,683 samples reconcile; three unknown frames and 514 depth-bound chains remain. Its two endpoints share actual libgit2, OpenSSL, SQLite and GC libraries. Each profile is a single user-only observation and does not attribute kernel work.

Earlier [Q→E8 counters](/home/connorbaker/Benchmarks/nix-performance-fixes/representation-simplification-20260929/counter-independent-review/READOUT.md), [407f→Q sampled profiles](/tmp/nix-full-nine-fp-profile-407f-q/readout/READOUT.md), and [E8 timings](/home/connorbaker/Benchmarks/nix-performance-fixes/representation-simplification-20260929/analysis/numeric-audit-e8c/READOUT.md) remain separate revision-specific evidence, including their regressions and limitations. Zero observed cached-tree markers does not prove absence. Static assembly, counters, samples, tracing and repeated timings answer different questions; none supplies performance results for the current unbuilt fixes.
