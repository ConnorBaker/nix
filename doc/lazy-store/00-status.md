# Lazy-store status

Measured revision `ad86c3f39` is faster in eight of nine cases against fixed base `8aad447`, with **69/75 wall pairs faster**. **The all-cases-faster goal is not met:** cold-path separate and paired wall statistics are higher. All 156 outputs and eighteen store comparisons pass, but CPU medians are higher in three cases and RSS in six.

## Semantic model

A source denotes a filesystem tree: regular-file bytes and executable bit, a symlink target, or a finite directory map. Its SHA-256 object hash is computed from that tree, including the synthetic wrappers for executable and symlink roots. Source store paths also depend on their name, ingestion method and references. The evaluator buffers pending text objects within a byte bound and flushes them before their paths escape; a successful flush requires every pending object to be valid.

Regular-file blobs live in store paths themselves. The database retains candidate holders per object, mode and ingestion; lookup considers only registered valid paths. Reuse checks physical metadata and records the recipient's own witness. Publication reconciles concurrent inode classes before exposing candidates, and a complete re-ingestion replaces that path's previous projection. Trees and symlink-target blobs live in the auxiliary object namespace. Holds protect typed auxiliary addresses through repair and collection; temporary roots belong to individual live store handles. The [auxiliary representation analysis](/home/connorbaker/Benchmarks/nix-performance-fixes/auxiliary-representation-e77d0c12e/README.md) separates required retrievable bodies and live claims from their current file-per-body layout; it proposes no replacement and leaves regular-file inode sharing intact.

Names describe the complete rendered source view. Mutable worktrees remain filesystem sources even when Git reports them clean; status flags and attribute normalization cannot prove raw-byte identity. Filters, unions and grafts must preserve or derive that identity. Source-to-store memos retain addresses, not destination existence: a copy registers a temporary root and checks the requested store path. Hashing and materialisation share a bounded ordered scheduler; concurrent execution requires separately established source, filter and callback permissions, and every accepted child finishes before its directory scope closes.

Completed evaluator requests are a separate kind of fact: the result of an earlier operation, identified by resolved source, method and output name. Reuse does not identify current mutable bytes. Filtered, asserted, reference-bearing and repair requests bypass this memo; their existing validation and destination-reuse rules still apply. Mounting or explicit reset retires completed requests.

The [specification](01-specification.md), [implementation derivation](04-derivation.md) and [store model](08-store-model.md) define these contracts. Changes are vetted against the [reasoning and design guidance](/home/connorbaker/ExtPackages/denotational-design/reasoning-and-design-guidance.md): enumerate finite domains, derive operations from their meanings, and retain external checks that can reject the implementation.

## Current validation

The one-file status change traverses an owned libgit2 list directly, retaining flags, entry/path order, membership, dirtiness and cleanup before submodule discovery. Other pack callbacks retain exception relays; raw-tree naming remains separate. [Current gates](/home/connorbaker/Benchmarks/nix-performance-fixes/status-list-validation-ad86c3f39/README.md) pass **614 unit cases, zero failures, one skip and one disabled case**, including seven required Git cases, plus **five Git functional targets without skips**. The optimized release passes **207 packaged tests, zero failures and 36 skips**. No tests were added; the initial formatting failure remains archived.

[Source and compiled limits](09-performance.md#current-source-validation) distinguish these gates from earlier full-checkout/overlay validation. Tests do not measure exact allocations or stack frames; Windows remains unverified.

## Performance

Five cold and ten warm pairs per case retain every observation. Cold means fresh private application state with warm OS caches; GitHub cold includes network access. Median paired wall change against fixed base (negative means lower):

| Workload | `path:` | `git+file:` | `github:` |
| --- | ---: | ---: | ---: |
| Cold · 32 attributes | +3.48% | -19.48% | -32.49% |
| Warm · 8 attributes | -87.31% | -5.94% | -12.90% |
| Warm · 32 attributes | -73.13% | -3.59% | -9.07% |

Cold-path CPU rises **3.16→6.70 s** and RSS **445.22→476.50 MiB**, both higher in every pair. Warm Git8/Git32 CPU is also higher in every pair. Resource reduction remains unresolved.

The [direct 7a6 comparison](/home/connorbaker/Benchmarks/nix-performance-fixes/resources-ad86c3f39-vs-7a6fb248d-audit/README.md) passes another 156 outputs but has only **36/75 wall wins**: four lower separate medians, three lower paired ratios and two lower paired differences. Paired CPU has seven higher cases. Cold-path paired wall/CPU rise **103.45 ms/0.15 s** despite slightly lower RSS. Warm Git8’s paired wall ratio is lower but its paired difference higher. No overall direct benefit or causal status-edit effect is established.

Historical [7a6 user](09-performance.md#historical-7a6base-user-cycle-profiles) and [kernel](09-performance.md#historical-7a6base-kernel-profiles) profiles cover all nine cases separately. Empty/unknown frames, incomplete callers, differing environments and overlapping routes prevent exact allocation, system-seconds or RSS attribution; neither collection measures ad86.

The [performance report](09-performance.md), [current samples](runs/2026-09-21-performance/paired-ad86c3f39.json), [main audit](/home/connorbaker/Benchmarks/nix-performance-fixes/paired-ad86c3f39-audit/README.md) and [provenance](/home/connorbaker/Benchmarks/nix-performance-fixes/release-provenance-ad86c3f39.json) retain bindings and limits. Historical adverse comparisons, corrections and interference/repetition records remain linked there; series are not pooled.
