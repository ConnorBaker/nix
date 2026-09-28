# Evaluation performance and preserved behavior

Measured **2e3e7e883adb7a320f6141e59d7bd0d7d66c6e22** (i) has lower wall medians in all nine cases against fixed base 8aad. Against previous measured runtime 407f, results are mixed: four separate and three paired wall medians are lower. These complete-build comparisons do not isolate individual edits or dependency effects.

## Model and changes

A source denotes its rendered tree; naming it is separate from proving the destination holds it. Evaluator Text objects publish eagerly through one Store operation. Mounted inputs still materialize before the relevant output, references remain rooted across collection-capable operations, and read-only evaluation computes addresses without writes.

The implementation removes evaluator buffer/flush wrappers and shares traversal, ordered directory folding and scoped restore writers. Legacy NAR hashing is supplied to the operation that needs it. Buffered Flat publication emits a regular-file node directly to the hasher/placement visitor; staged Flat and cross-device copies still acquire their written projection. Leases are demand-only. Physical holder checks, repair, security walks and compatibility validation remain required. Cached directories still rebuild/hash a summary after parsing verified metadata; this additional work has no isolated cost measurement.

[Runtime source](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/source-census-i/READOUT.md) is 40 net physical lines smaller than f and 1,036 smaller than caae/407f, yet remains +10,091 from 8aad. Counts include comments/blanks and exclude tests, docs, build and tooling. Less source does not imply uniformly less work.

## Protocol and all eighteen measurements

Both series evaluate Nixpkgs `6aefcda9401be8acc2b74244fb3b37520ea1f0a8` on aarch64 Linux: five cold pairs per source and ten per warm count/source (8 and 32), 156 evaluations including six excluded seeds and 75 retained pairs per baseline. Cold means fresh private application state, not evicted OS caches; cold GitHub includes acquisition/network work. Affinity does not reserve the host.

The evaluator cache is disabled (`eval-cache=false`). The unpinned `git+file:` URL reads live tracked files; GitHub pins the stated revision and the path fixture is unchanged. [Completion](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/comparison/COMPLETE.json) binds all 312 outputs and both full reports to the [released source](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/release/release-binding.json).

Rows show prior → i medians followed by **separate/paired (S/P) percentage changes**. Separate change divides separate medians; paired change takes the median of within-pair ratios. L/E/H counts lower/equal/higher paired wall observations. CPU adds exact user/system centiseconds before aggregation; component medians need not add. RSS is peak MiB, not allocation volume. Signs use unrounded values. [Full statistics](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/numeric-i/RESULT.json), [all 90 cells](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/numeric-i/ALL-90-CELLS.csv) and [the readout](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/numeric-i/READOUT.md) retain user/system metrics, MADs, ranges, deltas and ties.

### Fixed base 8aad → i

| Case | Wall s; S/P % | Wall L/E/H | CPU s; S/P % | RSS MiB; S/P % |
|---|---:|---:|---:|---:|
| cold 32 path | 3.943 → 3.576; -9.314/-13.587 | 5/0/0 | 3.670 → 5.950; +62.125/+64.365 | 445.062 → 504.230; +13.294/+13.261 |
| cold 32 git | 2.952 → 2.203; -25.371/-22.278 | 5/0/0 | 2.860 → 2.620; -8.392/-6.429 | 481.418 → 478.672; -0.570/-0.570 |
| cold 32 github | 12.258 → 8.975; -26.787/-28.914 | 5/0/0 | 23.020 → 5.440; -76.368/-76.151 | 1025.066 → 815.965; -20.399/-20.529 |
| warm 8 path | 3.709 → 0.436; -88.256/-87.828 | 10/0/0 | 3.430 → 0.850; -75.219/-75.439 | 224.266 → 205.484; -8.375/-8.375 |
| warm 8 git | 0.904 → 0.837; -7.380/-6.362 | 9/0/1 | 0.845 → 1.295; +53.254/+54.167 | 254.758 → 257.703; +1.156/+1.152 |
| warm 8 github | 0.322 → 0.275; -14.716/-14.351 | 10/0/0 | 0.310 → 0.265; -14.516/-15.877 | 197.346 → 201.572; +2.142/+2.139 |
| warm 32 path | 4.316 → 1.039; -75.930/-75.257 | 10/0/0 | 4.110 → 1.430; -65.207/-64.235 | 392.646 → 431.355; +9.858/+8.493 |
| warm 32 git | 1.521 → 1.474; -3.105/-1.571 | 7/0/3 | 1.445 → 1.900; +31.488/+31.487 | 479.344 → 480.379; +0.216/+0.218 |
| warm 32 github | 0.934 → 0.864; -7.508/-9.449 | 8/0/2 | 0.910 → 0.850; -6.593/-7.180 | 460.625 → 447.150; -2.925/-2.929 |

All nine separate and paired wall medians are lower, with 69/75 wins. CPU medians rise in three cases and RSS in five. Cold path CPU/RSS and both warm Git CPU measurements rise in every pair. Cold Git also has a higher system-time median despite lower total CPU. Lower wall time does not erase these costs.

### Previous measured runtime 407f → i

| Case | Wall s; S/P % | Wall L/E/H | CPU s; S/P % | RSS MiB; S/P % |
|---|---:|---:|---:|---:|
| cold 32 path | 3.417 → 3.334; -2.445/-6.726 | 3/0/2 | 5.980 → 5.880; -1.672/+0.170 | 516.363 → 490.180; -5.071/-5.095 |
| cold 32 git | 2.408 → 2.398; -0.424/-0.424 | 3/0/2 | 2.770 → 2.760; -0.361/+0.000 | 480.832 → 480.547; -0.059/-0.103 |
| cold 32 github | 3.938 → 3.962; +0.586/+0.586 | 2/0/3 | 5.430 → 5.330; -1.842/-1.311 | 946.016 → 964.957; +2.002/+0.519 |
| warm 8 path | 0.434 → 0.429; -1.122/-0.210 | 5/0/5 | 0.855 → 0.850; -0.585/+0.000 | 207.289 → 207.402; +0.055/-1.010 |
| warm 8 git | 0.837 → 0.851; +1.646/+0.831 | 4/0/6 | 1.285 → 1.300; +1.167/+2.449 | 260.818 → 259.701; -0.428/-0.434 |
| warm 8 github | 0.272 → 0.273; +0.296/+0.556 | 4/0/6 | 0.260 → 0.260; +0.000/+0.000 | 201.695 → 201.529; -0.082/-0.084 |
| warm 32 path | 1.014 → 1.014; +0.026/+0.136 | 5/0/5 | 1.400 → 1.425; +1.786/+1.087 | 426.145 → 426.285; +0.033/-0.431 |
| warm 32 git | 1.433 → 1.446; +0.928/+1.328 | 4/0/6 | 1.855 → 1.850; -0.270/-0.546 | 480.551 → 480.469; -0.017/-0.011 |
| warm 32 github | 0.850 → 0.824; -3.152/+0.362 | 4/0/6 | 0.840 → 0.815; -2.976/+0.575 | 445.670 → 445.521; -0.033/-0.040 |

Separate wall medians are 4 lower/5 higher, versus 3 lower/6 higher paired, with 34/75 wins. Warm 32 GitHub reverses sign for both wall (−3.152%/+0.362%) and CPU (−2.976%/+0.575%). Cold-path CPU is also lower separately but higher paired (−1.672%/+0.170%). Warm 32 path's displayed wall medians round to the same value; its unrounded change is positive.

CPU medians are 6 lower/1 equal/2 higher separately, versus 2/3/4 paired. RSS is 6 lower/3 higher separately, versus 8/0/1 paired: both warm path cases reverse sign; cold GitHub remains higher. The complete report retains all 11 classification disagreements and all 12 cells higher in every pair across the two cohorts.

## What compiled code and diagnostics establish

The [f→h directory review](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/assembly/cache-directory-h/COMPARISON.md) confirms removal, on the deferred-child path, of the 48-byte outer closure allocation, a second long-name allocation and a 104-byte result-getter copy. Collection, vector growth and first-result storage remain. The combined local hashDirectory frame grows 448→592 bytes; this is not a whole-stack comparison. i retains the same directory source and all 18 selected aliases' extents (16 bodies), without a fresh directory operand proof.

The [i cache review](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/assembly/cache-i/COMPARISON.md), with its [completed release association](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/assembly/cache-i/RELEASE-ASSOCIATION.json), confirms that the natural-result helper removes h's extra optional transport. Four primary extents total 6,804 bytes versus f's 6,828; maybeLstat grows four bytes. The other three local frames shrink from f's 336/336/400 to 320/320/368 bytes. Removed string moves did not allocate on success; hit/publication copies remain. These findings do not quantify workload frequency, peak stack or timing impact. g/h were not benchmarked.

[User counters against 8aad](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/pmu-8aad-i/review/READOUT.md) have seven lower/two higher instruction counts and six lower/three higher cycle counts. Cold path is +11.858% instructions/+48.937% cycles; warm 8 Git +0.910%/+19.540%; warm 32 Git −0.413%/+12.646%. [Against 407f](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/analysis/pmu-407f-i/review/CONTEXT.md), instructions fall only for cold Git (−1.734%) and cold GitHub (−0.861%); the other seven rise +0.008–+0.534%. Cycles rise in six cases, including cold path +11.713% despite instructions +0.008%. Each diagnostic has one inherited whole-command observation per side/case, with no significance estimate or per-edit causality. Hardware counts exclude kernel/hypervisor; requested task-clock:u is not proved pure user time. Counters are not pooled with repeated timings.

## Validation and retained limits

[Validation](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/validation-i/result.json) and [commit binding](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/commit-binding.json) record seven passing stages: 2,566 primary/923 fallback unit passes, 186/56 required cases, 18 functionals and 12 overlays plus a real mount probe; skips are 2/1 and disabled cases 3/1. [Packaged functionals](/home/connorbaker/Benchmarks/nix-performance-fixes/cumulative-simplification-20260928i/release/packaged-functionals.json) have 207 passes, 36 skips and no failures among 243 cases. Finite tests do not enumerate arbitrary programs or concurrent schedules.

Earlier c/e failures, d/f measurements and the [withdrawn flat-table regression](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-a88ecdb3a/COMPLETED-REVIEW.md) remain historical evidence. Earlier profiles identify routes, not per-call causal costs; truncated SHA chains cannot identify callers beyond `copyFd`'s roughly 64 KiB frame with a 16 KiB capture. Assembly, user counters, syscall diagnostics and repeated timings answer different questions. Host activity, external inputs and finite cohorts limit generalization; uniform CPU, memory and source-size improvements are not established.
