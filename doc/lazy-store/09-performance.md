# Evaluation performance and preserved behavior

Measured **8ff26a216812835c61c37f7baac418bb789f034d** (q), bound to frozen source `5d2b2a2b0c8bbbc5f`, has lower separate and paired wall medians in all nine cases against fixed base 8aad, with 72/75 paired wins. Against previous measured runtime 407f, four separate and two paired wall medians are lower, with 30/75 wins. These complete-build comparisons do not isolate individual edits or dependency effects.

## Model and changes

A source denotes its rendered tree; naming it is separate from proving the destination holds it. Evaluator Text objects publish eagerly through one Store operation. Mounted inputs still materialize before the relevant output, references remain rooted across collection-capable operations, and read-only evaluation computes addresses without writes.

The implementation removes evaluator buffer/flush wrappers and shares traversal, ordered directory folding and scoped restore writers. Legacy NAR hashing is supplied to the operation that needs it. Buffered Flat publication emits a regular-file node directly to the hasher/placement visitor; staged Flat and cross-device copies still acquire their written projection. Leases are demand-only. Physical holder checks, repair, security walks and compatibility validation remain required. Cached directories still rebuild/hash a summary after parsing verified metadata; this additional work has no isolated cost measurement.

Since M, fixed filtering consumes root-relative accepted names directly; source callbacks still receive source coordinates. Root listings erase rejected entries from their owned map, and verification uses one postorder node continuation. Content-address construction borrows Hash inputs and constructs the final alternative/optional payload in place; getHash shares one projection. Required ownership copies, validation and failure paths remain.

Q's [lifetime fix](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928q/analysis/source-review-q/INDEX.md) joins automatic GC through the factory owner's deleter before any destructor begins. Ordinary shared, unique and other unprotected owners wait inside autoGC even when passed false. Factory creation now allocates object and control block separately, adding an allocation and failure opportunity. No-GC exits retain their existing behavior; the base destructor still joins residual worker tail.

[The cumulative census](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928q/analysis/source-review-q/census/READOUT.md) records −49 runtime-source lines M→Q and −1,092 from 407f, but +10,035 from 8aad. P→Q itself adds ten lines for correctness. Counts include comments/blanks and exclude tests, docs, build and tooling. The broader source-size and performance goal remains open.

## Protocol and all eighteen measurements

Both series evaluate Nixpkgs `6aefcda9401be8acc2b74244fb3b37520ea1f0a8` on aarch64 Linux: five cold pairs per source and ten per warm count/source (8 and 32), 156 evaluations including six excluded seeds and 75 retained pairs per baseline. Cold means fresh private application state, not evicted OS caches; cold GitHub includes acquisition/network work. Affinity does not reserve the host.

The evaluator cache is disabled (`eval-cache=false`). The unpinned `git+file:` URL reads live tracked files; GitHub pins the stated revision and the path fixture is unchanged. [Completion](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928q/comparison/COMPLETE.json) binds all 312 outputs, all eighteen graph checks and both full reports to the [released source](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928q/release/release-binding.json).

Rows show prior → q medians followed by **separate/paired (S/P) percentage changes**. Separate change divides separate medians; paired change takes the median of within-pair ratios. L/E/H counts lower/equal/higher paired wall observations. CPU adds exact user/system centiseconds before aggregation; component medians need not add. RSS is peak MiB, not allocation volume. Signs use unrounded values. [Full statistics](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928q/analysis/numeric-q/RESULT.json), [all 90 cells](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928q/analysis/numeric-q/ALL-90-CELLS.csv) and [the readout](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928q/analysis/numeric-q/READOUT.md) retain user/system metrics, MADs, ranges, deltas and ties.

### Fixed base 8aad → q

| Case | Wall s; S/P % | Wall L/E/H | CPU s; S/P % | RSS MiB; S/P % |
|---|---:|---:|---:|---:|
| cold 32 path | 4.702 → 3.624; -22.928/-22.790 | 5/0/0 | 3.590 → 6.040; +68.245/+56.659 | 403.621 → 485.859; +20.375/+19.046 |
| cold 32 git | 3.283 → 2.352; -28.375/-24.133 | 4/0/1 | 2.810 → 2.710; -3.559/+5.039 | 480.188 → 481.828; +0.342/+0.791 |
| cold 32 github | 6.378 → 3.934; -38.316/-41.945 | 4/0/1 | 19.860 → 7.120; -64.149/-67.429 | 818.789 → 919.562; +12.308/+10.084 |
| warm 8 path | 3.326 → 0.374; -88.755/-88.793 | 10/0/0 | 3.150 → 0.825; -73.810/-73.384 | 222.510 → 203.990; -8.323/-8.325 |
| warm 8 git | 0.820 → 0.767; -6.532/-7.029 | 10/0/0 | 0.775 → 1.250; +61.290/+62.579 | 252.908 → 259.844; +2.742/+2.745 |
| warm 8 github | 0.302 → 0.265; -12.124/-12.570 | 10/0/0 | 0.290 → 0.250; -13.793/-14.039 | 195.719 → 200.693; +2.542/+2.540 |
| warm 32 path | 3.861 → 0.933; -75.846/-75.571 | 10/0/0 | 3.690 → 1.360; -63.144/-63.278 | 443.305 → 427.559; -3.552/-3.103 |
| warm 32 git | 1.398 → 1.329; -4.973/-5.513 | 9/0/1 | 1.340 → 1.770; +32.090/+34.074 | 477.486 → 479.500; +0.422/+0.425 |
| warm 32 github | 0.876 → 0.785; -10.407/-9.459 | 10/0/0 | 0.865 → 0.770; -10.983/-7.682 | 454.779 → 445.404; -2.061/-2.062 |

All nine separate and paired wall medians are lower, with 72/75 wins. CPU medians rise in three cases separately and four paired; cold Git reverses from −3.559% to +5.039%. RSS rises in six cases both ways. Cold-path CPU, user/system time and RSS, both warm Git CPU/user/system measurements, warm 8 Git RSS, and cold/warm 8 GitHub RSS are higher in every pair. Cold Git also has higher system time. Lower wall time does not erase these costs.

### Previous measured runtime 407f → q

| Case | Wall s; S/P % | Wall L/E/H | CPU s; S/P % | RSS MiB; S/P % |
|---|---:|---:|---:|---:|
| cold 32 path | 3.238 → 3.037; -6.185/+8.152 | 2/0/3 | 5.800 → 5.870; +1.207/+3.793 | 499.887 → 499.828; -0.012/-0.012 |
| cold 32 git | 2.213 → 2.178; -1.561/+0.362 | 2/0/3 | 2.590 → 2.550; -1.544/+0.000 | 481.836 → 481.883; +0.010/-0.011 |
| cold 32 github | 3.599 → 3.558; -1.153/-1.153 | 3/0/2 | 5.520 → 5.610; +1.630/+1.630 | 951.473 → 934.129; -1.823/-1.823 |
| warm 8 path | 0.386 → 0.389; +0.623/+2.949 | 3/0/7 | 0.830 → 0.830; +0.000/+0.000 | 207.715 → 204.777; -1.414/-2.721 |
| warm 8 git | 0.752 → 0.780; +3.693/+1.641 | 1/0/9 | 1.245 → 1.265; +1.606/+1.587 | 258.588 → 257.266; -0.511/-0.224 |
| warm 8 github | 0.255 → 0.261; +2.133/+0.655 | 4/0/6 | 0.240 → 0.250; +4.167/+0.000 | 200.016 → 200.590; +0.287/+0.320 |
| warm 32 path | 0.934 → 0.936; +0.278/-2.168 | 6/0/4 | 1.345 → 1.380; +2.602/+0.370 | 428.400 → 427.473; -0.217/-0.393 |
| warm 32 git | 1.322 → 1.339; +1.330/+0.577 | 4/0/6 | 1.800 → 1.835; +1.944/+0.546 | 480.406 → 480.619; +0.044/+0.074 |
| warm 32 github | 0.799 → 0.785; -1.802/+0.765 | 5/0/5 | 0.780 → 0.775; -0.641/+1.983 | 444.895 → 445.840; +0.212/+0.206 |

Separate wall medians are 4 lower/5 higher, versus 2 lower/7 higher paired, with 30/75 wins. Four wall signs reverse: cold path −6.185%/+8.152%, cold Git −1.561%/+0.362%, warm 32 path +0.278%/−2.168%, and warm 32 GitHub −1.802%/+0.765%. Cold path wins only two of five pairs and CPU rises +1.207%/+3.793% despite marginally lower RSS.

CPU medians are 2 lower/1 equal/6 higher separately, versus 0/3/6 paired. Cold Git's lower CPU becomes tied, warm 8 GitHub's higher CPU becomes tied, and warm 32 GitHub CPU and user time reverse from lower to higher. RSS is 5 lower/4 higher separately and 6 lower/3 higher paired: cold Git reverses from higher to lower. System time changes from higher to tied for warm 8/32 path and from lower to tied for warm 32 Git/GitHub. User time remains higher in cold path/Git/GitHub, warm 8 Git, warm 32 path and warm 32 Git; system time is higher both ways for cold path and warm 8 Git. Warm 8/32 GitHub RSS is higher in every pair.

The full reports retain all 14 classification disagreements and 15 every-pair adverse cells across both cohorts. All observations remain included; no outlier exclusion or pooling changes these results.

[M's complete measurements](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/analysis/numeric-m/READOUT.md) and [earlier i measurements](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/numeric-i/READOUT.md) remain separate historical evidence. L's inode-exhausted comparison was aborted; N/O/P were intermediate source/assembly cohorts, and P's failed lifetime control precluded benchmarking it. No partial or historical result is pooled with Q.

## Compiled code

The [M→N→O→P assembly archive](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928p/analysis/assembly-ca-filter-m-n-o-p/INDEX.md) confirms deletion of root concatenation/rebasing and the filter factory's second set: the 24-body filter core shrinks 1,076 bytes, with Collector::node/frame 1,256/160→996/80 and factory 1,780/304→964/176. Root listing removes a second map's accepted-node 72-byte allocation/copy, while preserving visible membership and malformed-name rejection. Shared postorder verification removes 1,264 selected bytes and repeated physical-path construction when reporting a missing holder; the holder query remains required. The new node frame is 272 bytes versus the old symlink/directory 192. First-set allocation, path copies and required checks remain.

P's outlined in-place CA factory is 680 bytes/frame 96 versus O's 920/208; metadata conversion is 768/208 versus 1,084/352. Borrowing Hash removes reviewed 80-byte argument copies. Adverse details remain: metadata caller+factory uses 304 local frame bytes versus M's 288; withoutRefs 208 versus 192; CAFixed's own frame 416 versus 352. addToStoreSlow retains its 1,328→1,344 frame increase and prefetchFile its 6,020→6,048 code growth. endIngestion adds four net NOP bytes with frame 576 unchanged. getHash's 136-byte body has no compiled saving. These local segments are not whole-stack maxima.

[Actual Q assembly](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928q/analysis/assembly-ca-gc-q/READOUT.md) retains all fourteen CA production-owner extents and local frames from P. Both factory control blocks join before destructor dispatch; ordinary destructor entry still changes vptrs. The factories request separate object/control-block storage: local 992+24 bytes, overlay 1,040+24, with local frames 128/112. autoGC grows 184 code bytes to 1,872, retaining frame 416. Its no-GC exits bypass ownership lookup, but [idle factory-owned local destruction](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928q/analysis/assembly-ca-gc-q-idle-destruction.md) invokes the two-lock wait helper twice versus P's once; P overlay already waited twice. Creation/destruction are not cost-free. Complete declared core/ownership bodies were reviewed; equal extents do not establish identity of every caller or referenced data. These static findings establish neither workload frequency nor timing causality.

## Historical M assembly and all-nine-case profiling

The [M assembly review](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/analysis/assembly/hash-json-m/README.md) checks the actual release and its immediate L predecessor. Sharing Hash JSON conversion and borrowing the input object removes a deep JSON-map copy and redundant hash initialization/transport. The ten selected extents total 17,928→17,536 bytes; this is not the whole library. Import grows 16 code bytes and its local frame grows 2,112→2,176 bytes. Shared JSON conversion still allocates formatted strings and adds a parse wrapper on the former V1 direct path. M's fetchers text is byte-identical to L's: the earlier eager/lazy attribute ownership simplification removes copies but grows the selected function family by 1,420 bytes. No individual timing improvement is inferred from these changes.

The historical [M native-frame-pointer profiles](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/analysis/native-fp-m/INDEX.md) cover 8aad and M in every case, not Q: 18 captures and six seeds, with all 24 outputs checked. All 16,405 samples reconcile; loss, throttling, non-user samples and leaf-IP mismatches are zero. Seventeen frames remain unresolved and 3,029 rendered chains reach depth 127 or more. Every raw sample and chain word was checked: 13,000 rendered address arrays equal the native arrays; the other 3,405 differ solely by perf's inserted ARM64 LR−1. The original overly strict register-payload gate failed and remains retained beside its corrected LR-only gate. The actual perf ELF was pinned only after capture; that provenance limit remains explicit. Task-clock periods are diagnostic weights, not cycles, calls or guaranteed pure user CPU time.

These captures recover outer callers beyond the roughly 64 KiB `copyFdRange` frame that defeated the earlier 16 KiB DWARF capture. All 293 observations have an outer frame; five still reach the depth bound. `copyFdRange` is a generic file-to-sink read: a SHA leaf beneath it does not establish destination copying. The [route reviews](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/analysis/native-fp-routes-m/INDEX.md) retain complete selected domains, PCs, source associations, unknown frames and bounds:

| Cases | Source-supported route finding |
|---|---|
| Warm path, 8 and 32 | Base names through NAR ingestion, which can stage a large tree before checking destination validity. M names the current tree before copying and returns an existing valid destination. Base SHA samples sit under `TeeSource`; M's sit under `HashingVisitor`. |
| Warm Git, 8 and 32 | All 81 sampled M `copyFdRange` chains belong to `MemoVisitor` naming. Each width has 41 naming SHA samples, separate from status and derivation hashing. A live worktree's revision/status is insufficient identity for its rendered bytes, including clean filters and index flags. |
| Cold path | SQLite self samples rise 56→117 while OpenSSL self samples are 53→53. Holder lookups and regular/auxiliary publication are observed costs. The main connection already uses external synchronization; residual SQLite allocation/WAL locks are different operations. |
| Cold Git | Both status and M's raw-byte naming are observed. Status membership and clean/dirty metadata do not replace observing the rendered tree. |
| GitHub, cold and both warm widths | Cold acquisition corroborates removed pack delta search and full immutable-tree NAR naming. Compression/index construction remain. Every observed warm packed-blob read serves an import; no repeated whole-tree naming route is established. |

One diagnostic per side/case cannot assign causal fractions of benchmark time. Library versions differ, worker chains cannot recover their initiating main-thread call, and a missing sampled frame does not prove an operation absent. Native address conservation does not prove complete unwinding.

## Earlier compiled-code and counter evidence

The earlier [f→h directory](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/assembly/cache-directory-h/COMPARISON.md) and [i cache](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/assembly/cache-i/COMPARISON.md) reviews retain removed closure/name/result transport and adverse local frames; they do not supply a new Q operand proof. The directory fold's frame grew 448→592 bytes, and i's maybeLstat grew four code bytes. Collection, first-result storage and cache hit/publication copies remain.

Historical [i counters against 8aad](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/pmu-8aad-i/review/READOUT.md) and [407f](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/pmu-407f-i/review/CONTEXT.md) retain their regressions and disagreements. They are single observations per case, not Q measurements or significance estimates. Hardware events exclude kernel/hypervisor; task-clock purity is not proved. Neither counters nor profiles are pooled with repeated timings.

## Validation and retained limits

[Validation](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928q/analysis/validation-independent-review/INDEX.md) and [commit binding](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928q/commit-binding.json) record seven passing stages: 2,574 primary/923 fallback unit passes, 282/56 required cases, 18 functionals and 12 overlays; skips are 2/1 and disabled cases 3/1. [Packaged functionals](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928q/release/packaged-functionals.json) have 207 passes, 36 skips and no failures among 243 cases.

The [P control and Q test design](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928q/analysis/source-review-q/INDEX.md) distinguish destructor-entry safety from the earlier member-lifetime test. P compiled but failed exactly the new beforeDestruction assertion; earlier assertions passed. The completed Q validation passes the strengthened test and twelve owner × store-type × sync cells. The control stops before deletion/remount; it is an ordering discriminator, not a UAF crash or sanitizer proof. Finite tests do not enumerate arbitrary programs or schedules.

Earlier failed builds, the [withdrawn flat-table regression](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-a88ecdb3a/COMPLETED-REVIEW.md), and prior profiles remain revision-specific. Assembly, counters, syscall diagnostics and repeated timings answer different questions. Host activity, external inputs and finite cohorts limit generalization; uniform CPU, memory and source-size improvements are not established.
