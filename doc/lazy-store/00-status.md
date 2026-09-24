# Lazy-store status

Revision `3fedcc95f` has lower separate wall medians and median paired wall-time ratios in **all nine benchmark cases** against branch base `8aad447`, with **71 of 75 pairs faster**. The [complete optimized comparison](runs/2026-09-21-performance/paired-3fedcc95f.json) passes all 156 output checks and eighteen complete store comparisons. Median CPU remains higher in three cases and peak RSS in six. Further source investigation continues; changes after this measured revision remain unmeasured.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash preserves that meaning, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file blobs live in store paths themselves. The database retains candidate holders per object, mode and ingestion; lookup considers only registered valid paths. Reuse checks physical metadata and records the recipient's own witness. Publication reconciles concurrent inode classes before exposing candidates, and a complete re-ingestion replaces that path's previous projection. Trees and symlink-target blobs live in the auxiliary object namespace. Holds protect typed auxiliary addresses through repair and collection; temporary roots belong to individual live store handles. The [auxiliary representation analysis](/home/connorbaker/Benchmarks/nix-performance-fixes/auxiliary-representation-e77d0c12e/README.md) separates required retrievable bodies and live claims from their current file-per-body layout; it proposes no replacement and leaves regular-file inode sharing intact.

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Source-to-store memos retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

Completed evaluator requests are a separate kind of fact: the result of an earlier operation, identified by resolved source, method and output name. Reuse does not identify current mutable bytes. Filtered, asserted, reference-bearing and repair requests bypass this memo; their existing validation and destination-reuse rules still apply. Mounting or explicit reset retires completed requests.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Completed validation and limits

The [repository-reuse validation](/home/connorbaker/Benchmarks/nix-performance-fixes/workdir-repository-reuse-validation/SUMMARY.json) for 3fed passes **four focused unit cases and five functional targets, with no skips**. The exact optimized release passes **207 packaged tests, zero failures and 36 skips**. The preceding [009 full gates](/home/connorbaker/Benchmarks/nix-performance-fixes/blob-hasher-value-validation/README.md) passed **2,501 primary unit tests**, **897 fallback utility tests**, **eight functional targets** and **twelve real overlay tests**, following actual mount probes. Its two primary skips and three disabled cases, plus one of each in fallback, remain separate. Those full-suite counts are earlier evidence, not a rerun for 3fed.

The [performance report](09-performance.md#verification) preserves finite-domain checks, faulty variants with exact restorations, source ownership evidence and their limits. Complete store comparisons check contents, modes, references and outputs under an injective substitution of store addresses; they do not establish equivalence of arbitrary embedded programs. The [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/paired-3fedcc95f-audit/README.md) independently checks recorded mappings and all 156 outputs, without repeating physical graph walks.

These results apply to aarch64 Linux; **Windows remains unverified**. Overlay's lack of cross-layer regular-holder sharing and disabled auxiliary verification/materialisation remain explicit in the store model. Original fixture/checker failures and corrected runs are retained in their archives, rather than counted as passes. Older [validation](05-validation.md) and [holder investigations](06-holder-index.md) are evidence for their recorded implementations.

## Performance and remaining costs

The pinned workload has five paired cold trials and ten paired warm trials per case. Cold means fresh private store/fetcher state with warm operating-system caches; GitHub cold timings include network access. Reductions in the **median paired wall-time ratio against branch base** are:

| Workload | `path:` | `git+file:` | `github:` |
| --- | ---: | ---: | ---: |
| Cold · 32 attributes | 12.20% | 19.05% | 36.32% |
| Warm · 8 attributes | 88.33% | 6.17% | 12.46% |
| Warm · 32 attributes | 75.25% | 3.35% | 7.46% |

Cold GitHub wins 4/5 pairs, warm Git8 9/10 and warm Git32 8/10; all other retained pairs are faster. Cold-path CPU rises 3.32→6.72 s and RSS 445.21→480.54 MiB; every cold-path pair uses more CPU and RSS. Both warm Git cases also use more CPU. Lower wall time does not establish lower resource use.

The [complete direct repeat](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-3fedcc95f-vs-0090e56c5-repeat-audit/README.md) against 009 verifies another 156 exact outputs. Only **35/75 wall pairs are faster**, with four lower and five higher median paired ratios. CPU medians rise in six cases, fall in two and tie in one; RSS medians rise in four and fall in five. Cold-path paired wall/CPU rise 141.92 ms/0.08 s while RSS falls 4.30 MiB (3/5 lower). Warm Git32 has paired wall +39.84 ms and RSS +1.13 MiB. These mixed results do not isolate a benefit from removing the repository open.

The [original direct run](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-3fedcc95f-vs-0090e56c5-interfered-audit/README.md) remains retained separately: 37/75 wall wins and five lower paired ratios. Our session accidentally ran a recursive `rg --files /tmp` search during its opening trials. The [interference and repeat record](/home/connorbaker/Benchmarks/nix-performance-fixes/direct-repetition-3fedcc95f/README.md) records the decision, made before reading the original direct metrics, to retain that entire attempt and repeat the full protocol once. Both runs pass all outputs; their timing results are neither pooled nor selected per case, and the repeat does not establish an otherwise idle host.

Measured revision `3fedcc95f` reuses the already-opened repository for workdir metadata and verification, removing a second open. The [repository-reuse validation](/home/connorbaker/Benchmarks/nix-performance-fixes/workdir-repository-reuse-validation/SUMMARY.json) records four focused unit cases and all five Git acquisition, refs, shallow, submodule and verification functional targets passing, with no skips. It retains the original repository binding: administrative redirection of `.git` between acquisition and metadata lookup can differ from rediscovery; ordinary HEAD updates within that binding remain visible. This is not an atomic snapshot guarantee.

The following source change reuses an acquired stored accessor for legacy hash computation, exposes known NAR listing types, and transfers owned fetcher results. The [final targeted gates](/home/connorbaker/Benchmarks/nix-performance-fixes/source-projections-final-validation-3fedcc95f/README.md) pass five GTests and six functional targets without skips. The [negative controls](/home/connorbaker/Benchmarks/nix-performance-fixes/source-projections-first-validation-3fedcc95f/README.md) reject five duplicate acquisitions, the modern null-accessor exception mismatch and omitted NAR types. The ownership review enumerates all fourteen Input-bearing pair-returning functions and sixteen return sites; borrowed values remain copied. These changes are not part of 3fed; optimized-release and performance validation remain pending.

The [performance report](09-performance.md#measurement-limits-and-earlier-evidence) retains earlier comparisons, installed-consumer checks, profiles and syscall counts with their original source bindings. Historical profiles do not measure 3fed or assign current system CPU/RSS costs.

## Evidence and reproduction

- [Performance report](09-performance.md), [current sample record](runs/2026-09-21-performance/paired-3fedcc95f.json) and [release provenance](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-3fedcc95f.json): exact source/binaries, inputs, raw observations and validation.
- [Evaluation driver](runs/nixpkgs-evaluation.py): three URI schemes, cold 32 and warm 8/32 attributes, balanced order and CPU affinity. Builds/profiles run separately; shared-host exclusivity is not claimed.
- [Complete store comparison](runs/compare-store-graphs.py) and [methodology counterchecks](runs/check-benchmark-methodology.py): exact graph/content/output checks and planted faults. Each cold store has its own comparison; copying an indexed store changes holder inodes and is not a representative warm fixture.
- [Validation commands](05-validation.md) and [overlay host driver](runs/overlay-suite-on-host.sh): actual mount tests are required; a skipped probe is not a pass.

When resuming, bind each result to its tested source and binary. Re-run affected checks and the full optimized comparison after runtime changes; retain regressions until subsequent measurements resolve them.
