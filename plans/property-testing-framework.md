# Property-Based Testing Framework — Remaining Work

Phase 1 and Phase 2 are fully landed. All P1–P6 property tests plus
additional invariants live in `src/libexpr-tests/eval-trace/property/`
(see `property/invariant/` for the per-property files).
`makeNixExprGen` includes `FromJSONGen`, `AttrAccessGen`,
`PathExistsGen`, and the nine nixpkgs-fidelity generators. The items
below are what still needs attention.

---

## 1. Known Limitations (still open)

### `simulateWarmRestart()` / `simulateColdProcess()` residual gaps

Updated 2026-04-29: the API named here used to be `simulateNewSession()`;
it was split into `simulateWarmRestart()` (mid-iteration-safe) and
`simulateColdProcess()` (wipes `positions` / `traceCtx` / cross-session
caches; not safe mid-iteration).  `StatHashStore` was removed as dead
code.  The residual limits are:

- **FS accessor cache**: clearing would require modifying `SourceAccessor`
  (public API). Tests that mutate files between sessions must call
  `invalidateFileCache(path)` per mutated path.
- **`EvalState::cacheFileContentHash`**: cleared by `simulateColdProcess`
  via `resetFileCache()`; `simulateWarmRestart` clears it via
  `clearCrossSessionCaches()`.  Neither clears it across RC iterations
  without an explicit callback.
- **`InterningPools`**: interned string IDs persist across sessions.

These are cross-session (same process) tests, not true cross-process tests.

### `EvalState` reuse across RC iterations

`cacheFileContentHash` can persist across 50–200 RapidCheck iterations,
potentially masking trace-system hash bugs, when neither helper is
invoked between iterations.

### GDP proof gap (reconciled)

`EvalTraceTest::withBs()` creates `Proof<BlockingTag>` unconditionally.
After the `ExclusiveTraceStoreAccess` capability refactor, `BlockingTag`
only certifies "I can block safely".  Exclusive store access is a
separate capability whose only factory is `TraceStore::withExclusiveAccess`.
Test threads that mint `bs` must still acquire `storeMutex_` via
`withExclusiveAccess` before touching store state, so the old
"unbounded forge via withBs" concurrency gap is closed at the
capability boundary.

---

## 2. Suspected Production Soundness Gaps

**Status (updated 2026-04-29):** of the original six-test list, only
`HasAttr.RemoveKey_Invalidates` is still actually skipped in the
code.  Plus one skipped concurrency test outside this list.

| Test | File | Current status |
|------|------|----------------|
| `HasAttr.DISABLED_RemoveKey_Invalidates` | `property/builtin/attrset/has-attr.cc` | `DISABLED_` — shared-fixture RC model can fail the pre-mutation warm-hit check even when deterministic `hasattr.cc` coverage passes.  Investigate or rewrite to run one generated case per fresh fixture / `EvalState`. |
| `ConcurrentEval.IndependentExpressions_NoCorruption` | `property/special/concurrent-eval.cc` | `GTEST_SKIP`'d — needs per-thread `EvalState` fixture variant.  Shared single `EvalState` has non-thread-safe GC heap, symbol table, and `activeSession_`. |

The other four tests originally listed here as `GTEST_SKIP`'d were
found to exist as live `TEST_F` bodies (not skipped):
- `TraceValueContext.DerivedAttribute_ValueDep_Invalidates` —
  `property/builtin/trace/value-context.cc`.
- `TraceValueContext.Concatenation_BothDeps_Invalidate` —
  `property/builtin/trace/value-context.cc`.
- `EqualityComparison.ScalarChange_Invalidates` —
  `property/builtin/comparison/equality.cc`.
- `ReadDirAttrAccess.ReadDir_MapAttrs_Soundness` —
  `property/coverage/readdir-attr-access.cc`.

`UpdateOperator.AccessedKeyChange_Invalidates` does not exist under
that name; the live test is `AccessedKeyChange_CorrectResult` in
`property/builtin/attrset/update-operator.cc`.

`DepInvalidation.RandomDepSlot` also does not exist at that path in
the current tree.

**Next step:** if the live tests above are still producing intermittent
failures that the original draft was tracking, assign them fresh
individual tracking entries with concrete reproduction evidence,
rather than carrying the pre-2026-04-29 list unchanged.

---

## 3. `ListLength.RemoveElement_Invalidates` (still open)

**Root cause:** `depKeySetCache` contamination across RC iterations in
the shared-DB model. A prior iteration's cached key set gets zipped with
a different trace's value blob.

**Fix options:**
- Per-iteration session recreation (call `makeCache` to recreate the session)
- `RC_PRE` guards ensuring structural consistency across iterations
- `maxSuccess = 1` with an outer loop that recreates the fixture

---

## 4. Cross-Session Property Tests (Track 2)

Uses `simulateWarmRestart()` (renamed from `simulateNewSession()`;
see §1).  Tests that mutate files between sessions must also call
`invalidateFileCache(path)` for each mutated path.
`cross-session-soundness.cc` covers the cold→warm case (S1); the other
three scenarios below are not yet implemented.

- **S2 Cross-Session Invalidation**: cold → `simulateWarmRestart()` →
  mutate dep → `invalidateFileCache` → warm → assert miss. Bug class:
  stale stat-hash entries surviving across sessions.
- **S3 Cross-Session Recovery**: cold v1 → `simulateWarmRestart()` → cold
  v2 → `simulateWarmRestart()` → revert to v1 → warm → assert recovery.
  Bug class: recovery history not persisted.
- **S4 Fingerprint Isolation**: two sessions with different fingerprints
  → traces don't leak.

---

## 5. CQK Coverage Gaps (C2)

Priority order by production frequency:

1. **GitRevisionIdentity** — `TempGitRepo` is ready. Generator evaluates
   expression under a git repo, tests commit hash change → invalidation.
2. **NarIdentity** — needs `FilterSourceGen` (TempDir-backed generator
   with filter function).
3. **DirectoryEntries** — RC generator creating `TempDir`, evaluating
   `builtins.readDir`, mutating directory contents.
4. **DummyStore-backed CQKs** (RuntimeFetchIdentity, DerivedStorePath,
   StorePathAvailability) — pre-populate `DummyStore.contents` before test.
5. **EnvironmentLookup invalidation** — standalone property test
   (separate from shared generator) to avoid shared-DB RC model flakiness.
6. **TraceValueContext precision** — test that changing an unrelated
   sibling does NOT invalidate a dependent trace.

| CQK | Prop S | Prop P | Gap |
|-----|--------|--------|-----|
| FileBytes | Yes | Yes | — |
| DirectoryEntries | Determ | Determ | RC gen needed |
| ExistenceCheck | Yes | Yes | — |
| EnvironmentLookup | Yes | — | No precision prop test |
| SessionSystemValue | — | — | Untestable in-process |
| RuntimeFetchIdentity | — | — | Pre-populate DummyStore |
| DerivedStorePath | — | — | Pre-populate DummyStore |
| VolatileExec | — | N/A | Always-miss by design |
| NarIdentity | — | — | FilterSourceGen + TempDir |
| StructuredProjection | Yes | Yes | — |
| ImplicitStructure | Yes | Yes | — |
| RawBytes | Yes | Partial | Whole-file by design |
| StorePathAvailability | — | — | Pre-populate DummyStore |
| GitRevisionIdentity | — | — | TempGitRepo ready |
| TraceValueContext | Determ | — | No precision prop test |
| TraceParentSlot | Determ | Determ | — |
| VolatileTime | — | N/A | Always-miss by design |

---

## 6. BUG-10 Float Regression Property (C3)

Extend `ScalarGen` to include float expressions. Add `RC_ASSERT` on
float equality with exact comparison (not epsilon) to catch
`std::to_chars` vs `std::to_string` precision loss through cache
round-trip.
