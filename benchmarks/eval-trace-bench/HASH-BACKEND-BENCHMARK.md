# Eval-trace hash backend benchmark notes

Date: 2026-04-26

This note records the initial BLAKE3 vs SHA-256 comparison for eval-trace
dependency hashing. The run was done on a busy machine, so the result should be
treated as a coarse signal check, not as a stable microbenchmark.

## Setup

The benchmark command used the nixpkgs release workload with 100 commits:

```sh
nix run .#eval-trace-bench -- generate --run-number 3 --num-commits 100 --eval-trace-hash-algorithm sha256
```

The SHA-256 run produced `cold/3` and `hot/3`. The closest BLAKE3 baseline was
`cold/1` and `hot/1`, which used the Nix default hash backend and had no
`extraEvalArgs` in the run manifests. Run `2` disabled structural variant
recovery and is not a hash-backend comparison point.

The SHA-256 manifests include:

```json
"extraEvalArgs": [
  "--option",
  "eval-trace-hash-algorithm",
  "sha256"
]
```

All compared outputs matched `reference`.

## Analysis commands

These reports were generated with eval-trace-bench rather than ad-hoc JSON
processing:

```sh
nix run .#eval-trace-bench -- runs --runs reference,cold/1,cold/3,hot/1,hot/3 --reference reference
nix run .#eval-trace-bench -- pairwise --baseline cold/1 --target cold/3 --runs cold/1,cold/3
nix run .#eval-trace-bench -- pairwise --baseline hot/1 --target hot/3 --runs hot/1,hot/3
nix run .#eval-trace-bench -- series --runs cold/1,cold/3,hot/1,hot/3
nix run .#eval-trace-bench -- classify --runs cold/1,cold/3,hot/1,hot/3
nix run .#eval-trace-bench -- sv --runs cold/1,cold/3
```

For run-to-run noise context:

```sh
nix run .#eval-trace-bench -- pairwise --baseline cold/0 --target cold/1 --runs cold/0,cold/1
nix run .#eval-trace-bench -- pairwise --baseline hot/0 --target hot/1 --runs hot/0,hot/1
```

## Findings

The hot comparison is the cleanest backend signal because both `hot/1` and
`hot/3` had 100 percent hits, zero misses, and no recovery-class shifts.

| Pair | Total wall | Median ratio | Mean ratio | Band summary |
| --- | ---: | ---: | ---: | --- |
| `hot/3` SHA-256 vs `hot/1` BLAKE3 | +9.30s | 1.042x | 1.056x | SHA-256 slower by >5% on 48 commits, slower by 1-5% on 33, within +/-1% on 10, faster by 1-5% on 9 |
| `cold/3` SHA-256 vs `cold/1` BLAKE3 | +8.27s | 0.995x | 1.174x | SHA-256 faster by >5% on 8, faster by 1-5% on 34, within +/-1% on 25, slower by 1-5% on 5, slower by >5% on 28 |
| `hot/1` BLAKE3 vs `hot/0` BLAKE3 | -59.26s | 0.811x | 0.777x | BLAKE3 repeat run was faster by >5% on 84 commits |
| `cold/1` BLAKE3 vs `cold/0` BLAKE3 | -228.92s | 0.831x | 0.783x | BLAKE3 repeat run was faster by >5% on 71 commits |

The cold comparison is not hash-backend-only. `cold/3` shifted 21 commits across
recovery outcome classes relative to `cold/1`; its hit-only commits increased
from 1 to 13, recovery attempts increased from 126 to 150, and recovery
failures decreased from 120 to 116. That makes cold wall time a blend of hash
backend behavior, cache/recovery path changes, and machine noise.

The component timers show the expected local cost increase for SHA-256 in
structural-variant hashing:

| Component | `cold/1` mean | `cold/3` mean | Delta |
| --- | ---: | ---: | ---: |
| `sv.hashUs` | 4.0 ms | 31.5 ms | +27.6 ms |
| `recovery.timeUs` | 111.2 ms | 162.2 ms | +51.0 ms |
| `verify.timeUs` | 1046.3 ms | 1126.6 ms | +80.3 ms |

On the hot path, there is no SV hashing or recovery. The visible delta is in
verification:

| Component | `hot/1` mean | `hot/3` mean | Delta |
| --- | ---: | ---: | ---: |
| `verify.timeUs` | 915.9 ms | 969.6 ms | +53.7 ms |
| `verifyTrace.timeUs` | 905.3 ms | 958.2 ms | +52.8 ms |
| `loadTrace.timeUs` | 4.8 ms | 5.3 ms | +0.5 ms |

The current stats do not expose a per-hash-input byte-size histogram, so this
run cannot directly answer "which backend wins by file size bucket". The
available evidence is per-run wall time, per-kind dep-hash timers, SV hash time,
verification time, and recovery behavior.

## Conclusion

This run does not show BLAKE3 being obviously slower than SHA-256. If anything,
the clean hot-path comparison points the other direction: SHA-256 was about 4
percent slower by median wall time and about 5 to 6 percent slower by mean wall
time versus the BLAKE3 baseline.

However, the machine noise is large enough that the single-run result is not a
stable throughput estimate. The BLAKE3-to-BLAKE3 repeat delta was much larger
than the SHA-256-vs-BLAKE3 hot delta. A stronger answer needs repeated paired
runs on a quieter machine and, ideally, additional eval-trace stats that bucket
hash inputs by byte size and record backend hash time per bucket.
