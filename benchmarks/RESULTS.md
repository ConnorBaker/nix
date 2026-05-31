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

## What this harness does *not* cover

The on-demand blob-fetch / blobless-clone path (`NIX_GIT_LAZY_FETCH`,
`GitPromisorProvider`) is **not exercised here** — `file://` repos never trigger
the promisor. That is the job of the NixOS VM test
(`tests/nixos/git-lazy-fetch.nix`), which stands up a real git HTTP server
(Gitea) advertising protocol-v2 `filter`. See that test for the filtered-fetch
proof (via GIT_TRACE), the lazy-vs-eager soundness check, and
`git-lazy-fetch-compare.nix` for the in-VM object-count comparison.
