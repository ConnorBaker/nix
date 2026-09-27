# Evaluation performance and preserved behavior

Latest measured runtime **daa1ff0798b3ec211f4d40c3f2bdc10f8f52b0bc** lowers all nine wall medians against fixed branch base **8aad447f0bace6be413963e897ad690247ad6aa2**, with **69/75 faster pairs**. However, **three CPU medians and seven peak-RSS medians rise**. Directly against **0871ab183779f1aa465740a3896eb06bebd18b5d**, three separate wall medians and three paired ratios are lower, with **29/75 faster pairs**. Cold-path wall is higher in all five direct pairs. The representation changes establish no universal performance or resource improvement.

## Protocol and measurements

The fixed nine-case domain evaluates Nixpkgs `6aefcda9401be8acc2b74244fb3b37520ea1f0a8` on aarch64 Linux. Each series has five cold pairs per source and ten pairs per warm count/source: **156 evaluations, six excluded warm seeds and 75 retained pairs**. Every scheduled observation is retained. The [driver](runs/nixpkgs-evaluation.py) records commands, identities and samples. Cold means fresh private application state, not evicted OS caches; GitHub acquisition includes network work. Target affinity does not reserve the host.

Rows show baseline→daa1 separate medians. Wall changes distinguish the ratio of separate medians from the median of paired ratios, each minus one; negative is lower. CPU includes all process threads' user and system time, recorded in centiseconds. RSS is peak resident memory, not allocated bytes. Component medians need not add. Rounded displays do not decide signs.

### Fixed branch base → daa1

| Case | Wall s | Wall change separate / paired | Faster pairs | CPU s | RSS MiB |
| --- | --- | --- | --- | --- | --- |
| cold 32 path | 3.462071→3.243805 | -6.305% / -6.538% | 5/5 | 3.190→5.630 | 445.22→484.70 |
| cold 32 git | 2.798784→2.164320 | -22.669% / -20.022% | 5/5 | 2.690→2.530 | 479.61→480.77 |
| cold 32 github | 6.240675→3.566899 | -42.844% / -40.703% | 4/5 | 16.420→5.210 | 960.20→970.21 |
| warm 8 path | 3.024978→0.388462 | -87.158% / -87.082% | 10/10 | 2.825→0.820 | 224.27→204.53 |
| warm 8 git | 0.806064→0.761243 | -5.561% / -6.244% | 8/10 | 0.750→1.250 | 254.77→259.81 |
| warm 8 github | 0.295529→0.263918 | -10.696% / -10.745% | 10/10 | 0.280→0.250 | 197.32→201.11 |
| warm 32 path | 3.520860→0.917099 | -73.952% / -73.745% | 10/10 | 3.350→1.340 | 444.90→425.20 |
| warm 32 git | 1.347960→1.319236 | -2.131% / -3.690% | 7/10 | 1.290→1.775 | 479.35→480.49 |
| warm 32 github | 0.863329→0.795845 | -7.817% / -8.047% | 10/10 | 0.850→0.780 | 444.70→445.81 |

Cold-path wall falls **6.305% separately / 6.538% paired**, while CPU rises **3.190→5.630 s** and RSS **445.22→484.70 MiB**. Every cold-path pair has higher CPU and RSS; both warm Git cases also have higher CPU in every pair. All nine lower wall medians describe this batch, not every execution.

### Previous runtime 0871 → daa1

| Case | Wall s | Wall change separate / paired | Faster pairs | CPU s | RSS MiB |
| --- | --- | --- | --- | --- | --- |
| cold 32 path | 3.216690→3.251862 | +1.093% / +2.809% | 0/5 | 5.750→5.500 | 492.72→502.74 |
| cold 32 git | 2.147770→2.157231 | +0.440% / +0.457% | 2/5 | 2.520→2.540 | 480.76→478.61 |
| cold 32 github | 3.327488→3.986190 | +19.796% / +14.120% | 1/5 | 5.200→5.300 | 995.34→966.38 |
| warm 8 path | 0.441135→0.400997 | -9.099% / -0.520% | 5/10 | 0.840→0.840 | 201.31→204.40 |
| warm 8 git | 0.773122→0.783898 | +1.394% / -0.276% | 5/10 | 1.255→1.260 | 259.84→261.76 |
| warm 8 github | 0.262035→0.261136 | -0.343% / +1.216% | 3/10 | 0.250→0.250 | 201.43→202.13 |
| warm 32 path | 0.959139→0.977707 | +1.936% / +1.907% | 3/10 | 1.390→1.380 | 426.08→426.28 |
| warm 32 git | 1.349420→1.351241 | +0.135% / +1.062% | 3/10 | 1.805→1.810 | 480.42→480.32 |
| warm 32 github | 0.812864→0.787015 | -3.180% / -2.624% | 7/10 | 0.805→0.775 | 446.02→446.89 |

Direct cold-path wall rises **1.093% separately / 2.809% paired**, with **0/5 faster pairs**. CPU falls **5.750→5.500 s**, while RSS rises **492.72→502.74 MiB**. Cold GitHub wall rises **19.796% / 14.120%**. Warm Git8 reverses from higher separately to lower paired; warm GitHub8 reverses the other way. Equal totals of three lower wall cases conceal this change in membership.

Direct CPU cases are **3 lower/2 equal/4 higher** separately versus **5/2/2** paired; RSS is **3/0/6** separately versus **4/0/5** paired. Warm path32 RSS reverses from higher separately to lower paired. Direct CPU pairs are **34/12/29** and RSS **27/0/48**. The [complete numeric review](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-daa1ff079/README.md) and [exact result](/home/connorbaker/Benchmarks/nix-performance-fixes/numeric-review-daa1ff079/result.json) preserve all eighteen cases × five metrics, user/system CPU, ranges, dispersion, ties and reversals. Independent checking covers **312 raw observations, 150 pairs, ninety cells and 1,481 numeric fields**. Main and direct series remain separate. These finite observations establish neither significance nor causal attribution to one edit.

## Source meaning and actual routes

The [specification](01-specification.md), [derivation](04-derivation.md) and [store model](08-store-model.md) define identity, contents, effects and ownership. Source schemes exercise different paths:

| Source | Selection and data route |
| --- | --- |
| Path | Mutable filesystem accessor; naming/materialisation when required, then store accessor. |
| Local Git | `git+file://…?shallow=1`, without ref/rev, selects fixed tracked membership over mutable filesystem bytes, with no cached fingerprint. |
| GitHub | Pinned archive resolved through the canonical tarball cache and its object-backed Git accessor. |

The [fetcher selection](../../src/libfetchers/git.cc) and [accessors](../../src/libfetchers/git-utils.cc) prevent conflating immutable Git-object improvements with local-worktree behavior. Clean status does not prove byte identity: attributes and index flags affect comparisons. Repair skips the source-known regular shortcut; supplied tree namers can still run for roots/directories, while fetch-layer repair policy separately forces misses.

Base 8aad adds clean-worktree revision metadata and derives a revision-based fingerprint. The branch explicitly caches no fingerprint: revision metadata does not identify live or attribute-rendered bytes. Base revision-keyed reuse and the branch's repeated walks therefore follow different identity rules. This explains route selection without assigning elapsed time to sampled weights or claiming every repeated walk is unavoidable.

The measured **four-file delta** uses CopyPool groups only with more than one worker, more than one child and at least one directory hint; all other groups read/fold inline even when a pool already exists. Hashing and materialisation retain ordered results and first-failing-child selection, with each child's read before its own fold. The inline route may avoid later reads, hooks or memo warming after failure; resource failures and complete effect traces are not asserted identical. Advisory directory flags still do not define child contents.

The same change replaces decimal `to_chars` with one bounded backward conversion into the existing 26-byte header buffer. Blob/tree framing, full uint64 sizes, one Sink call and the separate raw-mode octal formatter remain. This source change alone establishes neither earlier heap-allocation frequency nor current cycle savings.

The [complete retained inventory](/home/connorbaker/Benchmarks/nix-performance-fixes/directory-child-domain/README.md) has **5,620 of 7,069** multi-child directories without a directory child, containing 17,329 child edges. That identifies a topology opportunity. Filtering, unknown hints, known-node shortcuts and whether a pool already exists prevent treating it as an invocation or allocation count.

The inherited SQL representation owns typed, chunked directory bodies and symlink targets per ingestion. Reads own and verify a complete body snapshot; reused bodies enter the recipient's projection. Leases and complete publication replace per-body files and auxiliary graph marking. Regular holders retain physical checks and valid-path eligibility; overlay mutation remains upper-only. Resource failures and optional early-hit traces are not asserted identical.

### Actual compiled tradeoffs

The [completed AArch64 comparison](/home/connorbaker/Benchmarks/nix-performance-fixes/assembly-empty-groups-header-daa1ff079-vs-0871/comparison/ROUTES.md) retains a 590-body union with 533 exact-alias matches; ambiguous aliases remain unmatched. Selected instruction bytes fall **378,816→378,060**, while the store ELF grows **80 bytes** and util/fetchers ELF sizes stay unchanged. The selected families plus one local support edge are not a whole-program closure.

`feedHeader` falls **724→304 bytes**, fixed frame **320→64**, removing the emitted 201-byte decimal-table copy. `hashObject` retains its 208-byte frame: this specific overlapping caller/header segment falls **528→272**. The constructor tail-calls the header, so its frame does not overlap. Hashing's directory body falls **4,524→4,468 bytes** and materialisation's child-copy body **4,800→4,724**; both fixed frames remain **592 bytes**.

Both consumers now test count/hints before loading an existing pool. A warmed all-false group with at least two children avoids its failure vector, condition variable and empty wait lock, while adding a hint scan the prior live-pool branch skipped. No parent queued jobs are removed from that case. Descendant work, failure cleanup and SQL routes remain. These local code/frame facts establish no whole-stack, heap-allocation, frequency, cycle or RSS benefit.

## Validation and evidence boundaries

[Current validation](/home/connorbaker/Benchmarks/nix-performance-fixes/empty-copy-groups-validation/README.md) passes **2,516 primary / 911 fallback tests**, zero failures, with skips **2/1** and disabled **3/1**. All **99/32 required cases**, **17 functionals** and **12 real overlays** pass. The new 72-case failure oracle checks ordered read/fold failure selection; existing 120 header-byte boundaries and advisory-flag, hashing, SQL and ownership oracles remain. The packaged release passes **207 tests**, zero failures and 36 skips. Earlier failed fixtures/diagnostic continuations remain separate. Skipped/disabled/device-unavailable and Windows cases are not executed coverage; debug allocator observations do not measure release resources.

The [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/paired-daa1ff079-audit/README.md) checks 156 outputs and eighteen complete store-graph comparisons under injective address mappings. The runner walked graphs; archive checking verifies association. The [direct audit](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-daa1ff079-vs-0871ab183-audit/README.md) checks another 156 outputs against own-release and prior references, including drvPath; supplemental stores were not graph-walked. This does not prove arbitrary embedded-program equivalence. The [strict release proof](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-daa1ff079.json) binds actual measured component sources/outputs.

## Diagnostic observations and limits

The [fresh daa1 user-FP interpretation](/home/connorbaker/Benchmarks/nix-performance-fixes/resource-profile-user-fp-daa1ff079/independent-review/interpretation/README.md) covers all nine base/current cases: eighteen captures, six seeds and 24 output checks. One instrumented pair per case gives no variance estimate. Earlier [6043 kernel/syscall findings](/home/connorbaker/Benchmarks/nix-performance-fixes/syscalls-6043a65c5-vs-base/interpretation/README.md) remain revision-specific; no new daa1 kernel/syscall collection is supplied.

| Collection | Samples | Missing user entry | Unknown leaves / frames | Other limits |
| --- | ---: | ---: | ---: | --- |
| daa1 user FP | 26,518 | 4,607 | 2,530 / 13,253 | 2,641 at exactly depth 127; max 128; two zero-IP frames |
| 6043 kernel | 24,948 | n/a | 1 / 1 | 1,885 empty chains; 6.345344% of period |

Recorded loss/unparsed records and decoder/header stderr are zero, but callers remain incomplete. Exactly 127 is not proof of truncation. Unknown/empty samples stay in denominators; high-bit user unknowns are not assigned to kernel code. Inclusive routes overlap. Sample counts and period weights are not invocation counts or exact cycles; separate collections cannot be pooled. Endpoint identity and startup loader checks do not establish complete mappings or isolation.

Daa1 cold-path inclusive period shares include `CopyPool::ordered` **19.129%**, `MaterialisingVisitor::directory` **27.195%**, and the main observed `hashDirectory` spelling **26.293%**. Its lambda clone is retained separately. `SQLiteStmt::Use::next` is **7.368% versus 2.726% base**; regular/auxiliary publication remains visible. Cold Git combines status and traversal. Warm Git8/32 `getWorkdirInfo` shares fall **43.107→22.159% / 20.643→12.199%**, while hash/copy routes remain; warm path also retains them. All nine denominators and full observed symbol unions are retained. These shares are not a time decomposition or an incremental effect estimate.

Cold GitHub's base delta construction versus daa1 flush/compression differs from warm GitHub's evaluator/parser/inflate route. Base uses **libgit2 1.9.7**; branch uses **2.0.0-rc1 (`ae45d0d`) plus patches**. Fixed-base results therefore compare dependency bundles. Direct 0871→daa1 controls those versions, but still compares combined source edits and code layout, not isolated causal effects.

Both immediate header implementations use stack storage; older short string headers could already use SSO. Current named header observations are sparse and establish no speedup. Historical [allocator attribution](/home/connorbaker/Benchmarks/nix-performance-fixes/allocation-symbols-110adbaf7-complete-attribution/README.md) only cautions that a shared operator-new[] label does not identify array-buffer cost or RSS ownership.
