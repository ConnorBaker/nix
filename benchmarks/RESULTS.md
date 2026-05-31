# Benchmark results

A captured run of the CLI harness (`./run.sh --time`). Reproduce with the
commands in [`README.md`](./README.md). Semantic counts (walks/copies/cache) are
deterministic; wall-clock is machine-specific and shown for the `big` fixture
scale, where copy volume dominates start-up.

## Environment

| | |
| --- | --- |
| Date | 2026-05-31 |
| Host | Intel Core i9-14900K, 32 threads, Linux 7.0.0 |
| `ours` | `nix 2.35.0` @ `43134994f` (this branch) |
| `baseline` | `nix 2.35.0pre20260521_2d309b1` — upstream master `2d309b18e` (our merge-base) |
| `detsys` | `Determinate Nix 3.20.0` (2.34.6), `lazy-trees = true` |

## Semantic metrics (small fixtures — deterministic, the headline signal)

Format: `walks` (NAR-hash DryRuns) / `copies` (store writes) / `uncacheable`
bypasses / `cache hits`, for a **cold** store and a **warm** re-eval; plus the
fetcher-cache `rows` written.

### `filtered` — filtered source (`lib.cleanSource` shape), README §6.6/§7.12

| tree | cold | warm | verdict |
| --- | --- | --- | --- |
| baseline | walks 1 / copies 1 / **uncacheable 1** | walks 0 / **copies 1** / uncacheable 1 | re-copies every eval; cache bypassed |
| **ours** | walks 1 / copies 1 / **uncacheable 0** | walks 0 / **copies 0** / uncacheable 0 / hit 1 | **cacheable; warm re-eval does no copy** |
| detsys | walks 1 / copies 1 / **uncacheable 1** | walks 0 / **copies 1** / uncacheable 1 | same bypass as master |

**WIN (ours).** Master and DetSys both hit `fetch-to-store.cc`'s `filter ?
nullopt` bypass — the source is `uncacheable`, so a warm re-eval (one that
misses the eval-cache, e.g. an IDE/LSP or devshell loop) **re-copies the whole
filtered tree every time**. Ours folds the filter's `;shape` into the
fingerprint, so the row is written (rows 2→4) and the warm re-eval is a pure
cache hit: **0 walks, 0 copies**. This is the README's "largest single
performance hole".

### `cargo` — N `builtins.path` over subtrees of one rev (N=12), PROPOSAL §4

| tree | cold | warm |
| --- | --- | --- |
| baseline | walks 1 / **copies 12** | hits 13 |
| **ours** | walks 1 / **copies 1** | hits 1 |
| detsys | walks 1 / **copies 12** | hits 13 |

**WIN (ours).** All three walk the source once, but the eager trees then write
**12 separate store copies** (one per package name). Ours collapses them with
**copy-once-link-N**: one byte-copy + 11 hardlinks → **1 copy**. (Wall-clock
below shows this translating to real time at scale.)

### `interp` — interpolate a virtual source into a discarded string, PROPOSAL §8.2(2)

| tree | cold copies |
| --- | --- |
| baseline | **1** |
| **ours** | **0** |
| detsys | **1** |

**WIN (ours, including vs DetSys).** When a source path is built and
interpolated into a string but the path itself never reaches final output, ours
threads the `SourceVirtual` placeholder through the `nString` coercion and
**copies nothing**; both baseline *and* DetSys force a copy. This is the
"ZonePath footgun" the PROPOSAL claims to dissolve. (The win is only observable
when the path is discarded — the CLI devirtualises everything that reaches
output, README §2.4, so the workload returns the *string length*.)

### `pureflake` — metadata-only flake-input read, PROPOSAL §8.5 (parity row)

| tree | cold | warm |
| --- | --- | --- |
| baseline | walks 1 / copies 0 | hit 1 |
| ours | walks 1 / copies 0 | hit 1 |
| detsys | walks 1 / copies 0 | hit 1 |

**PARITY (expected).** Reading only `.rev` of a flake input copies nothing on
any lazy tree. Confirms ours matches where the PROPOSAL says it should, rather
than claiming a win that isn't there.

### `crossrev` — same subtree across two revs, README §6.1

| tree | cold | warm |
| --- | --- | --- |
| baseline | walks 2 / copies 2 | hits 4 |
| ours | **walks 4** / copies 2 | hits 2 |
| detsys | walks 2 / copies 2 | hits 4 |

**NOT A WIN HERE — reported honestly.** Ours shows *more* cold walks, not fewer.
The reason (documented in `run.sh`): this workload slices `/sub` off an
already-materialised `fetchGit.outPath`, so each rev's whole-repo input is
DryRun-hashed and ours additionally DryRun-hashes the deferred subtree, where the
eager trees `cp` it from the pre-materialised repo path. The cross-pipeline
tree-OID bridge (`xpipe_hits`) targets whole-input / tarball-vs-git tree-SHA
sharing and **does not fire** for a subtree sliced from a materialised store
path. The §6.1 win is real for the whole-input case but this particular workload
shape does not exercise it; massaging the workload to manufacture a win would
violate the project's own review discipline, so it stands as-is.

## Wall-clock (big fixtures: 50 KB × files, ~25 MB monorepo)

hyperfine mean, cold (store+cache wiped per run) and warm:

| workload | baseline | ours | detsys |
| --- | --- | --- | --- |
| `cargo` cold | 0.151 s | **0.092 s** | 0.213 s |
| `cargo` warm | 0.024 s | 0.023 s | 0.064 s |
| `filtered` cold | 0.038 s | 0.039 s | 0.069 s |
| `filtered` warm | 0.033 s | **0.025 s** | 0.067 s |

- **`cargo` cold: ours ≈ 1.6× faster than baseline, 2.3× faster than DetSys** —
  the copy-once-link-N saving (1 copy vs 12) shows up directly in wall time once
  the copies carry real bytes.
- **`filtered` warm: ours fastest** — the cacheable filtered source skips the
  re-copy that master/DetSys pay on every warm miss.
- DetSys carries a consistent ~25–40 ms higher floor (richer settings/feature
  parsing at start-up); at the tiny default scale that floor dominates, which is
  why timing uses the `big` scale.

### Additional comparator workloads

| workload | what it shows | result |
| --- | --- | --- |
| `filtersource` | real `builtins.filterSource` (the `lib.cleanSourceWith` primitive) | same win as `filtered`: ours warm copies 0 / uncacheable 0; baseline + DetSys re-copy |
| `parsecache` | `fromJSON (readFile X)` — the parse cache | walk-count parity (the saving is parse *time*, not walks; see microbench below) |
| `drv` | a derivation whose `src` is a filtered subtree — the `.drv` boundary | parity at walk level; exercises `derivationStrict` placeholder resolution end-to-end |

## Component microbenchmarks (Google Benchmark, single-tree)

Built with `withBenchmarks = true` (see README). These measure *our* mechanisms
directly — the cross-tree comparison can't (gbench links one tree). Numbers from
the i9-14900K; representative, not a CI gate.

### Copy-once-link-N — the cargo win, measured directly (`nix-store-benchmarks`)

64 KiB sibling files, N siblings of identical content:

| N | `CopyN_Independent` (eager) | `CopyOnceLinkN` (ours) | speedup |
| --- | --- | --- | --- |
| 4 | 495 µs | 333 µs | 1.5× |
| 16 | 1719 µs | 913 µs | 1.9× |
| 64 | 6924 µs | 3204 µs | **2.2×** |

The gap widens with N — copy cost is amortised as siblings grow, exactly the
copy-once-link-N thesis. (`SourceContentId::compute` = 188 ns.)

### MaterialisationScheduler + parse-cache (`nix-expr-benchmarks`)

| benchmark | N=4 | N=16 | N=64 |
| --- | --- | --- | --- |
| `OutPathsOf_SharedContent` (cold: walk + link-N) | 400 µs | 1009 µs | 3465 µs |
| `OutPathsOf_Warm` (peek-hit steady state) | 5.3 µs | 12.5 µs | 43 µs |

Cold scales sub-linearly in N (link-N amortising); warm is ~75× faster than cold
(the materialised-then-reused path). `ParseCacheRoundTrip` (upsert + lookup of a
Value tree in the persistent SQLite) = 8.6 µs.

### Operator-stack read path (`nix-fetchers-benchmarks`)

The proposal claims the operator algebra is a cheap "pure forward". Measured:

| benchmark | 16 files | 256 files |
| --- | --- | --- |
| `Read_BareMemory` (baseline) | 939 ns | 948 ns |
| `Read_SubsetStack` (DirectorySynthesizer∘Restrict∘Translate) | 1003 ns | 1056 ns |

The full operator stack adds only **~7–11%** over a bare read and barely grows
with tree size — direct evidence the per-read overhead is small. Plus the pure
helpers: `mergeFingerprintSuffix` 44 ns, `bareTreeOid` reject 2.5 ns,
`collectFilteredShape` ~4.5–5.9 M entries/s.

## Large-repo lazy-fetch at scale (`tests/nixos/git-lazy-fetch-scale.nix`)

A ~48 MiB repo of incompressible content (~2056 git objects) served over real
HTTP (Gitea), fetched with `git-lazy-fetch` on vs off, measuring the gitv3 cache
size (a bytes-on-wire proxy) per access pattern.

**Finding (negative — and the most important result here).** For a pinned-rev
`builtins.fetchGit { rev = …; }`, lazy-fetch pulls **~the same ~48 MiB at every
access pattern** (metadata-only, single-file read, full) as a full clone:

| access pattern | lazy fetch | full eager |
| --- | --- | --- |
| metadata (`.rev`) | ~48 MiB | — |
| single-file read | ~48 MiB | — |
| full materialise | ~48 MiB | ~48 MiB |

The protocol-level filter genuinely works (a `--filter=blob:none` probe pulls
**53 objects / 52 KiB** vs **2056 objects / 48 MiB** for a full clone), and the
partial clone *is* created — but the input's `revCount`/`lastModified`
computation walks and backfills the whole tree before any narrow per-file
prefetch runs. So **the bandwidth benefit is currently unrealised for the
dominant `fetchGit{rev}` access pattern.** Realising it needs `revCount` /
`lastModified` deferred over promisor remotes. The test asserts soundness (lazy
== eager store path + NAR hash) and reports this table; it does not fabricate a
saving.

## Coverage matrix (mechanism × covered-by)

What exercises each load-bearing mechanism, after this round of work:

| Mechanism (PROPOSAL §) | gbench (ours) | CLI comparator (3-way) | VM (real net) |
| --- | --- | --- | --- |
| Fingerprint composition (§1.1/§2.B) | ✅ fingerprint-bench | — (implicit) | — |
| Projection key encoding (§0/§2.A) | ✅ projection-bench | ✅ via cache rows | — |
| Filtered-shape walk (§2.D) | ✅ filtered-shape-bench | ✅ `filtered`/`filtersource` | — |
| Filtered-cache bypass (§6.6/§7.12) | — | ✅ `filtered`/`filtersource` | — |
| copy-once-link-N (§4/§2.O) | ✅ register-linked-ca-path-bench | ✅ `cargo` | — |
| MaterialisationScheduler `outPathsOf` (§2.O) | ✅ materialisation-scheduler-bench | ✅ `cargo` | — |
| `SourceContentId::compute` (§1.1) | ✅ | — | — |
| Operator-stack read path (§1.2–§1.4) | ✅ operator-stack-bench | — (implicit) | — |
| Parse cache (§2.E) | ✅ (round-trip) | ✅ `parsecache` (parity) | — |
| Virtual-source interpolation (§8.2) | — | ✅ `interp` | — |
| `.drv` boundary / derivationStrict | — | ✅ `drv` | — |
| Cross-rev subtree (§6.1) | — | ✅ `crossrev` (honest non-win) | — |
| Pure-eval flake-input parity (§8.5) | — | ✅ `pureflake` | — |
| Blobless partial clone + soundness (§6.3) | — | — | ✅ git-lazy-fetch.nix |
| Lazy-fetch 3-way object count | — | — | ✅ git-lazy-fetch-compare.nix |
| Lazy-fetch at scale (bytes) | — | — | ✅ git-lazy-fetch-scale.nix |
| `synthesiseTree` (§2.H) | ❌ not yet (git-fixture cost) | — | — |
| `readBlob` Phase-1/2 concurrency (§2.G) | ❌ not yet (needs parallel driver) | — | — |
| eval-cache warm path | ❌ (`--expr` isn't a flake; flake workload TODO) | — | — |

Remaining gaps are noted honestly rather than implied-covered:
`synthesiseTree` and `readBlob` concurrency have no microbenchmark yet (both need
a real git fixture / parallel driver), and the eval-cache warm path isn't in the
comparator because every workload uses `nix eval --expr` (not a flake
installable, so `openEvalCache` is never called). The on-demand blob-fetch path
itself is covered only by the VM tests — `file://` repos never trigger the
promisor.
