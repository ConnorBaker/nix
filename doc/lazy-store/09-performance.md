# Evaluation performance and preserved behavior

Latest measured runtime **a12e3dfabba797bd81c3544568760f57af932ced** lowers all nine wall medians against fixed branch base **8aad447f0bace6be413963e897ad690247ad6aa2**, with **69/75 faster pairs**. **Four CPU medians and six peak-RSS medians rise.** Directly against **daa1ff0798b3ec211f4d40c3f2bdc10f8f52b0bc**, four separate wall medians and four paired ratios are lower, with **39/75 faster pairs**; seven CPU and six RSS medians rise. The combined changes establish no universal performance or resource improvement.

## Protocol and measurements

The fixed nine-case domain evaluates Nixpkgs `6aefcda9401be8acc2b74244fb3b37520ea1f0a8` on aarch64 Linux. Each series has five cold pairs per source and ten pairs per warm count/source: **156 evaluations, six excluded warm seeds and 75 retained pairs**. Every scheduled observation is retained. The [driver](runs/nixpkgs-evaluation.py) records commands, identities and samples. Cold means fresh private application state, not evicted OS caches; GitHub acquisition includes network work. Target affinity does not reserve the host.

Rows show baseline→a12 separate medians. Wall changes distinguish the ratio of separate medians from the median of paired ratios, each minus one; negative is lower. CPU adds recorded user and system centiseconds for each observation before taking the median; component medians need not add. RSS is peak resident memory, not allocated bytes. Rounded displays do not decide signs.

### Fixed branch base → a12

| Case | Wall s | Wall change separate / paired | Faster pairs | CPU s | RSS MiB |
| --- | --- | --- | --- | --- | --- |
| cold 32 path | 3.890228→3.396120 | -12.701% / -12.260% | 5/5 | 3.290→5.090 | 444.70→498.26 |
| cold 32 git | 3.035077→2.446773 | -19.383% / -19.169% | 5/5 | 2.590→2.620 | 479.13→482.38 |
| cold 32 github | 6.671025→3.689074 | -44.700% / -41.384% | 4/5 | 21.590→5.280 | 974.64→961.93 |
| warm 8 path | 3.160004→0.384250 | -87.840% / -87.735% | 10/10 | 2.920→0.815 | 223.83→202.23 |
| warm 8 git | 0.849799→0.775626 | -8.728% / -7.857% | 8/10 | 0.785→1.250 | 254.36→259.25 |
| warm 8 github | 0.294399→0.261492 | -11.178% / -12.129% | 10/10 | 0.290→0.245 | 197.11→200.97 |
| warm 32 path | 3.751498→0.927370 | -75.280% / -75.569% | 10/10 | 3.490→1.360 | 444.65→428.87 |
| warm 32 git | 1.434248→1.337699 | -6.732% / -7.017% | 8/10 | 1.340→1.790 | 478.93→479.94 |
| warm 32 github | 0.853804→0.801648 | -6.109% / -7.778% | 9/10 | 0.845→0.785 | 441.99→445.30 |

Cold-path wall falls **12.701% separately / 12.260% paired**, while CPU rises **3.290→5.090 s** and RSS **444.70→498.26 MiB**. Every cold-path pair has higher CPU and RSS; both warm Git cases also have higher CPU in every pair. Cold Git CPU reverses: its separate median rises **2.590→2.620 s**, while its paired ratio is lower. All nine lower wall medians describe this batch, not every execution.

### Previous runtime daa1 → a12

| Case | Wall s | Wall change separate / paired | Faster pairs | CPU s | RSS MiB |
| --- | --- | --- | --- | --- | --- |
| cold 32 path | 3.626626→3.444141 | -5.032% / -3.670% | 5/5 | 5.650→5.670 | 500.28→490.39 |
| cold 32 git | 2.333357→2.434631 | +4.340% / +0.533% | 2/5 | 2.540→2.570 | 479.70→481.92 |
| cold 32 github | 3.947010→4.139549 | +4.878% / +0.056% | 2/5 | 5.330→5.420 | 967.92→949.59 |
| warm 8 path | 0.395639→0.392383 | -0.823% / -0.607% | 6/10 | 0.850→0.845 | 204.12→204.24 |
| warm 8 git | 0.855274→0.837922 | -2.029% / -2.339% | 8/10 | 1.315→1.305 | 259.08→259.31 |
| warm 8 github | 0.259264→0.263280 | +1.549% / +0.798% | 3/10 | 0.245→0.250 | 200.96→200.84 |
| warm 32 path | 0.949361→0.944139 | -0.550% / -2.520% | 7/10 | 1.380→1.390 | 426.82→429.79 |
| warm 32 git | 1.395024→1.446803 | +3.712% / +2.257% | 3/10 | 1.850→1.855 | 478.94→481.04 |
| warm 32 github | 0.789530→0.797470 | +1.006% / +1.566% | 3/10 | 0.780→0.785 | 445.72→445.78 |

Direct cold-path wall falls **5.032% separately / 3.670% paired**, with **5/5 faster pairs**. Its CPU rises **5.650→5.670 s** separately but falls paired. Cold Git and GitHub wall medians rise; GitHub's **+4.878% separate / +0.056% paired** illustrates the difference between these reductions. The same four cases have lower separate and paired wall medians: cold path, warm path8, warm Git8 and warm path32.

Direct CPU cases are **2 lower/0 equal/7 higher** separately versus **6/1/2** paired; RSS is **3/0/6** separately versus **4/0/5** paired. All three cold CPU cases and warm Git32 reverse from higher separately to lower paired; warm path32 becomes equal paired. Warm Git8 RSS is higher separately but **0.000798% lower paired**, a sign hidden by ordinary rounding. Direct CPU pairs are **33/9/33**, RSS **34/0/41**. Warm GitHub32 user time falls separately but rises paired. No case changes sign between its paired ratio and paired delta in this batch.

The [complete numeric review](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-a12e3dfab/COMPLETED-REVIEW.md) and [exact result](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-a12e3dfab/result.json) retain all eighteen cases × five metrics, user/system CPU, ranges, dispersion, ties and reversals. Independent checking covers **312 raw observations, 150 pairs, ninety cells and 1,481 numeric fields**. Main and direct series remain separate. These finite observations establish neither significance nor causal attribution to one edit.

## Source meaning and actual routes

The [specification](01-specification.md), [derivation](04-derivation.md) and [store model](08-store-model.md) define identity, contents, effects and ownership. Source schemes exercise different paths:

| Source | Selection and data route |
| --- | --- |
| Path | Mutable filesystem accessor; naming/materialisation when required, then store accessor. |
| Local Git | `git+file://…?shallow=1`, without ref/rev, selects fixed tracked membership over mutable filesystem bytes, with no cached fingerprint. |
| GitHub | Pinned archive resolved through the canonical tarball cache and its object-backed Git accessor. |

The [fetcher selection](../../src/libfetchers/git.cc) and [accessors](../../src/libfetchers/git-utils.cc) prevent conflating immutable Git-object improvements with local-worktree behavior. Clean status does not prove byte identity: attributes and index flags affect comparisons. Repair skips the source-known regular shortcut; supplied tree namers can still run for roots/directories, while fetch-layer repair policy separately forces misses.

Base 8aad adds clean-worktree revision metadata and derives a revision-based fingerprint. The branch explicitly caches no fingerprint: revision metadata does not identify live or attribute-rendered bytes. Base revision-keyed reuse and the branch's repeated walks therefore follow different identity rules. This explains route selection without assigning elapsed time to sampled weights or claiming every repeated walk is unavoidable.

Six separate [base/daa1 debug evaluations](/home/connorbaker/Benchmarks/nix-performance-fixes/warm-git-route-base-daa1/nix-warm-git-route-diagnostic-base-daa1-interpretation/README.md) confirm persistent root-cache hits on both warm base runs and live-root hashing on both warm daa1 runs. Daa1 also completes 237/553 requested-subpath copy operations, with destinations and hashes matching its seed. These messages do not count physical writes: regular roots buffered within 1 MiB can return for an already-valid destination before creating temporary files. Directory and larger-file routes can still materialise temporary state. These diagnostics are not a12 timings or a fresh filesystem census.

The measured [seven-file cohort](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-simplification-validation-a12e3dfab/source.patch) replaces CopyPool's per-child failure vector with one earliest-index failure. Accepted children still join, launch failures retain priority and ordered consumption stops at the winning child failure. Discarded exception objects are destroyed outside the pool mutex and before their jobs become joined; their destruction can occur earlier and reenter the pool. Full effect and resource-failure traces are not asserted identical.

Traversal borrows an existing prefix at the accessor root and retains an owned join elsewhere. Tar ancestor insertion uses `try_emplace`, preserving existing directory/file/symlink conflict behavior while avoiding mapped-value construction on a hit; the owning key remains. Hash construction removes a redundant zeroing operation after member initialization. Auxiliary publication relies on the key validation already performed by `Hold::addAuxiliary` before staging; complete transaction publication remains.

The inherited SQL representation owns typed, chunked directory bodies and symlink targets per ingestion. Reads own and verify a complete body snapshot; reused bodies enter the recipient's projection. Leases and complete publication replace per-body files and auxiliary graph marking. Regular holders retain physical checks and valid-path eligibility; overlay mutation remains upper-only. Optional early-hit traces and resource failures are not asserted identical.

### Actual compiled tradeoffs

The [completed AArch64 comparison](/home/connorbaker/Benchmarks/nix-performance-fixes/assembly-semantic-simplification-a12e3dfab/COMPARISON-SUMMARY.json) has a **380-body union and 340 mutually unique exact-alias matches** across util, store and fetchers; ambiguous aliases remain unmatched. Selected instruction bytes rise **235,724→235,900**. The util ELF grows **eight bytes**; whole store/fetchers sizes are unchanged. This selected family plus one local direct support edge is not a whole-program closure, and unchanged sizes do not establish byte identity.

[CopyPool’s eleven unique bodies](/home/connorbaker/Benchmarks/nix-performance-fixes/assembly-semantic-simplification-a12e3dfab/nix-copypool-assembly-comparison-a12e3dfab.md) fall **7,520→7,476 bytes** overall. `ordered` falls **1,260→1,124**, frame **240→224**, removing its exception-array allocation, initialization and scan. `execute` grows **464→544** with frame **80 unchanged**, supporting exception retirement outside the mutex; its success path avoids the old indexed-slot work. Hashing’s directory body grows **4,468→4,576** with frame **592 unchanged** through changed SHA256 inlining; materialisation’s **4,724-byte / 592-byte-frame** body retains its routes.

Auxiliary insertion falls **1,200→928 bytes** with frame **176 unchanged**, removing the repeated key gate and its throw. The admission gate, five SQL binds per chunk, TRANSIENT copies, transaction retries and owning projections remain. The other sixteen primary publication bodies retain their sizes, frames and call maps.

Root-prefix borrowing grows two traversal bodies: **1,224→1,380 bytes** with frame **256 unchanged**, and **1,192→1,520** with frame **352→368**. It adds optional-state initialization and engagement checks; the nonroot path keeps its join and adds move/cleanup work. The latter body also newly inlines a symlink hashing path with an indirect override fallback. The other 49 compared traversal/enclosing extents retain their sizes, mnemonic/direct-call sequences and stack adjustments apart from relocations.

Tar `addNode` falls **6,560→6,488 bytes**, frame **2,464→2,448**, while `createDirectory` grows **284→380**, frame **192→208**, through cleanup inlining. Their overlapping frame segment remains **2,656 bytes**. The baseline already avoided a map-node allocation on a hit; the removed work is temporary empty-map initialization and cleanup. Long owning keys still allocate. These code, construction and frame facts establish no invocation frequency, total stack bound, cycle or RSS benefit.

## Validation and evidence boundaries

[Current validation](/home/connorbaker/Benchmarks/nix-performance-fixes/semantic-simplification-validation-a12e3dfab/result.json) passes **2,517 primary / 912 fallback tests**, zero failures, with skips **2/1** and disabled **3/1**. All **103/36 required cases**, **17 functionals** and **12 real overlays** pass. The new sixteen-cell exception-cleanup test checks reentry; the expanded 144-cell launch-failure test includes failing detached children. Existing ordered-fold, lifetime, SQL, naming and header oracles remain. The packaged release passes **207 tests**, zero failures and **36 skips**. Skipped/disabled/device-unavailable and Windows cases are not executed coverage; debug allocator observations do not measure release resources.

The [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-a12e3dfab/main-audit.json) checks 156 outputs and eighteen complete store-graph comparisons under injective address mappings. The runner walked graphs; subsequent artifact checking verifies association. The [direct audit](/home/connorbaker/Benchmarks/nix-performance-fixes/comparison-a12e3dfab/direct-audit.json) checks another 156 outputs against own-release and prior references, including drvPath; supplemental stores were not graph-walked. Independent completed-output checks rechecked the schedules, order, commands, time fields, reference mappings and all 312 outputs. This does not prove arbitrary embedded-program equivalence. The [strict release proof](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-a12e3dfab.json) binds actual measured component sources/outputs.

## Historical diagnostics and limits

The [daa1 user-FP interpretation](/home/connorbaker/Benchmarks/nix-performance-fixes/resource-profile-user-fp-daa1ff079/independent-review/interpretation/README.md) and [6043 kernel/syscall findings](/home/connorbaker/Benchmarks/nix-performance-fixes/syscalls-6043a65c5-vs-base/interpretation/README.md) are **historical, not a12 profiles**. Each full profile series has eighteen captures, six seeds and 24 output checks. One instrumented pair per case gives no variance estimate; these observations cannot establish the incremental effect of a12.

| Historical collection | Samples | Missing user entry | Unknown leaves / frames | Other limits |
| --- | ---: | ---: | ---: | --- |
| daa1 user FP | 26,518 | 4,607 | 2,530 / 13,253 | 2,641 at exactly depth 127; max 128; two zero-IP frames |
| 6043 kernel | 24,948 | n/a | 1 / 1 | 1,885 empty chains; 6.345344% of period |

Recorded loss/unparsed records and decoder/header stderr are zero, but callers remain incomplete. Exactly 127 is not proof of truncation. Unknown/empty samples stay in denominators; high-bit user unknowns are not assigned to kernel code. Inclusive routes overlap. Sample counts and period weights are not invocation counts or exact cycles; separate collections cannot be pooled. Endpoint identity and startup loader checks do not establish complete mappings or isolation.

Historical cold Git includes status and traversal; warm Git still traverses mutable bytes. Cold GitHub's delta construction/flush/compression differs from warm GitHub's evaluator/parser/inflate route. Base uses **libgit2 1.9.7**; a12 uses **2.0.0-rc1 (`ae45d0d`) plus patches**. Fixed-base results compare dependency bundles. Direct daa1→a12 holds those dependency versions fixed but still compares combined source edits and code layout, not isolated causal effects. Historical [allocator attribution](/home/connorbaker/Benchmarks/nix-performance-fixes/allocation-symbols-110adbaf7-complete-attribution/README.md) also cautions that a shared operator-new[] label does not identify array-buffer cost or RSS ownership.
