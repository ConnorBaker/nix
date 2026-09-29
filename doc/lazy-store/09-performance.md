# Evaluation performance and preserved behavior

Measured **85f94252769771b6ba4c357d94f338058b78274a** (m), bound to frozen source `cf47ab310dfc7fa7`, has lower separate and paired wall medians in all nine cases against fixed base 8aad. Against previous measured runtime 407f, results are mixed: five separate and three paired wall medians are lower, with 32/75 paired wins. These complete-build comparisons do not isolate individual edits or dependency effects.

## Model and changes

A source denotes its rendered tree; naming it is separate from proving the destination holds it. Evaluator Text objects publish eagerly through one Store operation. Mounted inputs still materialize before the relevant output, references remain rooted across collection-capable operations, and read-only evaluation computes addresses without writes.

The implementation removes evaluator buffer/flush wrappers and shares traversal, ordered directory folding and scoped restore writers. Legacy NAR hashing is supplied to the operation that needs it. Buffered Flat publication emits a regular-file node directly to the hasher/placement visitor; staged Flat and cross-device copies still acquire their written projection. Leases are demand-only. Physical holder checks, repair, security walks and compatibility validation remain required. Cached directories still rebuild/hash a summary after parsing verified metadata; this additional work has no isolated cost measurement.

[Runtime source](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/analysis/source-census-m/READOUT.md) is 47 net physical lines smaller than f and 1,043 smaller than 407f, yet remains +10,084 from 8aad. The final L→M change removes 23 runtime lines. Counts include comments/blanks and exclude tests, docs, build and tooling. Less source does not imply uniformly less work.

## Protocol and all eighteen measurements

Both series evaluate Nixpkgs `6aefcda9401be8acc2b74244fb3b37520ea1f0a8` on aarch64 Linux: five cold pairs per source and ten per warm count/source (8 and 32), 156 evaluations including six excluded seeds and 75 retained pairs per baseline. Cold means fresh private application state, not evicted OS caches; cold GitHub includes acquisition/network work. Affinity does not reserve the host.

The evaluator cache is disabled (`eval-cache=false`). The unpinned `git+file:` URL reads live tracked files; GitHub pins the stated revision and the path fixture is unchanged. [Completion](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/comparison/COMPLETE.json) binds all 312 outputs and both full reports to the [released source](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/release/release-binding.json).

Rows show prior → m medians followed by **separate/paired (S/P) percentage changes**. Separate change divides separate medians; paired change takes the median of within-pair ratios. L/E/H counts lower/equal/higher paired wall observations. CPU adds exact user/system centiseconds before aggregation; component medians need not add. RSS is peak MiB, not allocation volume. Signs use unrounded values. [Full statistics](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/analysis/numeric-m/RESULT.json), [all 90 cells](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/analysis/numeric-m/ALL-90-CELLS.csv) and [the readout](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/analysis/numeric-m/READOUT.md) retain user/system metrics, MADs, ranges, deltas and ties.

### Fixed base 8aad → m

| Case | Wall s; S/P % | Wall L/E/H | CPU s; S/P % | RSS MiB; S/P % |
|---|---:|---:|---:|---:|
| cold 32 path | 3.945 → 3.140; -20.417/-22.623 | 5/0/0 | 3.660 → 5.500; +50.273/+50.273 | 444.914 → 502.352; +12.910/+15.633 |
| cold 32 git | 2.893 → 2.777; -4.027/-5.806 | 3/0/2 | 2.780 → 2.900; +4.317/+4.000 | 481.469 → 480.703; -0.159/-0.158 |
| cold 32 github | 6.903 → 3.516; -49.067/-45.424 | 5/0/0 | 17.110 → 5.510; -67.797/-67.778 | 973.328 → 959.410; -1.430/-1.151 |
| warm 8 path | 3.375 → 0.391; -88.425/-88.295 | 10/0/0 | 3.210 → 0.840; -73.832/-73.832 | 224.264 → 204.473; -8.825/-8.825 |
| warm 8 git | 0.851 → 0.776; -8.901/-9.589 | 9/0/1 | 0.815 → 1.260; +54.601/+55.086 | 254.758 → 259.654; +1.922/+1.922 |
| warm 8 github | 0.305 → 0.264; -13.366/-12.560 | 10/0/0 | 0.290 → 0.250; -13.793/-13.793 | 197.570 → 202.150; +2.318/+2.272 |
| warm 32 path | 3.925 → 0.951; -75.770/-75.922 | 10/0/0 | 3.775 → 1.395; -63.046/-63.239 | 444.891 → 427.301; -3.954/-2.826 |
| warm 32 git | 1.399 → 1.354; -3.275/-3.271 | 10/0/0 | 1.345 → 1.825; +35.688/+34.211 | 479.338 → 479.490; +0.032/+0.031 |
| warm 32 github | 0.880 → 0.789; -10.439/-8.867 | 10/0/0 | 0.870 → 0.775; -10.920/-8.815 | 456.938 → 447.248; -2.121/-2.126 |

All nine separate and paired wall medians are lower, with 72/75 wins. CPU and RSS medians each rise in four cases. Cold-path CPU/RSS and both warm Git CPU measurements rise in every pair. Cold Git now has a higher total-CPU median as well as higher system time. Lower wall time does not erase these costs.

### Previous measured runtime 407f → m

| Case | Wall s; S/P % | Wall L/E/H | CPU s; S/P % | RSS MiB; S/P % |
|---|---:|---:|---:|---:|
| cold 32 path | 2.688 → 2.792; +3.841/+6.395 | 0/0/5 | 5.160 → 5.420; +5.039/+4.264 | 512.672 → 490.566; -4.312/-3.626 |
| cold 32 git | 2.187 → 2.178; -0.413/+0.050 | 2/0/3 | 2.600 → 2.590; -0.385/+0.000 | 482.539 → 478.648; -0.806/-0.406 |
| cold 32 github | 4.109 → 4.106; -0.051/-0.051 | 4/0/1 | 5.300 → 5.350; +0.943/+1.737 | 973.160 → 978.719; +0.571/+0.784 |
| warm 8 path | 0.395 → 0.390; -1.046/-1.282 | 6/0/4 | 0.850 → 0.845; -0.588/+0.000 | 205.494 → 205.535; +0.020/+0.646 |
| warm 8 git | 0.794 → 0.807; +1.690/+1.078 | 4/0/6 | 1.290 → 1.285; -0.388/+0.385 | 260.857 → 261.699; +0.323/+0.054 |
| warm 8 github | 0.263 → 0.267; +1.384/+0.777 | 3/0/7 | 0.255 → 0.250; -1.961/+0.000 | 201.605 → 201.701; +0.047/+0.046 |
| warm 32 path | 0.964 → 0.956; -0.836/+2.107 | 3/0/7 | 1.395 → 1.405; +0.717/+0.360 | 429.281 → 429.096; -0.043/-0.222 |
| warm 32 git | 1.360 → 1.405; +3.309/+2.439 | 4/0/6 | 1.835 → 1.880; +2.452/+1.893 | 480.547 → 480.531; -0.003/-0.002 |
| warm 32 github | 0.818 → 0.811; -0.883/-0.341 | 6/0/4 | 0.805 → 0.800; -0.621/+0.000 | 445.809 → 446.793; +0.221/+0.214 |

Separate wall medians are 5 lower/4 higher, versus 3 lower/6 higher paired, with 32/75 wins. Cold path is slower in all five pairs: wall +3.841%/+6.395% and CPU +5.039%/+4.264%, despite lower RSS (−4.312%/−3.626%). Cold Git (−0.413%/+0.050%) and warm 32 path (−0.836%/+2.107%) reverse wall signs between separate and paired medians.

CPU medians are 5 lower/0 equal/4 higher separately, versus 0/4/5 paired. Warm 8 Git CPU reverses from lower to higher (−0.388%/+0.385%); cold Git, warm 8 path, warm 8 GitHub and warm 32 GitHub change from lower to tied. RSS is 4 lower/5 higher both ways. The complete report retains all 13 classification disagreements and all 16 cells higher in every pair across the two cohorts.

[Earlier i measurements](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/numeric-i/READOUT.md) remain separate historical evidence. The [L comparison](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928l/comparison) was aborted after inode exhaustion; no partial L result is pooled with M.

## Compiled code and all-nine-case profiling

The [M assembly review](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/analysis/assembly/hash-json-m/README.md) checks the actual release and its immediate L predecessor. Sharing Hash JSON conversion and borrowing the input object removes a deep JSON-map copy and redundant hash initialization/transport. The ten selected extents total 17,928→17,536 bytes; this is not the whole library. Import grows 16 code bytes and its local frame grows 2,112→2,176 bytes. Shared JSON conversion still allocates formatted strings and adds a parse wrapper on the former V1 direct path. M's fetchers text is byte-identical to L's: the earlier eager/lazy attribute ownership simplification removes copies but grows the selected function family by 1,420 bytes. No individual timing improvement is inferred from these changes.

The [fresh native-frame-pointer profiles](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/analysis/native-fp-m/INDEX.md) cover both releases in every case: 18 captures and six seeds, with all 24 outputs checked. All 16,405 samples reconcile; loss, throttling, non-user samples and leaf-IP mismatches are zero. Seventeen frames remain unresolved and 3,029 rendered chains reach depth 127 or more. Every raw sample and chain word was checked: 13,000 rendered address arrays equal the native arrays; the other 3,405 differ solely by perf's inserted ARM64 LR−1. The original overly strict register-payload gate failed and remains retained beside its corrected LR-only gate. The actual perf ELF was pinned only after capture; that provenance limit remains explicit. Task-clock periods are diagnostic weights, not cycles, calls or guaranteed pure user CPU time.

These captures recover outer callers beyond the roughly 64 KiB `copyFdRange` frame that defeated the earlier 16 KiB DWARF capture. All 293 observations have an outer frame; five still reach the depth bound. `copyFdRange` is a generic file-to-sink read: a SHA leaf beneath it does not establish destination copying. The [route reviews](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/analysis/native-fp-routes-m/INDEX.md) retain complete selected domains, PCs, source associations, unknown frames and bounds:

| Cases | Source-supported route finding |
|---|---|
| Warm path, 8 and 32 | Base names through NAR ingestion, which can stage a large tree before checking destination validity. M names the current tree before copying and returns an existing valid destination. Base SHA samples sit under `TeeSource`; M's sit under `HashingVisitor`. |
| Warm Git, 8 and 32 | All 81 sampled M `copyFdRange` chains belong to `MemoVisitor` naming. Each width has 41 naming SHA samples, separate from status and derivation hashing. A live worktree's revision/status is insufficient identity for its rendered bytes, including clean filters and index flags. |
| Cold path | SQLite self samples rise 56→117 while OpenSSL self samples are 53→53. Holder lookups and regular/auxiliary publication are observed costs. The main connection already uses external synchronization; residual SQLite allocation/WAL locks are different operations. |
| Cold Git | Both status and M's raw-byte naming are observed. Status membership and clean/dirty metadata do not replace observing the rendered tree. |
| GitHub, cold and both warm widths | Cold acquisition corroborates removed pack delta search and full immutable-tree NAR naming. Compression/index construction remain. Every observed warm packed-blob read serves an import; no repeated whole-tree naming route is established. |

One diagnostic per side/case cannot assign causal fractions of benchmark time. Library versions differ, worker chains cannot recover their initiating main-thread call, and a missing sampled frame does not prove an operation absent. Native address conservation does not prove complete unwinding.

## Historical compiled-code and counter evidence

The [f→h directory review](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/assembly/cache-directory-h/COMPARISON.md) confirms removal, on the deferred-child path, of the 48-byte outer closure allocation, a second long-name allocation and a 104-byte result-getter copy. Collection, vector growth and first-result storage remain. The combined local hashDirectory frame grows 448→592 bytes; this is not a whole-stack comparison. i retains the same directory source and all 18 selected aliases' extents (16 bodies), without a fresh directory operand proof.

The [i cache review](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/assembly/cache-i/COMPARISON.md), with its [completed release association](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/assembly/cache-i/RELEASE-ASSOCIATION.json), confirms that the natural-result helper removes h's extra optional transport. Four primary extents total 6,804 bytes versus f's 6,828; maybeLstat grows four bytes. The other three local frames shrink from f's 336/336/400 to 320/320/368 bytes. Removed string moves did not allocate on success; hit/publication copies remain. These findings do not quantify workload frequency, peak stack or timing impact. g/h were not benchmarked.

[User counters against 8aad](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/pmu-8aad-i/review/READOUT.md) have seven lower/two higher instruction counts and six lower/three higher cycle counts. Cold path is +11.858% instructions/+48.937% cycles; warm 8 Git +0.910%/+19.540%; warm 32 Git −0.413%/+12.646%. [Against 407f](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/pmu-407f-i/review/CONTEXT.md), instructions fall only for cold Git (−1.734%) and cold GitHub (−0.861%); the other seven rise +0.008–+0.534%. Cycles rise in six cases, including cold path +11.713% despite instructions +0.008%. Each diagnostic has one inherited whole-command observation per side/case, with no significance estimate or per-edit causality. Hardware counts exclude kernel/hypervisor; requested task-clock:u is not proved pure user time. Counters are not pooled with repeated timings.

## Validation and retained limits

[Validation](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/validation-m/result.json) and [commit binding](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/commit-binding.json) record seven passing stages: 2,570 primary/923 fallback unit passes, 272/56 required cases, 18 functionals and 12 overlays; skips are 2/1 and disabled cases 3/1. [Packaged functionals](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928m/release/packaged-functionals.json) have 207 passes, 36 skips and no failures among 243 cases. Finite tests do not enumerate arbitrary programs or concurrent schedules.

Earlier c/e failures, d/f measurements and the [withdrawn flat-table regression](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-a88ecdb3a/COMPLETED-REVIEW.md) remain historical evidence. Earlier profiles identify routes, not per-call causal costs; truncated SHA chains cannot identify callers beyond `copyFd`'s roughly 64 KiB frame with a 16 KiB capture. Assembly, user counters, syscall diagnostics and repeated timings answer different questions. Host activity, external inputs and finite cohorts limit generalization; uniform CPU, memory and source-size improvements are not established.
