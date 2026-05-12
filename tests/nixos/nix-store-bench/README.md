# `nix-store-bench`

A NixOS-VM rig for the `optimise-store` and GC throughput benches in
[`src/libstore-tests/optimise-bench.cc`](../../../src/libstore-tests/optimise-bench.cc).
Wraps the `nix-store-benchmarks` binary with filesystem / throttle /
dm-delay / bpftrace scaffolding so each cell measures the operation
itself, not the cost of repopulating a synthetic store.

## File layout

| File | Role |
| ---- | ---- |
| `adhoc.nix`         | Entry point. Takes CLI / Nix args, runs `evalTest`, surfaces assertions + warnings, returns the test derivation. |
| `bench-options.nix` | Module schema. Declares every user-facing knob (with `type` / `default` / `description`) plus derived data (`useBlockDev`, `throttleParams`, etc.) and cross-option `assertions` / `warnings`. |
| `mk-test.nix`       | NixOS test module. Reads `config.bench.*`, emits `nodes.machine` and a `testScript` (constants prelude + `readFile ./testScript.py`). |
| `testScript.py`     | Bulk of the test driver — fixture setup, throttle daemon launch, bench invocation, bpftrace gating, result extraction. Name-bound by `mk-test.nix`'s prelude (see file header). |
| `decide.py`         | `bench-decide` CLI. Two subcommands: `summary` (one JSON) and `ab` (two JSONs + bpftrace dumps + threshold flags). |
| `throttle-daemon.sh`| In-VM daemon that toggles cgroup `io.max` + dm-delay around the measured call. Lock-step protocol with the bench's `ThrottleGate` C++ class. |

There are **no Hydra-jobset cells** here on purpose. The matrix of
(fs × throttle × layout × replica × benchName) is too large and the
`decide.py` thresholds are tuned for production-relevant fixture
sizes, not small-fixture CI smoke runs. Run scenarios via `adhoc.nix`.

## Eval flow

```
adhoc.nix
  │
  │  args@{ flakeRoot ? …, … }: view binding captures only the keys
  │  the caller actually supplied; schema defaults handle the rest
  ▼
nixos-lib.evalTest
  │  imports [ bench-options.nix, mk-test.nix, { config.bench = benchArgs; } ]
  │  (benchArgs = args minus the entry-point-only `flakeRoot` key)
  │
  ├──> bench-options.nix
  │      ─ declares options.bench.* (strict types) + assertions + warnings
  │      ─ computes derived data (useBlockDev, throttleParams, …)
  │      ─ derives bench.name default if not user-set
  │      ─ builds the assertions list
  │
  └──> mk-test.nix
         ─ reads config.bench.*
         ─ emits config.nodes.machine and config.testScript
         ─ testScript = constants prelude + readFile ./testScript.py

after evalTest returns:
  adhoc.nix
    ─ checks failedAssertions (throws on any)
    ─ lib.showWarnings on config.warnings
    ─ deepSeq config.bench (forces eager type checks)
    ─ returns config.test
```

The `runTest`-equivalent code is inlined into `adhoc.nix` rather than
in a separate `mk-bench.nix`, because there's only one entry point.

### Auto-derived test name

If the caller doesn't supply `name`, `bench-options.nix` derives one
from every distinguishing field:
`adhoc-<benchName>-<fs>-<throttle>-<layout>-<replica>-n<nPaths>-t<threads>[-multi]`.
Two scenarios that differ in any visible parameter get different
derivation hashes — no silent cache collisions.

## Why the assertions are surfaced manually

`runTest`'s nixosTest class does not declare `options.assertions` or
`options.warnings`. There's an explicit comment to that effect in
`nixos/lib/testing/legacy.nix:11`. Our module declares both itself,
and `adhoc.nix` reads them after `evalTest` — `throw`ing on failed
assertions and printing warnings via `lib.showWarnings`.

## CLI examples

Run a default GC bench on ext4:

```
nix build --no-link -L -f tests/nixos/nix-store-bench/adhoc.nix
```

Override individual fields. The schema is strict about types: pass
string-valued options with `--argstr` and everything else (ints,
bools, null) with `--arg` plus a Nix expression.

```
nix build --no-link -L -f tests/nixos/nix-store-bench/adhoc.nix \
  --argstr benchName gc_clusters \
  --argstr fs btrfs --argstr throttle nvme \
  --argstr layout sharded --argstr replica multi \
  --argstr dispatchOnly syscall \
  --arg nPaths 10000 --arg threads 4
```

Two caveats on these flags:

- **Thread axes aren't validated by the schema.** The `nPaths`
  assertion only checks the path count, so unregistered
  `(nPaths, threads[, threads2])` combos eval cleanly and then fail
  at run time with `Failed to match any benchmarks`. Pick a row from
  `src/libstore-tests/optimise-bench.cc`: `threads = 4` is registered
  for every cell; `1` / `16` only on parts of the
  `flat_single_replica_hardlink` baseline (raise `cores` for `16`).

- **GC benches without `dispatchOnly` run a regression-gated A/B.**
  The test feeds both `syscall` and `iouring` JSONs into
  `bench-decide ab`, which checks three things: VFS-op parity (≤ 5%
  delta, a correctness check), syscall ratio (`b/a ≤ 1.05`, i.e. up
  to 5% more syscalls allowed), and wall improvement
  (`(a-b)/a ≥ -0.05`, i.e. up to 5% slower wall allowed). The
  defaults are a "no regression vs syscall path" gate, not a
  "demand a 10% win" gate — io_uring's per-worker-rings design
  hits parity with the syscall path on the canonical
  `gc_barabasi/ext4/flat/single` scenario; demanding a win would
  flag a passing run on the very scenario the code was tuned for.
  To demand an actual win for performance investigations, pass
  `--wall-improvement 0.10` etc. when running `bench-decide ab`
  outside the VM on the copied JSONs. A `VERDICT: FAIL` from the
  in-VM run is a real regression; check the printed `vfs=` parity
  first to confirm kernel work is still comparable. To skip the A/B
  step entirely (e.g. when iterating on something unrelated), pass
  `--argstr dispatchOnly syscall` (or `iouring`) to switch the
  in-VM step to `bench-decide summary` (no thresholds).

Programmatic (from another `.nix` file):

```nix
import tests/nixos/nix-store-bench/adhoc.nix {
  benchName = "optimise_with_concurrent_gc";
  threads = 4;
  threads2 = 4;
  fs = "ext4";
  throttle = "io2";
}
```

## `bench-decide`

After a test finishes, the JSON / bpftrace artefacts are decoded by
`bench-decide`:

```
# Summary of one JSON
bench-decide summary /tmp/syscall.json

# A/B with default thresholds (syscall vs iouring)
bench-decide ab /tmp/syscall.json /tmp/iouring.json \
                /tmp/syscall.bpf.txt /tmp/iouring.bpf.txt

# A/B with custom labels and loosened wall-time threshold
bench-decide ab a.json b.json a.bpf b.bpf \
                --a-name baseline --b-name patched \
                --wall-improvement 0.05
```

Run `bench-decide ab --help` / `bench-decide summary --help` for the
full flag list.
