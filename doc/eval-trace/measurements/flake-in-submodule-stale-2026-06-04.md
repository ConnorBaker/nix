# `flakes / flake-in-submodule` stale serve — investigation (2026-06-04, v9: FIX LANDED)

> **FIX LANDED (commit `4e9862ce8`).** H1 is now gated on source immutability:
> `h1StorePathKey` skips any flake-graph node whose input is not locked
> (`Input::isLocked`), threaded via `FlakeGraphAuthorityNodeSpec.sourceIsImmutable` →
> `SemanticRegistry::nonImmutableSources_` → the verifier gate. Additive: locked flakes +
> file-eval unchanged. VERIFIED: `flakes/flake-in-submodule` now PASSES; `eval-trace-soundness`
> (H1 Test 5) still PASSES; all 12 eval-trace functional + 425 eval-trace unit tests PASS;
> nixpkgs `asciidoc.nativeBuildInputs` cold+warm byte-identical to `--no-eval-trace`; the
> dirty→dirty chain serves live content at every edit. BENCH (eval-trace-bench run 3, 100
> closures.gnome commits, release binary): SOUNDNESS 100/100 PASS (cold+hot byte-identical to
> `--no-eval-trace`); PERF-NEUTRAL (cold 3.38s = 0.50× ref, hot 0.87s = 0.13× ref, ref 6.71s —
> hot matches the documented ~0.9s baseline; expected, the locked bench flake leaves
> `nonImmutableSources` empty ⇒ H1 unchanged). ADVERSARIAL PASS (subagent, 6 cases +
> code review): NO soundness hole — commit-after-dirty re-locks correctly (new narHash),
> runtime roots sound by content-addressing, per-node granularity holds, gate covers
> consult+populate, HOT-1 doesn't cache the carrier. NOTE: `nix build .` is still red but now
> ONLY on two PRE-EXISTING flaky property tests (`SortList.Reorder` /
> `FilterList.PositiveValueChange`, see `property-test-overinvalidation-2026-06-04.md`) — NOT
> flake-in-submodule.

`nix build .` fails its check phase on `flakes / flake-in-submodule`. Deep-dived per
repeated "dig deeper / are you sure?" requests. This file's earlier versions were
WRONG; superseded retractions are at the bottom. Everything below was instrumented
+ measured (debug instrumentation since reverted), not inferred. **v8 status: ROOT CAUSE
PINNED (instrumented + isolated) to H1 — the persisted store-path→content-hash cache
(`FileContentHashes`, landed 2026-06-01). H1 caches the COMMITTED content hash keyed by the
flake-source CARRIER MOUNT path (`<store>/submodule/sub.nix`), which eval-trace keeps STABLE
across the dirty edit while its backing content is live (v1→v2). H1's "store paths are
immutable ⇒ never stale" assumption is VIOLATED for that stable-but-mutable mount path, so it
returns the committed hash and the committed trace verifies green. ISOLATION PROOF: an
env-gated H1 bypass (single-line, `lookupFileContentHash` returns nullopt) makes the dirty
eval serve the CORRECT v2 — H1 is necessary AND sufficient. H1 is WRITE-ONCE, so it holds the
committed v1 hash permanently — which is why EVERY dirty edit serves v1, never the latest
dirty content. Fix is eval-trace-side and surgical (H1 gate). Earlier fix directions "dirty ⇒
uncacheable" (v5) and "fetcher/gitlink freeze" (v6) WITHDRAWN; v7's "eval-trace cache" was
right but unspecific — it is H1 specifically.**

## ROOT CAUSE PINNED (v8): H1 — store-path→content-hash cache over a stable-but-mutable mount path

Instrumented the three candidates (resolved-graph node identity, FileBytes dep resolution,
session keys) at `flake.cc:673` / `dep-resolution-service.cc:287` / the session-open adapter
(logs reverted after measuring; clean rebuild verified). Committed vs the stale dirty eval,
sharing one cache:

| | committed | dirty (stale) | |
|---|---|---|---|
| `graphDigest` (primary key) | `f6b3171b` | `24fc7409` | **differ** ⇒ dirty is a primary MISS |
| `sourceIdentity` | `5fc78e22` | `5fc78e22` | same |
| `stableRecoveryKey` | `d561fe9f` | `d561fe9f` | **same** ⇒ history-bootstrap can bridge |
| carrier mount storePath | `g554339q…` | `g554339q…` | **same** (stable mount) |
| node `narHash` | `OLmio/H04…` | `OLmio/H04…` | same |

The decisive read: **`--no-eval-trace` mounts the SAME `g554339q…` path yet serves `v2-dirty`**
— so the mount path is a STABLE identity with MUTABLE backing content (v1 at commit-time, v2
when dirty). And the `XSUB resolve` log (`computePathHashedDep`, the live re-hash) **never fired
on the dirty eval** — the FileBytes resolution was short-circuited. `NIX_SHOW_STATS` says why:
`depHash.fileContentCacheHits=2`. It is **H1** (the persisted `FileContentHashes` cache, keyed
by resolved store path, consulted in front of the live re-hash).

**Isolation proof (airtight).** Added an env-gated bypass to `lookupFileContentHash` (return
`nullopt` when `XSUB_NO_H1` is set), rebuilt, two fresh committed→dirty runs:

```
RUN X (H1 active):   dirty served = v1-committed (STALE)
   XSUB H1 hit storePath=<store>/submodule/sub.nix   (and flake.nix)
RUN Y (H1 bypassed): dirty served = v2-dirty   (CORRECT)
   XSUB H1 BYPASS storePath=<store>/submodule/sub.nix
   XSUB resolve key=/sub.nix path=<store>/submodule/sub.nix   ← live re-hash now runs
```

Bypassing H1 (and nothing else) fully fixes the serve. So **H1 is necessary AND sufficient** for
the stale serve. With H1 out of the way, `computePathHashedDep` reads the live mount content
(v2), the committed trace's recorded `v1` hash mismatches, the trace is rejected, and eval
re-runs → `v2`.

**Mechanism.** A flake source is mounted by eval-trace at a stable CARRIER path (so recorded dep
keys stay resolvable across sessions). For the dirty submodule eval the carrier path is unchanged
from the committed eval (`g554339q…`), but its backing content is the live working tree. H1's
gate (`h1StorePathKey`: kind ∈ {FileBytes,RawBytes} ∧ source Registered ∧ `isInStore`) accepts
this path and H1b populates it at cold-record with the committed `v1` hash. H1's soundness rests
on "the store path is a NAR content address ⇒ immutable ⇒ an entry is never stale" — TRUE for a
LOCKED flake source (fixed rev ⇒ fixed path), FALSE for the carrier mount of an UNLOCKED/dirty
source (stable path, live content). So H1 returns the committed `v1` for the dirty eval and the
committed trace verifies green.

**Adversarial pass (scrutinising this conclusion).**
- *Sole anchor?* Yes — the single-line H1 bypass is sufficient to serve the correct value (RUN Y);
  nothing else changed. Necessary too: with H1 present (RUN X) it stale-serves.
- *Is history-bootstrap a blind-trust path that serves without verifying deps?* No — with H1
  bypassed, bootstrap REJECTS the committed trace (RUN Y re-evals to v2). Bootstrap does verify
  deps; H1 is what corrupts that verification.
- *Why always `v1`, never the latest dirty (`v2`→`v3`→`v4`)?* Because H1 is WRITE-ONCE ("first
  writer wins", `putFileContentHash`): it holds the committed `v1` hash permanently, so every
  dirty eval's FileBytes dep resolves to `v1` via H1 and only the `v1` trace ever verifies. This
  matches the measured "every dirty edit serves v1" exactly — a strong consistency check.
- *Relationship to the earlier "two-phase content-blind dirty key".* That described a SYMPTOM
  (the content-blind `K_dirty` and the identity-blind `stableRecoveryKey` let the committed trace
  be FOUND via primary-promotion / bootstrap). H1 is the ANCHOR (it makes the found trace's deps
  VERIFY against live-dirty source). Without H1, the content-blind key is harmless: the live
  FileBytes re-hash catches the change and rejects the trace (RUN Y). So the v5 "two-phase" write
  is accurate about trace-matching but H1 is the verification-corruption root.
- *Stable-but-mutable mount path — measured or assumed?* Measured: the H1 HIT in RUN X proves the
  key string is identical committed-vs-dirty; RUN Y reads `v2` from the same-named path. NOT yet
  separately isolated: WHICH layer keeps the carrier path stable (carrier-mount remap vs warm
  fetcher-cache path reuse). That detail is downstream of H1 for the SERVE — H1 is the fix point
  regardless — but it is the thing that makes H1's immutability premise false and is the next
  instrumentation target if the fix is taken at the mount layer instead.

**Fix direction (surgical, eval-trace-side).** Gate H1 on genuine immutability: only cache/serve
when the resolved path is truly content-addressed. For flake sources that means "the governing
flake input is LOCKED" (fixed rev / narHash). Skip H1 for unlocked/dirty flake sources, whose
carrier mount is stable-but-mutable. This matches H1's own soundness theorem (cache only immutable
paths) and is the minimal change. (It also, harmlessly, foregoes H1 caching for plain dirty
flakes while iterating — they are already correct via content-addressed paths, so it is only a
small perf give-up on a dirty tree, not a correctness change.) Alternative (bigger): make the
carrier mount content-distinct for dirty flakes so H1 keys differ per content — closes it at the
mount layer but touches the carrier machinery. Prefer the H1 gate.

## Established facts (high confidence)

- **eval-trace-specific + pre-existing.** Repro: dirty a submodule's `sub.nix`,
  re-eval `#sub`. eval-trace ON → stale "v1"; `--no-eval-trace` → "v2-DIRTY"
  (= master). Old `result/bin/nix` (f1398d126) fails identically; deferFlush ruled
  out. Upstream test (`4b9735b76`, Eelco 2025-09-25, on master).
- **Only stale AFTER a committed eval.** Dirty-from-start on a FRESH cache, eval-trace
  ON → "v2-DIRTY" (correct). So eval-trace CAN evaluate dirty; the staleness needs a
  prior committed eval warming a cache.
- **The parent `narHash` is identical committed-vs-dirty — but that does NOT mean the
  evaluated source is committed.** A submodule is a gitlink in the parent tree, so the
  parent `narHash` does not change on submodule-worktree dirt. An EARLIER instrumentation
  pass read the resolved-graph root node's `narHash` as identical and (wrongly) inferred
  "evaluates the committed copy v1". A LATER, more direct pass instruments the actual
  `state.evalFile` site (flake.cc:484) and shows it reads **dirty** `sub.nix="v2-DIRTY"`
  under BOTH eval-trace and `--no-eval-trace`. So the live source eval-trace evaluates is
  DIRTY; the "v1" staleness does NOT come from evaluating a committed source — it comes
  from RECOVERY (Finding 2). The narHash-identical observation is real; its "→ committed
  source" interpretation was the misread (gitlink: narHash-identical ≠ content-identical).

## Finding 1 — a real CanonicalHashBuilder bug (proven; but NOT this stale serve)

`computeLockedVersionIdentity` (flake.cc) hashes `field("kind", "git-rev")` (committed)
vs `field("kind", "dirty-rev")` (dirty). Measured: BOTH produce the SAME hash. Root
cause: **a string literal passed to `CanonicalHashBuilder::field` resolves to the
`field(std::string_view, bool)` overload** — `const char*`→`bool` is a *standard*
conversion that beats the *user-defined* `const char*`→`string_view` — so literal
field VALUES hash as `true`, and distinct literals collide ("git-rev" == "dirty-rev"
== "target-key" == …). Proven: with an explicit `std::string_view("dirty-rev")` the
hash differs from the literal form, and from `git-rev`. 17 `field(tag,"literal")` call
sites are affected (most harmless — a constant literal always hashes `true`; the bug
bites only where two DIFFERENT literals must distinguish cases).

**Fix — LANDED + TESTED.** Added
```cpp
void field(std::string_view tag, const char * value) { writeFrame(tag, std::string_view(value)); }
```
to `canonical-hash.hh`, bumped `kSchemaEpoch` 25→26 (it changes every literal-valued
hash preimage), updated the `SessionKey_PinnedDigest_RegressionGuard` pin, and added an
ISOLATED regression test `CanonicalHashBuilderOverload.StringLiteralFieldValueIsNotHashedAsBool`
(store/hash.cc) that FAILS without the fix (`field("k","alpha")` == `field("k","beta")`
== `field("k",true)`) and PASSES with it. Verified the fix is SAFE: the only suite
breakage it introduces is the pinned-digest guard (expected). NOTE: two property tests
`EvalTraceProperty_FilterList.PositiveValueChange_CorrectlyInvalidates` and
`EvalTraceProperty_SortList.Reorder_CorrectlyInvalidates` fail on the CLEAN tree (HEAD,
without the fix) — a SEPARATE, pre-existing list-invalidation soundness gap (`misses +
recovery == 0` when a sorted/filtered list value changes), unrelated to Finding 1 or 2.

**BUT it does NOT fix flake-in-submodule:** with the fix in place the dirty eval STILL
returns "v1". The fix makes the session reuse key correctly DIFFER committed-vs-dirty
(so the dirty eval is now a primary cache MISS), but the dirty eval then falls into
history-bootstrap RECOVERY and serves the committed trace anyway (Finding 2). So
Finding 1 is a real bug found along the way, not the cause of this test.

## Finding 2 — ruling out the wrong layers

What the stale serve is NOT (each measured, each a prior hypothesis I had to discard):
- NOT a committed source copy / reused carrier. The flake source eval-trace evaluates is
  DIRTY: instrumented `getFlake`'s `evalFile` (flake.cc:484) reads `sub.nix = "v2-DIRTY"`
  under BOTH eval-trace and `--no-eval-trace` (different store paths, both dirty). An
  earlier "committed carrier reused" hypothesis was WRONG — the source is dirty; the
  staleness is downstream of evaluation, in recovery.
- NOT the eval-trace session reuse key. With Finding 1's fix the session reuse key
  DIFFERS committed-vs-dirty (measured) → the dirty eval is a primary cache MISS.
- NOT the nix flake/attr eval cache (`--option eval-cache false` still stale).

## Finding 2 — ROOT CAUSE (verified, TWO-PHASE): a content-blind dirty session key, poisoned by history-bootstrap

The stale serve is not a single event — it is two phases, and the dirty session key is
**content-blind** (the same hash for every dirty content version of a submodule). Per-step
counters from a clean cache (standalone repro, `outputs/out/bin/nix`; `served` is the
`#sub` value; `histBoot` = `recovery.historyBootstraps`):

```
Step0  committed cold     served=v1-committed  hits=0 miss=2 record=2 histBoot=0
Step0b committed warm      served=v1-committed  hits=2 miss=0 record=0 histBoot=0   (primary hit, correct)
Step1  FIRST dirty  v2     served=v1-committed  hits=2 miss=0 record=0 histBoot=2   (PHASE A: bootstrap serves v1)
Step1b FIRST dirty  v2     served=v1-committed  hits=2 miss=0 record=0 histBoot=0   (PHASE B: primary hit on poison)
Step2  SECOND dirty v3     served=v1-committed  hits=2 miss=0 record=0 histBoot=0   (PHASE B, DIFFERENT content)
Step3  THIRD dirty  v4     served=v1-committed  hits=2 miss=0 record=0 histBoot=0   (PHASE B, DIFFERENT content)
control --no-eval-trace at v4: "v4-dirty"   (always correct)
```

**Phase A (first dirty eval).** `histBoot=2`: a primary MISS under the dirty session key
`K_dirty` (≠ committed `K_committed` thanks to Finding 1's fix) falls into
**history-bootstrap recovery** — `scanHistory(stableRecoveryKey, attrPath)` — which finds
the committed `#sub` trace and serves stale "v1". The `stableRecoveryKey` is
**byte-identical** committed-vs-dirty (measured `85ba8c35…`): `computeStableRecoveryKey`
(adapter:69-77) = `computeOriginalSourceIdentityHash` = a hash of the ORIGINAL flakeref
(`git+file://…?submodules=1`), blind to working-tree dirt. Confirmed independent of
structural recovery: `--option eval-trace-structural-recovery false` does NOT change it.
The committed trace's `FileBytes` dep references the committed, content-addressed
(immutable) source store path (still "v1"), so it re-verifies green. **Crucially, the
bootstrap also PROMOTES that trace under `K_dirty`** — `record=0` (it is not a new trace
recording) but a persistent `CurrentNodes` write keyed by `K_dirty`.

**Phase B (every subsequent dirty eval, including DIFFERENT dirty content).** `histBoot=0,
hits=2`: a plain PRIMARY HIT under `K_dirty` on the promoted poison. This is the decisive
generalization: `K_dirty` is **content-blind** — v2, v3, v4 all hash to the same
`K_dirty` (if it varied with content, Step2 would re-bootstrap; it shows `histBoot=0`).
For a dirty submodule there is NO content-distinguishing identity: the parent tree
`narHash` is gitlink-stable (submodule-worktree dirt doesn't change the parent's recorded
gitlink), and `dirtyRev` = parent-HEAD + "-dirty" (git.cc:1147, HEAD-based, also content-
blind). So **every** dirty edit stale-serves "v1" — measured v2…v5 all serve "v1" while
`--no-eval-trace` serves the live content each time.

Why dep-verify can't save it: a flake source is a content-addressed store path, so a
recorded `FileBytes` dep on the committed path ALWAYS re-verifies. Source-change detection
must come from the IDENTITY layer — and for a dirty submodule the identity layer has no
content-distinguishing material at all.

## CORRECTION (measured): dirty trees DO work — and the freeze is the EVAL-TRACE cache, not the fetcher

The Phase-A/B analysis above is correct about the eval-trace MECHANISM (bootstrap + primary
hit on a frozen key). But it led me to a WRONG fix direction ("dirty ⇒ uncacheable"). A
controlled plain-vs-submodule probe (`/tmp/dirty-identity-probe.sh`, `outputs/out/bin/nix`,
`et`=eval-trace served, `nt`=`--no-eval-trace` served, `path`/`narHash` from `nix flake
metadata --json`):

```
EXPERIMENT A — PLAIN dirty flake (sub.nix tracked at flake root): eval-trace WORKS
A0 committed  et="v1-committed" nt="v1-committed"  path=sh62…  narHash=90ub…  store sub.nix="v1-committed"
A1 dirty v2   et="v2-dirty"     nt="v2-dirty"      path=1x10…  narHash=jFZY…  store sub.nix="v2-dirty"   hits=1 rec=1
A2 dirty v3   et="v3-dirty"     nt="v3-dirty"      path=jrav…  narHash=5bK7…  store sub.nix="v3-dirty"   hits=1 rec=1

EXPERIMENT B — SUBMODULE dirty flake (the failing case): source path FROZEN
B0 committed  et="v1-committed" nt="v1-committed"  path=18f2…  narHash=OHEa…  store submodule/sub.nix="v1-committed"
B1 dirty v2   et="v1-committed" nt="v2-dirty"      path=18f2…  narHash=OHEa…  store submodule/sub.nix="v1-committed"  (STALE)
B2 dirty v3   et="v1-committed" nt="v3-dirty"      path=18f2…  narHash=OHEa…  store submodule/sub.nix="v1-committed"  (STALE)
```

Decisive reads:
- **Plain dirty flakes content-address correctly.** Each dirty edit is fetched to a NEW
  store path with a NEW narHash (`sh62→1x10→jrav`, `90ub→jFZY→5bK7`); the store copy holds
  the dirty content. eval-trace's FileBytes deps re-resolve against the new path, mismatch,
  and it re-records (`rec=1`) and serves the live value. **Dirty works.** So a blanket
  "dirty ⇒ uncacheable" is wrong — it would disable this already-correct case.
- **Experiment B's frozen `18f2…`/`OHEa…` is a CACHE ARTIFACT, not a fetcher fundamental.**
  Experiment B reused ONE `XDG_CACHE_HOME` across B0/B1/B2 and read `path`/`narHash` from
  `nix flake metadata` — which returned the cached COMMITTED values. Re-run with a FULLY
  FRESH cache per fetch (`/tmp/submodule-fresh-fetch.sh`):
  ```
  committed  rev=93f37fbc  narHash=XBtY…  storePath=5vi4…  store submodule/sub.nix="v1-committed"
  dirty v2   rev=null      narHash=tSWg…  storePath=knq8…  store submodule/sub.nix="v2-dirty"
  dirty v3   rev=null      narHash=zaTn…  storePath=5rhx…  store submodule/sub.nix="v3-dirty"
  ```
  The git fetcher DOES content-address dirty submodule content: narHash, store path, and the
  store copy's `submodule/sub.nix` all move per dirty edit. So "gitlink freezes the parent
  narHash" (the v6 claim) is FALSE — a fresh fetch reflects the dirty submodule fine.

So the freeze is NOT the git fetcher. Locating it (`/tmp/submodule-which-cache.sh`): after a
committed eval-trace eval + dirty edit, **clearing ONLY `eval-trace*.sqlite` (leaving the
warm `fetcher-cache-v4.sqlite`) makes eval-trace serve the correct `v2-dirty`.** With the
eval-trace cache present it stale-serves `v1`. The dirty fetch is `rev=null`/uncacheable, so
the committed fetcher-cache row is never reused for it — the evaluator gets dirty content
either way. **The staleness lives entirely in the eval-trace cache**, which serves its
committed `#sub` trace for the dirty eval (via the two-phase mechanism: history-bootstrap on
the content-blind `stableRecoveryKey`, then primary hits on the promoted trace; the committed
`FileBytes` dep re-verifies against the still-present committed store path).

So the root cause is narrow, measured, and **on this branch**: eval-trace recovers/serves its
committed trace for a dirty eval instead of reflecting the dirty source the fetcher already
provides. It is NOT "dirty has no identity" (v5) and NOT a fetcher/source-addressing freeze
(v6).

## Fix direction (corrected — eval-trace-side; "uncacheable" and "fetcher freeze" both WITHDRAWN)

The fix is in eval-trace: make the dirty eval reflect the dirty source the fetcher already
content-addresses, so eval-trace stops matching its committed trace — exactly the behaviour
that already makes the PLAIN dirty case work (Experiment A: new content-addressed source path
per edit ⇒ FileBytes deps mismatch ⇒ re-record). The open implementation question is WHY the
plain case reflects the dirty source but the submodule case doesn't, given the fetcher
provides a dirty-distinct store path for both. Candidate gaps to instrument next:
- the dirty submodule eval's resolved-graph node `narHash` / `evaluationRoot` — does eval-trace
  key on the dirty-reflecting `tSWg…` or on a stale committed `OHEa…` pulled from the flake/
  metadata cache?
- the recorded `FileBytes` dep KEY for the submodule file — is it the dirty content-addressed
  store path (which would mismatch, like the plain case) or a carrier/mount logical path that
  is stable across dirty submodule content (which would re-verify green)?
- the content-blind `stableRecoveryKey` (original-ref-based) that lets history-bootstrap bridge
  the dirty eval back to the committed trace.

REJECTED fix directions (each with the measured reason):
- **"dirty ⇒ uncacheable under eval-trace" (v5): WITHDRAWN.** Experiment A: plain dirty flakes
  already work; disabling caching for all dirty flakes is a needless regression.
- **"Fetcher / source-addressing freeze" (v6): WITHDRAWN.** Fresh-fetch + which-cache prove the
  fetcher content-addresses dirty submodule content and that clearing the eval-trace cache
  alone fixes the serve. The fetcher is not the culprit.
- **"Gate history-bootstrap recovery" (v4): INSUFFICIENT** — Phase B is a primary hit no
  recovery gate touches (two-phase table above).

Finding 1's hash fix is a necessary correctness fix but orthogonal.

## Retractions (own them)

- v1: "computeFlakeSourceIdentity → computeLockedVersionIdentity (flake.cc:166-172
  dirtyRev stripping)" — WRONG path; `computeFlakeSourceIdentity` uses the ORIGINAL
  ref (`computeOriginalSourceIdentityHash`).
- v2: "the locked identity collapse (lockedVersionIdentity) is THE cause" — it IS
  collapsed (Finding 1), but fixing it does not fix the stale serve (Finding 2). So
  the identity collapse is necessary-not-sufficient, not the whole story.
- v3: "Finding 2 = a flake-source/carrier cache reuses the COMMITTED source store path
  for the dirty eval" — WRONG. The source is dirty (direct `evalFile` instrumentation);
  the reuse is in history-bootstrap RECOVERY, not source resolution. The "narHash
  identical → evaluates committed v1" inference was a gitlink misread.
- v5 (the FIX direction): "dirty ⇒ uncacheable under eval-trace." MEASURED-WRONG /
  WITHDRAWN. Experiment A shows a PLAIN dirty flake already works in eval-trace (new
  content-addressed source path + narHash per edit → correct invalidation), so disabling
  caching for all dirty flakes is a needless regression. I generalized from the submodule
  case to "dirty" without testing a plain dirty flake. Lesson (again): test the GENERAL
  claim ("dirty doesn't work") against a controlled case (plain dirty) before acting on it —
  I asserted a category limit from one special case.
- v6 (root cause + fix): "the dirty SUBMODULE flake source path / narHash is gitlink-frozen
  and doesn't reflect the evaluated content; fix it in the fetcher / source-addressing."
  MEASURED-WRONG / WITHDRAWN. The frozen `18f2…`/`OHEa…` was a `nix flake metadata` CACHE
  artifact (shared `XDG_CACHE_HOME`): a FRESH fetch content-addresses dirty submodule content
  correctly (narHash `XBtY→tSWg→zaTn`, store copy holds the dirty file). And clearing ONLY
  `eval-trace*.sqlite` (fetcher cache left warm) makes eval-trace serve the correct dirty
  value. So the freeze is the EVAL-TRACE cache, not the fetcher. Lesson: I read `path`/
  `narHash` from a metadata command sharing a warm cache and treated a cache hit as a fetcher
  fundamental — control the cache (fresh vs warm) and bisect the layers (which sqlite?) before
  attributing a freeze.
- v4 (the FIX direction, not the root cause): "exclude dirty from history-bootstrap
  recovery, or fold the dirty/locked identity into `stableRecoveryKey`." MEASURED-WRONG.
  The per-step counter table shows the stale serves after the first dirty edit are PRIMARY
  hits (`histBoot=0`) under a content-blind dirty key — a history-bootstrap gate doesn't
  touch them, and there is no content-distinguishing identity to fold for a dirty
  submodule. The sound fix is "dirty ⇒ uncacheable under eval-trace." I framed a fix
  direction from the first-dirty mechanism alone without testing dirty→dirty; the
  generalization (every dirty edit stale-serves via a content-blind key) only appeared
  once I ran successive dirty edits and read the counters per step.
- All wrong versions implied a single clean root cause and/or a tidy fix, and several
  were inferences (from narHash equality; from the first-dirty event alone) rather than
  direct measurements. Finding 2's MECHANISM is now fully pinned (two-phase: bootstrap
  Phase A + primary-hit Phase B, content-blind key, measured per-step) — but only after I
  stopped inferring and (a) instrumented the counters + `stableRecoveryKey` digest +
  `evalFile` read, and (b) ran the dirty→dirty chain end-to-end. Method lesson reaffirmed
  hard: measure end-to-end (which COUNTER fires? does dirty→dirty also break?) before
  declaring root cause OR fix; never infer from a proxy (narHash) or a single event when
  the full behavior is directly observable.
