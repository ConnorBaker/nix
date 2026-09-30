# Exact f307 versus 8aad warm-8 Git diagnostic

The existing ten-pair benchmark remains the timing authority: f307 warm-8
`git+file:` CPU is +56.25% against 8aad in paired median, with all ten
pairs higher. This one diagnostic used the exact sealed f307 and 8aad
binaries, the original 8-attribute expression, unchanged Nixpkgs fixture
and affinity, two fresh private states, one seed and one user-FP profile
per endpoint. All four eight-row JSON outputs exactly matched their own
pinned cohort references. Pre- and postflight complete 89,979-node source
provenance checks and binary hashes passed; selected mapped SQLite,
OpenSSL, GC and libgit2 files matched the bound hashes. `run.json` and
`PLAN.json` retain exact argv, times, output hashes, source pins and
host pressure. The private states were retired under the checked
`state-retirement-receipt.json`; both paths are absent.

| Exact capture | GNU-time user / system seconds | User-FP samples | Git status inclusive | Memo naming inclusive | Regular hash / POSIX read | copyFdRange |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 8aad | 0.41 / 0.59 | 206 | 85 | 0 | 0 / 0 | 0 |
| f307 | 0.59 / 0.95 | 285 | 64 | 104 | 47 / 47 | 36 |

The profile runs therefore used +0.18 s user and +0.36 s system time
for f307, consistent in direction with the repeated CPU regression.
These are single instrumented runs; the individual times are not a
replacement for the benchmark. Every one of f307's 36 observed
`copyFdRange` chains contains `MemoVisitor::node`,
`HashingVisitor::regular`, and `PosixDirectorySourceAccessor::readFile`.
The current branch also has 39 selected SHA leaf samples under memo
naming, versus zero under that route in base; base has 8 selected SHA
samples under Git status versus 6 in f307. This directly observes
current raw-worktree hashing separately from libgit2 status, without
counting invocations or bytes. Status samples fall 85→64, so these
profiles do not support blaming *more libgit2 status CPU* for the extra
user time.

Each `perf report` reported zero lost samples. All 206 and 285 rendered
samples have parseable frame lines, and each leaf DSO partition sums to
its sample total (`PROFILE-COUNTS.json`). The script has 56 and 66
chains, respectively, rendered at the requested 127-frame boundary;
their ancestry is incomplete. There was no raw/native callchain-address
reconciliation in this packet, and inlining/tail calls are not recovered.
The profile event is `task-clock:u`, sampled at 499 Hz, and excludes
kernel time. Inclusive routes overlap; samples are not syscall counts or
a causal partition of CPU seconds.

Source separates the semantic routes. `GitRepoImpl::getWorkdirInfo`
(`src/libfetchers/git-utils.cc`) computes tracked membership and dirty
metadata using libgit2 status. The workdir accessor at the same file's
`getAccessor` wraps the actual filesystem and filters by membership;
`git.cc` deliberately clears `cachedFingerprint` because a clean Git
status/HEAD identity cannot prove mutable file bytes (index flags and
filters can hide differences). `MemoVisitor::node`
(`src/libfetchers/fetch-to-store.cc`) descends on an unnamed/missing
entry. `PosixDirectorySourceAccessor::readFile`
(`src/libutil/posix-source-accessor.cc`) opens the selected file, fstats
it, then calls `copyFdRange`, which reads its bytes into the hash sink.
These operations can be required by the current source's meaning.
Historical base profiles also have a different NAR traversal that can
read the worktree, so a new named raw-reading route is not by itself
proof of a redundant full read.

Exact runtime bindings differ: base uses libgit2 1.9.7 and zlib 1.3.2,
while f307 uses libgit2 2.0.0-rc.1 and zlib-ng 2.3.3. SQLite 3.51.2,
OpenSSL 3.6.3 and GC 8.2.12 file hashes match. These library changes
confound causal attribution to Nix source alone.

The remaining source of the larger **system-time** delta is not located
by this user-only profile. The planned FD-resolved strace phase was
omitted: a tiny `strace true` probe failed with `PTRACE_TRACEME: Operation
not permitted` under this environment. No permission escalation was
available. Earlier all-process syscall aggregates belong to older branch
endpoints and cannot substitute for exact f307 FD/callsite attribution.
The checked claim is that the exact current binary spends observable user
CPU naming mutable worktree bytes; the claim that this route causes all
of the regression, or that libgit2/kernel work does not contribute,
remains unproved.
