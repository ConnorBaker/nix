# Lazy-store benchmarks

A three-layer harness for measuring the source-materialisation work described in
[`doc/tecnix-survey/PROPOSAL.md`](../doc/tecnix-survey/PROPOSAL.md) against the two
precedents it compares to. Each layer owns the claims it can actually reach;
together they cover the PROPOSAL §4 / §8.5 head-to-head and the README §6
empirics.

| Layer | Tool | What it measures | Cross-tree? |
| --- | --- | --- | --- |
| **CLI semantic + wall-clock** | this dir (`run.sh`) | tree *walks*, *copies*, cache-writes, and hyperfine time across 8 workloads | **3-way** (ours / upstream-master / DetSys) |
| **Component microbench** | `src/lib{store,expr,fetchers}-tests/*-bench.cc` (gbench, `-Dbenchmarks=true`) | in-process cost of *our* mechanisms: copy-once-link-N, the MaterialisationScheduler, the parse cache, the operator-stack read path, fingerprint composition, projection cache, filtered-shape walk | single-tree (regression guard) |
| **Real-network e2e** | `tests/nixos/git-lazy-fetch{,-compare,-scale}.nix` | blobless/partial-clone correctness + soundness, a 3-way object count, and a ~48 MiB scale measurement over a real git HTTP server | in-VM |

Why three layers and not one: gbench links one tree's libraries, so a benchmark
using our `SourceContentId` / `MaterialisationScheduler` types cannot compile
against DetSys or upstream-master — it is inherently single-tree. The
cross-tree comparison is therefore a black-box CLI job (`run.sh`). And neither
of those exercises the on-demand blob-fetch path at all: that needs a real git
server advertising protocol-v2 `filter`, which only the NixOS VM tests provide
(`file://` repos never trigger the promisor).

[`RESULTS.md`](./RESULTS.md) has a **coverage matrix** (mechanism × which layer
exercises it), captured numbers for every layer, and the large-repo finding —
including which mechanisms are *not* yet covered, stated explicitly.

## The three trees

| Name | What | Binary |
| --- | --- | --- |
| `baseline` | upstream master `2d309b18e` — *prior to our changes* (our merge-base) | built on demand into `.baseline-nix` (see below) |
| `ours` | this tree | `../build/src/nix/nix` |
| `detsys` | DeterminateSystems/nix-src, run with `lazy-trees = true` | `~/ext-sources/nix-src/build/src/nix/nix` |

Override any path with `BENCH_BIN_BASELINE` / `BENCH_BIN_OURS` / `BENCH_BIN_DETSYS`,
choose the set with `BENCH_TREES="ours baseline"`, and (if built) add `tecnix`.

Build the baseline once:

```bash
nix build -L ".?rev=2d309b18e5a3a8734f4e64e659dac1813964c241#default" \
  --out-link benchmarks/.baseline-nix
```

## Running the CLI harness

```bash
cd benchmarks
./run.sh                      # all workloads, semantic metrics, 3 trees
./run.sh --time               # also hyperfine wall-clock
./run.sh -w filtered -w cargo # only these workloads (repeatable)
./run.sh --json out.json      # machine-readable
BENCH_SCALE=big ./fixtures.sh && ./run.sh --time   # large files: wall-clock wins become visible
```

### Isolation

Every invocation runs against a private chroot store + state dir + a clean
`nix.conf` + a redirected `HOME` (so the per-user `~/.cache/nix` fetcher cache is
private too). No daemon. This guarantees controlled cold/warm states and stops
the trees cross-polluting each other's caches. Modelled on the functional-test
isolation in `tests/functional/common/vars.sh`. See `lib.sh`.

### The semantic metric

Walks and copies are counted by running `nix eval -vvvv` and grepping the
`fetch-to-store` debug Activity lines — the same technique as
`tests/functional/source/single-walk-fusion.sh`:

| Marker (in the `-vvvv` log) | Meaning |
| --- | --- |
| `hashing '<path>'` | a NAR-hash **walk** (DryRun) |
| `copying '<path>' to the store` | a **copy** (walk + store write) |
| `is uncacheable` | the fetcher-cache **bypass** (README §6.6) |
| `cache hit` | a fetcher-cache hit |
| `cross-pipeline cache hit` | our tree-OID bridge fired (ours only) |

These are deterministic and machine-independent — they are how the PROPOSAL
states its claims ("1 walk vs N"). Wall-clock (hyperfine) makes them tangible
but is noisy and machine-specific; at the default tiny fixture scale it is
dominated by process start-up, so use `BENCH_SCALE=big` for timing.

## Workloads

| Name | PROPOSAL/README ref | Claim under test |
| --- | --- | --- |
| `filtered` | §6.6 / §7.12 | filtered source (`lib.cleanSource` shape) is cacheable on ours; bypasses the fetcher cache (re-copies every cold eval) on master+DetSys |
| `cargo` | §4 | N `builtins.path` over subtrees of one rev → **1 copy + N−1 hardlinks** (ours) vs N copies (eager trees) |
| `crossrev` | §6.1 | same subtree across two revs — see the honest note in `run.sh`; the cross-pipeline bridge does *not* fire for a subtree sliced off a materialised input |
| `pureflake` | §8.5 | metadata-only read of a flake input → 0 copies on all lazy trees (**parity**) |
| `interp` | §8.2(2) | interpolate a virtual source into a discarded string → **0 copies** (ours) vs 1 (eager trees, incl. DetSys) — the ZonePath footgun |
| `parsecache` | §2.E | `fromJSON (readFile X)` — the parse cache (walk-count parity; the win is parse *time*, see the gbench round-trip) |
| `drv` | — | a derivation whose `src` is a filtered subtree — the `.drv` write / `derivationStrict` placeholder-resolution path |
| `filtersource` | §6.6 | real `builtins.filterSource` (the `lib.cleanSourceWith` primitive) — same cache-bypass win as `filtered` |

See [`RESULTS.md`](./RESULTS.md) for captured runs across all layers, the
coverage matrix, and the large-repo scale finding.

## Files

- `lib.sh` — isolation, binary resolution, metric extraction (sourced).
- `fixtures.sh` — generates the git-repo fixtures (deterministic; `BENCH_SCALE=big` for timing).
- `run.sh` — the workload runner.
- `RESULTS.md` — a captured run with analysis.
- `.fixtures/`, `.work/`, `.baseline-nix`, `*.json` — generated, git-ignored.
