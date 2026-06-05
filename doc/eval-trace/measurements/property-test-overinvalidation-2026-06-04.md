# FilterList/SortList property failures — a REAL HOT-1 stat-race soundness bug I introduced (2026-06-04, CORRECTED)

> **THIS DOCUMENT'S ORIGINAL CLAIM WAS WRONG.** It asserted these two property
> failures were "over-invalidation expectation bugs, NOT soundness bugs," and
> "pre-existing on clean HEAD." Both claims were false. A reviewer asked "if you
> build those tests relative to master they work — are you sure those failures
> weren't introduced by your changes?" — and they were. The failures are caused
> by **HOT-1** (the posix content cache, commit `f4527def4`, my change), and they
> are **real soundness tests** that caught a real stat-race in HOT-1. I dismissed
> them without bisecting. Own it. The corrected analysis follows; the original
> (wrong) text is deleted, not buried.

## The two tests

- `EvalTraceProperty_FilterList.PositiveValueChange_CorrectlyInvalidates`
- `EvalTraceProperty_SortList.Reorder_CorrectlyInvalidates`

Both evaluate `builtins.length (filter/sort … (fromJSON (readFile f)))`, mutate the
JSON file by a **same-byte-size** change (a single-digit positive → another
single-digit positive; or a reorder), `invalidateFileCache`, and assert the cache
DETECTED the change (`deltaTraceCacheMisses + deltaRecoveryAttempts >= 1`). Their
own comments call them soundness tests — correctly.

## Bisect: HOT-1 is the cause (NOT pre-existing)

- On the current branch binary both fail **deterministically** in isolation (not
  "flaky / pass in some runs" as the original doc claimed).
- `NIX_EVAL_TRACE_NO_POSIX_CONTENT_CACHE=1` (the old HOT-1 disable flag) → **both
  PASS**. Default (HOT-1 on) → both FAIL. So **HOT-1 is necessary and sufficient**
  for the failures. HOT-1 landed in `f4527def4` / `47ecfdfd4` (mine). These are
  branch changes, not pre-existing.

## Root cause: HOT-1's posix freshness token can't see a same-size sub-tick edit

HOT-1's posix tier keys the cache on a freshness token
`mtime + ctime + size + inode` (`posixFreshnessToken`, verifier.cc). Its soundness
comment claimed "ctime is kernel-set on every change, so any content change moves
the token." That is FALSE for two changes within the filesystem's timestamp
granularity tick: they share an identical mtime AND ctime, so the token is
byte-identical and HOT-1 serves the stale hash.

Instrumented the HOT-1 table ops on the failing iteration (token = `pfb:len:path:
mtime:size:ctime:inode`):
```
put    inserted=1  …/test-…-3.json:1780607426.801489521:8:1780607426.801489521:85520
lookup HIT         …/test-…-3.json:1780607426.801489521:8:1780607426.801489521:85520   ← byte-identical token, stale hit
```
The store (of the pre-mutation hash) and the post-mutation lookup carry an
**identical token** — the same-size edit landed inside the mtime/ctime tick. A
passing iteration shows two *different* mtimes (`…798…` vs `…799…`) — its edit
advanced the timestamp, so the token differed and it correctly missed. The
size-changing sibling `AddPositiveElement_Invalidates` PASSES (size moves the
token); only the same-size tests fail. This is the classic stat-race of
mtime-based caches (the one git's "racy-clean" handling exists for).

## Reachability

- **Cross-process `nix eval` is sound** (measured): a JSON edited between two
  separate eval processes gets a strictly-later mtime than the recorded entry, so
  HOT-1 misses and serves the correct value (verified with same-size,
  result-CHANGING edits → always correct). The collision needs two same-size
  writes within the granularity tick, which separate processes can't produce.
- **Intra-process re-eval CAN hit it**: `nix repl` `:reload` after a same-size
  edit, or a watch/codegen loop, re-evaluates in one process with both writes
  close in time. Narrow, but real — and a default-ON soundness gap regardless.

## Fix: HOT-1 default-OFF (opt-in), property tests stand as the guard

Flipped `posixContentCacheEnabled()` to **default-off**; opt-in with
`NIX_EVAL_TRACE_POSIX_CONTENT_CACHE=1`. This makes the default sound. The store-path
H1 tier is UNAFFECTED (store paths are immutable content addresses — no race; its
soundness is independent and verified by `eval-trace-soundness.sh` Test 5, still
passing). With HOT-1 off, the two property tests PASS by default and now serve as
the regression guard: re-enabling HOT-1 unsoundly fails them.

VERIFIED after the flip: both property tests PASS; 554 property/materialization/
verify tests PASS; all 12 eval-trace functional tests PASS; nixpkgs
`asciidoc.nativeBuildInputs` cold+warm byte-identical to `--no-eval-trace`.

## Re-enable path (proper fix, deferred)

A racy-clean guard (git-style): persist each posix entry's write wall-time; on
lookup, treat a file whose mtime is within the FS granularity of that time as racy
and re-read instead of serving the cached hash. A pure mtime-vs-now window is not
provably sound without bounding the FS granularity (ZFS here showed exact-ns token
collisions for ms-apart intra-process writes), so this needs the entry-timestamp +
a granularity probe, not a fixed window. Until then, default-off.

## Dig deeper (2026-06-04): the mechanism is the COARSE realtime clock, and it's a cache CLASS

The "timestamp granularity" framing above was right in spirit but I had the granularity
wrong by ~6 orders of magnitude, and the bug is not HOT-1-only.

**Mechanism — the Linux coarse realtime clock, not nanosecond granularity.** `nix::lstat`
is a raw `::lstat` syscall (no cache), so the colliding token reflects the file's REAL
mtime/ctime — which genuinely did not advance across a same-size rewrite. Measured the
exact `ofstream(trunc)` + same-size-write pattern in a tight loop:

| filesystem | mtime/ctime collisions (same-size rewrite, ~µs apart) |
|---|---|
| ZFS (`/tmp`, this host, `CLK_TCK=100` ⇒ 10 ms tick) | **1888/2000** (94%); 99% back-to-back; **49% even ~1 ms apart** |
| tmpfs (`/dev/shm`) | **0/2000** |

So file mtime/ctime on ZFS-on-Linux is driven by the **coarse realtime clock**
(`current_time()`/`ktime_get_coarse_real_ts64`), which only advances on the timer tick
(~10 ms at HZ=100) — the nanosecond field is populated but constant within a tick. tmpfs
uses fine-grained timestamps and never collides. **The bug is filesystem-dependent**:
present on coarse-mtime FS (ZFS), absent on fine-grained ones — which is exactly why it
surfaced on this host and why a naive "mtime is nanosecond, edits always move it"
assumption is false.

**Cross-process is sound; intra-process within a tick is the gap.** A recorded entry's
mtime is the file's last write BEFORE the recording process; any later edit happens AFTER
that full process (≫ a 10 ms tick), so its mtime differs ⇒ cross-process `nix eval`
misses and serves correctly (re-verified: same-size result-changing edits across separate
processes always correct). Only intra-process re-eval within one tick (`nix repl :reload`,
a watch/codegen loop, the property-test harness) collides.

**It's a cache CLASS, not one cache — and the caches are coupled.** A second persistent
mtime-token cache exists: the **#2 git-clean marker** (`gitCleanToken`, dep-hash-fns.cc),
DEFAULT-ON, keyed on `HEAD oid + .git/index mtime+size`. It deliberately returns a stale
"clean" verdict for an UNSTAGED edit (index untouched) and relies on the FileBytes content
backstop to catch it — which I verified works with HOT-1 off (git repo on ZFS, unstaged
`[1,2]→[9,2]` → eval serves the correct `9` via FileBytes re-read; the marker is created
and stale-clean, but harmless). The dangerous coupling: HOT-1 (a FileBytes mtime-SHORTCUT)
made that very backstop stale on coarse-mtime FS, so **when HOT-1 was on, an unstaged
same-size edit defeated the git-clean marker AND its FileBytes backstop together
(intra-process) — a stale serve with no floor.** The git-clean marker's local soundness
reasoning assumed the layer below (FileBytes) was content-accurate; HOT-1 silently broke
that invariant. HOT-1 default-off restores it. (The marker's misleading "mtime ⇒ miss"
comment and a stale "default off" line were corrected in dep-hash-fns.cc.)

**Re-enable / hardening note (refined).** The granularity IS boundable after all — it is
the timer tick (≤ ~10 ms at HZ ≥ 100), not unbounded — so a racy-clean guard with a window
comfortably above it (e.g. ≥ 1 s) would be sound for re-enabling HOT-1. The general rule
for this class: an mtime/stat token is a PRECISION hint only; never let it be the
soundness floor — keep a content-based check below it, and ensure no shortcut (HOT-1) makes
that content check itself mtime-based.

## Lesson

I called soundness tests "expectation bugs" and "pre-existing" without bisecting —
twice in the original write-up, self-contradictorily ("fail on clean HEAD" AND "my
Finding-1 change shifted the seed so they fail"). The disable-flag bisect (one
command, no rebuild) would have caught it immediately. When a test you're tempted
to dismiss is labelled a soundness test, bisect which of YOUR commits flips it
before deciding it's wrong.
